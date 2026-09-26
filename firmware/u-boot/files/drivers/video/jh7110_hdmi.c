// SPDX-License-Identifier: GPL-2.0+
/*
 * StarFive JH7110 HDMI framebuffer for U-Boot
 *
 * Brings the JH7110 display pipeline up from cold, with no firmware blob:
 *
 *   PMU PD_VOUT -> SYSCRG display clocks/resets -> VOUTCRG clocks/resets
 *   -> dom_vout_syscon routing (DC8200 panel 0 DPI -> HDMI, RGB888)
 *   -> Innosilicon HDMI controller init -> HDMI PHY pre-PLL (pixel clock)
 *   -> DC8200 timing, primary plane and DPI output -> HDMI mode setup
 *   -> HDMI PHY post-PLL and TMDS drivers.
 *
 * One fixed CEA mode: 1080p60 by default, 720p60 with hdmi_mode=720p or
 * CONFIG_JH7110_HDMI_DEFAULT_MODE="720p".  The DC8200 pixel clock comes
 * from the HDMI PHY, as in Linux; with hdmi_pixclk=pll2 or
 * CONFIG_JH7110_HDMI_PIXCLK_PLL2 it comes from PLL2, as in HFI BIOS.
 * The register values and their order follow the Linux JH7110 display
 * series (v4, September 2026: jh7110-vout-subsystem, jh7110-inno-hdmi,
 * phy-jh7110-inno-hdmi, inno-hdmi) and the upstream verisilicon DC driver.
 *
 * The DC8200 does not snoop the CPU caches.  The display controller is
 * given the framebuffer's real (below 4 GiB) address, and the CPU, the EFI
 * GOP and therefore the operating system use the SoC's uncached alias of
 * DRAM at +16 GiB, so every write reaches memory at once and no cache
 * maintenance is ever needed.  The real framebuffer RAM is marked
 * no-map/reserved in the device tree handed to the OS, which also puts it
 * in the EFI memory map as reserved.
 *
 * Copyright (c) 2026 the openbsd_hdmi_vf2 contributors
 */

#define LOG_CATEGORY UCLASS_VIDEO

#include <command.h>
#include <dm.h>
#include <env.h>
#include <event.h>
#include <fdtdec.h>
#include <log.h>
#include <video.h>
#include <asm/io.h>
#include <dm/ofnode.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/sizes.h>

/*
 * The JH7110 decodes DRAM twice: cached through the U74 front port at
 * [0x0_4000_0000, 0x2_4000_0000) and uncached through the system port at
 * [0x4_4000_0000, 0x6_4000_0000), i.e. physical address bit 34 set.  (This
 * is the map in the Linux "XPbmtUC" RFC, which uses exactly that bit.  The
 * 0x240000000 in U-Boot's JH7110 notes is where cached DRAM ends, not where
 * the alias starts.)
 */
#define JH7110_UNCACHED_OFFSET		0x400000000ULL

/* PMU */
#define PMU_SW_TURN_ON_POWER		0x0c
#define PMU_SW_ENCOURAGE		0x44
#define PMU_CURR_POWER_MODE		0x80
#define PMU_ENCOURAGE_ON		0xff
#define PMU_ENCOURAGE_EN_LO		0x05
#define PMU_ENCOURAGE_EN_HI		0x50
#define PMU_PD_VOUT			BIT(4)

/* SYSCRG: clock n at n * 4, bit 31 is the gate */
#define CLK_ENABLE			BIT(31)
#define SYSCLK_VOUT_SRC			58
#define SYSCLK_NOC_BUS_DISP_AXI		60
#define SYSCLK_VOUT_TOP_AHB		61
#define SYSCLK_VOUT_TOP_AXI		62
#define SYSCLK_VOUT_TOP_HDMITX0_MCLK	63
#define SYSCLK_I2STX0_BCLK		165
#define SYSCRG_RST_ASSERT		0x2f8
#define SYSCRG_RST_STATUS		0x308
#define SYSRST_NOC_BUS_DISP_AXI		26
#define SYSRST_VOUT_TOP_SRC		43

/* VOUTCRG */
#define VOUTCLK_APB			0
#define VOUTCLK_DC8200_PIX		1
#define VOUTCLK_DIV_MASK		GENMASK(23, 0)
#define VOUTCLK_DC8200_AXI		4
#define VOUTCLK_DC8200_CORE		5
#define VOUTCLK_DC8200_AHB		6
#define VOUTCLK_DC8200_PIX0		7
#define VOUTCLK_DC8200_PIX1		8
#define VOUTCLK_HDMI_TX_MCLK		15
#define VOUTCLK_HDMI_TX_BCLK		16
#define VOUTCLK_HDMI_TX_SYS		17
#define VOUTCLK_MUX_MASK		GENMASK(27, 24)
#define VOUTCLK_MUX_HDMI_PHY		BIT(24)
#define VOUTCRG_RST_ASSERT		0x48
#define VOUTCRG_RST_STATUS		0x4c
#define VOUTRST_DC8200_AXI		0
#define VOUTRST_DC8200_AHB		1
#define VOUTRST_DC8200_CORE		2
#define VOUTRST_HDMI_TX			9

/* dom_vout_syscon */
#define VOUT_SYSCFG_4			0x4
#define VOUT_HDMI_DP_BIT_DEPTH		BIT(25)
#define VOUT_HDMI_DP_YUV_MODE		GENMASK(27, 26)
#define VOUT_HDMI_DP_YUV_MODE_RGB	(3 << 26)
#define VOUT_HDMI_DPI_BIT_DEPTH		GENMASK(29, 28)
#define VOUT_HDMI_DPI_DP_SEL		BIT(30)
#define VOUT_SYSCFG_8			0x8
#define VOUT_HDMI_PANEL_SEL		BIT(4)

/* Innosilicon HDMI controller: 8-bit registers, 4 bytes apart */
#define HDMI_SYS_CTRL			0x00
#define  SYS_NOT_RST_ANALOG		BIT(6)
#define  SYS_NOT_RST_DIGITAL		BIT(5)
#define  SYS_REG_CLK_INV		BIT(4)
#define  SYS_REG_CLK_SOURCE_SYS		BIT(2)
#define  SYS_PWR_OFF			BIT(1)
#define  SYS_INT_POL_HIGH		BIT(0)
#define HDMI_VIDEO_CONTRL1		0x01
#define HDMI_VIDEO_CONTRL2		0x02
#define HDMI_VIDEO_CONTRL		0x03
#define HDMI_VIDEO_CONTRL3		0x04
#define HDMI_AV_MUTE			0x05
#define  AV_AUDIO_MUTE			BIT(1)
#define  AV_VIDEO_BLACK			BIT(0)
#define HDMI_VIDEO_TIMING_CTL		0x08
#define  TIMING_HSYNC_POSITIVE		BIT(3)
#define  TIMING_VSYNC_POSITIVE		BIT(2)
#define  TIMING_EXTERNAL_VIDEO		BIT(0)
#define HDMI_VIDEO_EXT_HTOTAL_L		0x09
#define HDMI_VIDEO_EXT_HBLANK_L		0x0b
#define HDMI_VIDEO_EXT_HDELAY_L		0x0d
#define HDMI_VIDEO_EXT_HDURATION_L	0x0f
#define HDMI_VIDEO_EXT_VTOTAL_L		0x11
#define HDMI_VIDEO_EXT_VBLANK		0x13
#define HDMI_VIDEO_EXT_VDELAY		0x14
#define HDMI_VIDEO_EXT_VDURATION	0x15
#define HDMI_DDC_BUS_FREQ_L		0x4b
#define HDMI_DDC_BUS_FREQ_H		0x4c
#define HDMI_HDCP_CTRL			0x52
#define HDMI_INTERRUPT_MASK1		0xc0
#define HDMI_INTERRUPT_STATUS1		0xc1
#define  INT_EDID_READY			BIT(2)
#define HDMI_STATUS			0xc8
#define  STATUS_HOTPLUG			BIT(7)
#define  STATUS_MASK_INT_HOTPLUG	BIT(5)
#define HDMI_PHY_SYNC			0xce
#define HDMI_PHY_SYS_CTL		0xe0
#define HDMI_PHY_CHG_PWR		0xe1
#define HDMI_PHY_DRIVER			0xe2
#define HDMI_PHY_PRE_EMPHASIS		0xe3
#define HDMI_PHY_FEEDBACK_DIV_LOW	0xe7
#define HDMI_PHY_FEEDBACK_DIV_HIGH	0xe8
#define HDMI_PHY_PRE_DIV_RATIO		0xed

/* Innosilicon HDMI PHY, in the same window at register index 0x100 */
#define PHY_PRE_PLL_CONTROL		0x1a0
#define  PRE_PLL_POWER_DOWN		BIT(0)
#define  PCLK_VCO_DIV_5			BIT(1)
#define PHY_PRE_PLL_DIV_1		0x1a1
#define PHY_PRE_PLL_DIV_2		0x1a2
#define  SPREAD_SPECTRUM_MOD_DISABLE	BIT(6)
#define  PRE_PLL_FRAC_DIV_DISABLE	(3 << 4)
#define PHY_PRE_PLL_DIV_3		0x1a3
#define PHY_PRE_PLL_TMDSCLK_DIV		0x1a4
#define PHY_PCLK_DIV_AB			0x1a5
#define PHY_PCLK_DIV_CD			0x1a6
#define PHY_PRE_PLL_LOCK_STATUS		0x1a9
#define PHY_POST_PLL_DIV_1		0x1aa
#define PHY_POST_PLL_DIV_2		0x1ab
#define PHY_POST_PLL_DIV_3		0x1ac
#define PHY_POST_PLL_DIV_4		0x1ad
#define PHY_POST_PLL_LOCK_STATUS	0x1af
#define PHY_BIAS_CONTROL		0x1b0
#define  BIAS_ENABLE			BIT(2)
#define PHY_TMDS_CONTROL		0x1b2
#define PHY_LDO_CONTROL			0x1b4
#define PHY_SERIALIZER_CONTROL		0x1be
#define PHY_RX_CONTROL			0x1cc
#define PHY_PRE_PLL_FRAC_DIV_23_16	0x1d1
#define PHY_PRE_PLL_FRAC_DIV_15_8	0x1d2
#define PHY_PRE_PLL_FRAC_DIV_7_0	0x1d3

/* VeriSilicon DC8200, output 0 */
#define DC_TOP_CHIP_MODEL		0x0020
#define DC_TOP_CHIP_REV			0x0024
#define DC_FB_ADDRESS			0x1400
#define DC_FB_STRIDE			0x1408
#define DC_DISP_PANEL_CONFIG		0x1418
#define  PANEL_DE_EN			BIT(0)
#define  PANEL_DE_POL			BIT(1)
#define  PANEL_DAT_EN			BIT(4)
#define  PANEL_DAT_POL			BIT(5)
#define  PANEL_CLK_EN			BIT(8)
#define  PANEL_CLK_POL			BIT(9)
#define  PANEL_RUNNING			BIT(12)
#define DC_DISP_HSIZE			0x1430
#define DC_DISP_HSYNC			0x1438
#define DC_DISP_VSIZE			0x1440
#define DC_DISP_VSYNC			0x1448
#define  SYNC_EN			BIT(30)
#define  SYNC_POL_NEGATIVE		BIT(31)
#define DC_DISP_DPI_CONFIG		0x14b8
#define  DPI_FMT_MASK			GENMASK(2, 0)
#define  DPI_FMT_RGB888			5
#define DC_FB_CONFIG			0x1518
#define  FB_CONFIG_FMT_MASK		GENMASK(31, 26)
#define  FB_CONFIG_FMT_X8R8G8B8		(5 << 26)
#define  FB_CONFIG_SWIZZLE_MASK		GENMASK(24, 23)
#define  FB_CONFIG_UV_SWIZZLE_EN	BIT(25)
#define DC_FB_SIZE			0x1810
#define DC_FB_CONFIG_EX			0x1cc0
#define  FB_EX_COMMIT			BIT(12)
#define  FB_EX_FB_EN			BIT(13)
#define  FB_EX_DISPLAY_ID		BIT(19)
#define DC_DISP_PANEL_START		0x1ccc
#define  PANEL_START_RUNNING0		BIT(0)
#define  PANEL_START_MULTI_DISP_SYNC	BIT(3)
#define DC_DISP_DP_CONFIG		0x1cd0
#define  DP_EN				BIT(3)
#define DC_FB_TOP_LEFT			0x24d8
#define DC_FB_BOTTOM_RIGHT		0x24e0
#define DC_FB_BLEND_CONFIG		0x2510
#define  BLEND_DISABLE			BIT(1)
#define DC_DISP_PANEL_CONFIG_EX		0x2518
#define  PANEL_EX_COMMIT		BIT(0)

#define FB_MAX_XSIZE			1920
#define FB_MAX_YSIZE			1080
#define FB_BYTES_PER_PIXEL		4

#define PLL_TIMEOUT_US			100000

/* PLL2 as the mainline SPL leaves it; feeds vout_src */
#define JH7110_PLL2_HZ			1188000000

struct jh7110_mode {
	const char *name;
	u32 clock;
	u16 hdisplay, hsync_start, hsync_end, htotal;
	u16 vdisplay, vsync_start, vsync_end, vtotal;
	u8 vic;
	/* pre-PLL: the Innosilicon table entry for this pixel clock */
	u8 prediv;
	u16 fbdiv;
	u8 tmds_a, tmds_b, tmds_c;
	u8 pclk_a, pclk_b, pclk_c, pclk_d;
};

/* CEA-861 modes, both +hsync/+vsync; PLL values from phy-jh7110-inno-hdmi */
static const struct jh7110_mode jh7110_modes[] = {
	{ "1080p", 148500000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125,
	  16, 1, 99, 1, 1, 1, 1, 2, 2, 2 },
	{ "720p", 74250000, 1280, 1390, 1430, 1650, 720, 725, 730, 750,
	  4, 1, 99, 1, 2, 2, 1, 2, 3, 4 },
};

struct jh7110_hdmi_priv {
	void __iomem *dc;
	void __iomem *hdmi;
	void __iomem *voutsys;
	void __iomem *voutcrg;
	void __iomem *syscrg;
	void __iomem *pmu;
	const struct jh7110_mode *mode;
	bool pixclk_pll2;
	ulong fb_phys;
	ulong fb_size;
	bool up;
};

/* Framebuffer RAM to keep away from the OS (see the EVT_FT_FIXUP spy) */
static ulong jh7110_fb_phys;
static ulong jh7110_fb_size;
static struct jh7110_hdmi_priv *jh7110_active;

static void hdmi_write(struct jh7110_hdmi_priv *p, u16 reg, u8 val)
{
	writel(val, p->hdmi + reg * 4);
}

static u8 hdmi_read(struct jh7110_hdmi_priv *p, u16 reg)
{
	return readl(p->hdmi + reg * 4) & 0xff;
}

static void hdmi_modify(struct jh7110_hdmi_priv *p, u16 reg, u8 mask, u8 val)
{
	hdmi_write(p, reg, (hdmi_read(p, reg) & ~mask) | (val & mask));
}

static int hdmi_poll(struct jh7110_hdmi_priv *p, u16 reg, u8 bit)
{
	u32 val;

	return readl_poll_timeout(p->hdmi + reg * 4, val, val & bit,
				  PLL_TIMEOUT_US);
}

static void jh_clk_gate(void __iomem *crg, unsigned int id, bool on)
{
	if (on)
		setbits_le32(crg + id * 4, CLK_ENABLE);
	else
		clrbits_le32(crg + id * 4, CLK_ENABLE);
}

/* JH7110 reset status reads 1 once a line is deasserted */
static int jh_reset_deassert(void __iomem *crg, u32 assert_off, u32 status_off,
			  unsigned int id)
{
	u32 bank = (id / 32) * 4, bit = BIT(id % 32), val;

	clrbits_le32(crg + assert_off + bank, bit);
	return readl_poll_timeout(crg + status_off + bank, val, val & bit,
				  10000);
}

static int jh7110_power_vout(struct jh7110_hdmi_priv *p)
{
	u32 val;

	if (readl(p->pmu + PMU_CURR_POWER_MODE) & PMU_PD_VOUT)
		return 0;

	writel(PMU_PD_VOUT, p->pmu + PMU_SW_TURN_ON_POWER);
	writel(PMU_ENCOURAGE_ON, p->pmu + PMU_SW_ENCOURAGE);
	writel(PMU_ENCOURAGE_EN_LO, p->pmu + PMU_SW_ENCOURAGE);
	writel(PMU_ENCOURAGE_EN_HI, p->pmu + PMU_SW_ENCOURAGE);

	return readl_poll_timeout(p->pmu + PMU_CURR_POWER_MODE, val,
				  val & PMU_PD_VOUT, 100000);
}

/*
 * Everything that must be running before any register in the 0x29400000
 * display window may be touched: an unclocked or unpowered access there
 * wedges the bus.
 */
static int jh7110_vout_enable(struct jh7110_hdmi_priv *p)
{
	u32 val;
	int ret;

	ret = jh7110_power_vout(p);
	if (ret) {
		log_err("jh7110-hdmi: PD_VOUT did not power up\n");
		return ret;
	}

	/* jh7110-vout-subsystem: NoC display bus clock, then its reset */
	jh_clk_gate(p->syscrg, SYSCLK_NOC_BUS_DISP_AXI, true);
	ret = jh_reset_deassert(p->syscrg, SYSCRG_RST_ASSERT, SYSCRG_RST_STATUS,
			     SYSRST_NOC_BUS_DISP_AXI);
	if (ret) {
		log_err("jh7110-hdmi: NoC display bus reset stuck\n");
		return ret;
	}

	/* voutcrg: its parent clocks and the shared top reset */
	jh_clk_gate(p->syscrg, SYSCLK_VOUT_SRC, true);
	jh_clk_gate(p->syscrg, SYSCLK_VOUT_TOP_AHB, true);
	jh_clk_gate(p->syscrg, SYSCLK_VOUT_TOP_AXI, true);
	jh_clk_gate(p->syscrg, SYSCLK_VOUT_TOP_HDMITX0_MCLK, true);
	/* HFI's VideoBIOS ungates this too; it feeds hdmi_tx_bclk */
	jh_clk_gate(p->syscrg, SYSCLK_I2STX0_BCLK, true);
	ret = jh_reset_deassert(p->syscrg, SYSCRG_RST_ASSERT, SYSCRG_RST_STATUS,
			     SYSRST_VOUT_TOP_SRC);
	if (ret) {
		log_err("jh7110-hdmi: VOUT top reset stuck\n");
		return ret;
	}

	/*
	 * The DC8200 pixel clocks.  By default both take the HDMI PHY's
	 * pixel clock (assigned-clock-parents in the Linux device tree), so
	 * pixel data and TMDS share one PLL.  With hdmi_pixclk=pll2 they take
	 * vout_src (PLL2) divided down to the pixel rate instead, which is
	 * what HFI BIOS's VideoBIOS does.  The gates stay off until the
	 * clock is running.
	 */
	if (p->pixclk_pll2) {
		val = readl(p->voutcrg + VOUTCLK_DC8200_PIX * 4);
		val = (val & ~VOUTCLK_DIV_MASK) |
		      DIV_ROUND_CLOSEST(JH7110_PLL2_HZ, p->mode->clock);
		writel(val, p->voutcrg + VOUTCLK_DC8200_PIX * 4);
	}
	val = readl(p->voutcrg + VOUTCLK_DC8200_PIX0 * 4);
	val &= ~(VOUTCLK_MUX_MASK | CLK_ENABLE);
	if (!p->pixclk_pll2)
		val |= VOUTCLK_MUX_HDMI_PHY;
	writel(val, p->voutcrg + VOUTCLK_DC8200_PIX0 * 4);
	val = readl(p->voutcrg + VOUTCLK_DC8200_PIX1 * 4);
	val &= ~(VOUTCLK_MUX_MASK | CLK_ENABLE);
	if (!p->pixclk_pll2)
		val |= VOUTCLK_MUX_HDMI_PHY;
	writel(val, p->voutcrg + VOUTCLK_DC8200_PIX1 * 4);

	/* HDMI controller: pclk (hdmi_tx_sys), mclk, bclk, then its reset */
	jh_clk_gate(p->voutcrg, VOUTCLK_HDMI_TX_SYS, true);
	jh_clk_gate(p->voutcrg, VOUTCLK_HDMI_TX_MCLK, true);
	jh_clk_gate(p->voutcrg, VOUTCLK_HDMI_TX_BCLK, true);
	ret = jh_reset_deassert(p->voutcrg, VOUTCRG_RST_ASSERT,
			     VOUTCRG_RST_STATUS, VOUTRST_HDMI_TX);
	if (ret) {
		log_err("jh7110-hdmi: HDMI controller reset stuck\n");
		return ret;
	}

	/* DC8200: core, axi, ahb clocks, then its three resets */
	jh_clk_gate(p->voutcrg, VOUTCLK_DC8200_CORE, true);
	jh_clk_gate(p->voutcrg, VOUTCLK_DC8200_AXI, true);
	jh_clk_gate(p->voutcrg, VOUTCLK_DC8200_AHB, true);
	ret = jh_reset_deassert(p->voutcrg, VOUTCRG_RST_ASSERT,
			     VOUTCRG_RST_STATUS, VOUTRST_DC8200_CORE);
	ret = ret ?: jh_reset_deassert(p->voutcrg, VOUTCRG_RST_ASSERT,
				    VOUTCRG_RST_STATUS, VOUTRST_DC8200_AXI);
	ret = ret ?: jh_reset_deassert(p->voutcrg, VOUTCRG_RST_ASSERT,
				    VOUTCRG_RST_STATUS, VOUTRST_DC8200_AHB);
	if (ret) {
		log_err("jh7110-hdmi: DC8200 reset stuck\n");
		return ret;
	}

	return 0;
}

/* jh7110-inno-hdmi: DC8200 panel 0, DPI, 8-bit RGB into the transmitter */
static void jh7110_route_panel0(struct jh7110_hdmi_priv *p)
{
	clrsetbits_le32(p->voutsys + VOUT_SYSCFG_4,
			VOUT_HDMI_DPI_DP_SEL | VOUT_HDMI_DP_BIT_DEPTH |
			VOUT_HDMI_DP_YUV_MODE | VOUT_HDMI_DPI_BIT_DEPTH,
			VOUT_HDMI_DP_YUV_MODE_RGB);
	clrbits_le32(p->voutsys + VOUT_SYSCFG_8, VOUT_HDMI_PANEL_SEL);
}

static void hdmi_ddc_rate(struct jh7110_hdmi_priv *p, ulong rate)
{
	ulong div = (rate >> 2) / 100000;

	hdmi_write(p, HDMI_DDC_BUS_FREQ_L, div & 0xff);
	hdmi_write(p, HDMI_DDC_BUS_FREQ_H, (div >> 8) & 0xff);
	hdmi_write(p, HDMI_INTERRUPT_MASK1, 0);
	hdmi_write(p, HDMI_INTERRUPT_STATUS1, INT_EDID_READY);
}

/* inno_hdmi_init_hw() */
static void jh7110_hdmi_init_hw(struct jh7110_hdmi_priv *p)
{
	hdmi_modify(p, HDMI_SYS_CTRL, SYS_NOT_RST_DIGITAL, SYS_NOT_RST_DIGITAL);
	udelay(150);
	hdmi_modify(p, HDMI_SYS_CTRL, SYS_NOT_RST_ANALOG, SYS_NOT_RST_ANALOG);
	udelay(150);

	/*
	 * Register clock from the system clock, inverted, power on (0x75),
	 * as Linux does from here on.  HFI BIOS instead keeps the TMDS clock
	 * as register clock and only writes SYS_CTRL once the PHY PLLs have
	 * locked; with the system clock the order does not matter.
	 */
	hdmi_modify(p, HDMI_SYS_CTRL,
		    SYS_REG_CLK_INV | SYS_REG_CLK_SOURCE_SYS | SYS_PWR_OFF |
		    SYS_INT_POL_HIGH,
		    SYS_REG_CLK_INV | SYS_REG_CLK_SOURCE_SYS | SYS_INT_POL_HIGH);

	/* inno_hdmi_standby() */
	hdmi_modify(p, HDMI_SYS_CTRL, SYS_PWR_OFF, SYS_PWR_OFF);
	hdmi_write(p, HDMI_PHY_DRIVER, 0x00);
	hdmi_write(p, HDMI_PHY_PRE_EMPHASIS, 0x00);
	hdmi_write(p, HDMI_PHY_CHG_PWR, 0x00);
	hdmi_write(p, HDMI_PHY_SYS_CTL, 0x15);

	hdmi_ddc_rate(p, p->mode->clock);
	hdmi_modify(p, HDMI_STATUS, STATUS_MASK_INT_HOTPLUG,
		    STATUS_MASK_INT_HOTPLUG);
}

/* phy-jh7110-inno-hdmi .set_rate, then .prepare (wait for lock) */
static int jh7110_phy_pixel_clock(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;

	hdmi_modify(p, PHY_BIAS_CONTROL, BIAS_ENABLE, BIAS_ENABLE);
	hdmi_write(p, PHY_RX_CONTROL, 0x0f);

	hdmi_modify(p, PHY_PRE_PLL_CONTROL, PRE_PLL_POWER_DOWN,
		    PRE_PLL_POWER_DOWN);
	hdmi_modify(p, PHY_PRE_PLL_CONTROL, PCLK_VCO_DIV_5, 0);
	hdmi_write(p, PHY_PRE_PLL_DIV_1, m->prediv & 0x3f);
	hdmi_write(p, PHY_PRE_PLL_DIV_2, SPREAD_SPECTRUM_MOD_DISABLE |
		   PRE_PLL_FRAC_DIV_DISABLE | ((m->fbdiv >> 8) & 0x0f));
	hdmi_write(p, PHY_PRE_PLL_DIV_3, m->fbdiv & 0xff);
	hdmi_write(p, PHY_PCLK_DIV_AB, (m->pclk_a & 0x1f) |
		   ((m->pclk_b & 3) << 5));
	hdmi_write(p, PHY_PCLK_DIV_CD, (m->pclk_d & 0x1f) |
		   ((m->pclk_c & 3) << 5));
	hdmi_write(p, PHY_PRE_PLL_TMDSCLK_DIV, (m->tmds_c & 3) |
		   ((m->tmds_b & 3) << 2) | ((m->tmds_a & 3) << 4));
	hdmi_write(p, PHY_PRE_PLL_FRAC_DIV_7_0, 0);
	hdmi_write(p, PHY_PRE_PLL_FRAC_DIV_15_8, 0);
	hdmi_write(p, PHY_PRE_PLL_FRAC_DIV_23_16, 0);
	hdmi_modify(p, PHY_PRE_PLL_CONTROL, PRE_PLL_POWER_DOWN, 0);

	if (hdmi_poll(p, PHY_PRE_PLL_LOCK_STATUS, BIT(0))) {
		log_err("jh7110-hdmi: HDMI PHY pre-PLL did not lock at %u Hz\n", m->clock);
		return -ETIMEDOUT;
	}

	return 0;
}

/* phy-jh7110-inno-hdmi .power_on: post-PLL (prediv 1, fbdiv 20, postdiv 1) */
static int jh7110_phy_power_on(struct jh7110_hdmi_priv *p)
{
	hdmi_modify(p, PHY_BIAS_CONTROL, BIAS_ENABLE, BIAS_ENABLE);
	hdmi_write(p, PHY_RX_CONTROL, 0x0f);
	hdmi_write(p, PHY_POST_PLL_DIV_2, 1);
	hdmi_write(p, PHY_POST_PLL_DIV_3, 20);
	hdmi_write(p, PHY_POST_PLL_DIV_4, 1);
	hdmi_write(p, PHY_POST_PLL_DIV_1, 0x0e);

	if (hdmi_poll(p, PHY_POST_PLL_LOCK_STATUS, BIT(0))) {
		log_err("jh7110-hdmi: HDMI PHY post-PLL did not lock\n");
		return -ETIMEDOUT;
	}

	hdmi_write(p, PHY_LDO_CONTROL, 0x07);
	hdmi_write(p, PHY_SERIALIZER_CONTROL, 0x71);
	hdmi_write(p, PHY_TMDS_CONTROL, 0x8f);

	return 0;
}

static void jh7110_dc_timing(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;

	writel(m->hdisplay | (m->htotal << 16), p->dc + DC_DISP_HSIZE);
	writel(m->vdisplay | (m->vtotal << 16), p->dc + DC_DISP_VSIZE);
	writel(m->hsync_start | (m->hsync_end << 15) | SYNC_EN,
	       p->dc + DC_DISP_HSYNC);
	writel(m->vsync_start | (m->vsync_end << 15) | SYNC_EN,
	       p->dc + DC_DISP_VSYNC);
}

static void jh7110_dc_plane(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;
	u32 pos_br = (m->hdisplay & 0x7fff) | ((m->vdisplay & 0x7fff) << 15);

	clrsetbits_le32(p->dc + DC_FB_CONFIG,
			FB_CONFIG_FMT_MASK | FB_CONFIG_SWIZZLE_MASK |
			FB_CONFIG_UV_SWIZZLE_EN, FB_CONFIG_FMT_X8R8G8B8);
	writel(lower_32_bits(p->fb_phys), p->dc + DC_FB_ADDRESS);
	writel(m->hdisplay * FB_BYTES_PER_PIXEL, p->dc + DC_FB_STRIDE);
	writel(0, p->dc + DC_FB_TOP_LEFT);
	writel(pos_br, p->dc + DC_FB_BOTTOM_RIGHT);
	writel(pos_br, p->dc + DC_FB_SIZE);
	writel(BLEND_DISABLE, p->dc + DC_FB_BLEND_CONFIG);
	clrsetbits_le32(p->dc + DC_FB_CONFIG_EX, FB_EX_DISPLAY_ID,
			FB_EX_FB_EN | FB_EX_COMMIT);
}

/* vs_bridge_atomic_enable_dpi() */
static void jh7110_dc_output(struct jh7110_hdmi_priv *p)
{
	clrbits_le32(p->dc + DC_DISP_DP_CONFIG, DP_EN);
	writel(DPI_FMT_RGB888, p->dc + DC_DISP_DPI_CONFIG);
	clrbits_le32(p->dc + DC_DISP_PANEL_CONFIG,
		     PANEL_DAT_POL | PANEL_DE_POL | PANEL_CLK_POL);
	setbits_le32(p->dc + DC_DISP_PANEL_CONFIG,
		     PANEL_DE_EN | PANEL_DAT_EN | PANEL_CLK_EN);
	setbits_le32(p->dc + DC_DISP_PANEL_CONFIG, PANEL_RUNNING);
	clrbits_le32(p->dc + DC_DISP_PANEL_START, PANEL_START_MULTI_DISP_SYNC);
	setbits_le32(p->dc + DC_DISP_PANEL_START, PANEL_START_RUNNING0);
	setbits_le32(p->dc + DC_DISP_PANEL_CONFIG_EX, PANEL_EX_COMMIT);
}

/*
 * inno_hdmi_setup() for 8-bit full-range RGB, in DVI mode: without an EDID
 * to say the sink is HDMI, that is what Linux does too, and it is what HFI
 * BIOS uses.  Every HDMI sink accepts it, DVI sinks behind an adapter as
 * well, and a framebuffer console needs neither audio nor InfoFrames.
 */
static int jh7110_hdmi_setup(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;
	int ret;

	hdmi_modify(p, HDMI_AV_MUTE, AV_AUDIO_MUTE | AV_VIDEO_BLACK,
		    AV_AUDIO_MUTE | AV_VIDEO_BLACK);
	hdmi_write(p, HDMI_HDCP_CTRL, 0);	/* DVI: no data islands */

	ret = jh7110_phy_power_on(p);
	if (ret)
		return ret;

	hdmi_write(p, HDMI_VIDEO_TIMING_CTL, TIMING_EXTERNAL_VIDEO |
		   TIMING_HSYNC_POSITIVE | TIMING_VSYNC_POSITIVE);
	hdmi_write(p, HDMI_VIDEO_EXT_HTOTAL_L, m->htotal & 0xff);
	hdmi_write(p, HDMI_VIDEO_EXT_HTOTAL_L + 1, m->htotal >> 8);
	hdmi_write(p, HDMI_VIDEO_EXT_HBLANK_L, (m->htotal - m->hdisplay) & 0xff);
	hdmi_write(p, HDMI_VIDEO_EXT_HBLANK_L + 1,
		   (m->htotal - m->hdisplay) >> 8);
	hdmi_write(p, HDMI_VIDEO_EXT_HDELAY_L,
		   (m->htotal - m->hsync_start) & 0xff);
	hdmi_write(p, HDMI_VIDEO_EXT_HDELAY_L + 1,
		   (m->htotal - m->hsync_start) >> 8);
	hdmi_write(p, HDMI_VIDEO_EXT_HDURATION_L,
		   (m->hsync_end - m->hsync_start) & 0xff);
	hdmi_write(p, HDMI_VIDEO_EXT_HDURATION_L + 1,
		   (m->hsync_end - m->hsync_start) >> 8);
	hdmi_write(p, HDMI_VIDEO_EXT_VTOTAL_L, m->vtotal & 0xff);
	hdmi_write(p, HDMI_VIDEO_EXT_VTOTAL_L + 1, m->vtotal >> 8);
	hdmi_write(p, HDMI_VIDEO_EXT_VBLANK, m->vtotal - m->vdisplay);
	hdmi_write(p, HDMI_VIDEO_EXT_VDELAY, m->vtotal - m->vsync_start);
	hdmi_write(p, HDMI_VIDEO_EXT_VDURATION, m->vsync_end - m->vsync_start);
	hdmi_write(p, HDMI_PHY_PRE_DIV_RATIO, 0x1e);
	hdmi_write(p, HDMI_PHY_FEEDBACK_DIV_LOW, 0x2c);
	hdmi_write(p, HDMI_PHY_FEEDBACK_DIV_HIGH, 0x01);

	/* SDR RGB444 input, external DE; 8-bit RGB out; no CSC */
	hdmi_write(p, HDMI_VIDEO_CONTRL1, 0x01);
	hdmi_write(p, HDMI_VIDEO_CONTRL2, 0x30);
	hdmi_write(p, HDMI_VIDEO_CONTRL3, 0x18);
	hdmi_modify(p, HDMI_VIDEO_CONTRL, BIT(7) | BIT(0), BIT(0));

	hdmi_ddc_rate(p, m->clock);

	hdmi_modify(p, HDMI_AV_MUTE, AV_AUDIO_MUTE | AV_VIDEO_BLACK, 0);

	/* inno_hdmi_power_up() without a PHY table on the JH7110 */
	hdmi_modify(p, HDMI_SYS_CTRL, SYS_PWR_OFF, SYS_PWR_OFF);
	hdmi_modify(p, HDMI_SYS_CTRL, SYS_PWR_OFF, 0);

	return 0;
}

static const struct jh7110_mode *jh7110_pick_mode(void)
{
	const char *want = env_get("hdmi_mode") ?: CONFIG_JH7110_HDMI_DEFAULT_MODE;
	int i;

	if (*want) {
		for (i = 0; i < ARRAY_SIZE(jh7110_modes); i++)
			if (!strcmp(want, jh7110_modes[i].name))
				return &jh7110_modes[i];
		log_warning("jh7110-hdmi: hdmi_mode=%s unknown, using %s\n", want,
			    jh7110_modes[0].name);
	}

	return &jh7110_modes[0];
}

static int jh7110_hdmi_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct jh7110_hdmi_priv *p = dev_get_priv(dev);
	u32 model;
	int ret;

	p->mode = jh7110_pick_mode();
	p->pixclk_pll2 = IS_ENABLED(CONFIG_JH7110_HDMI_PIXCLK_PLL2);
	if (env_get("hdmi_pixclk"))
		p->pixclk_pll2 = !strcmp(env_get("hdmi_pixclk"), "pll2");
	p->fb_phys = plat->base;
	p->fb_size = plat->size;

	if (p->fb_phys + p->fb_size > SZ_4G) {
		log_err("jh7110-hdmi: framebuffer at %#lx is beyond the DC8200's reach\n",
			p->fb_phys);
		return -EINVAL;
	}

	ret = jh7110_vout_enable(p);
	if (ret)
		return ret;

	model = readl(p->dc + DC_TOP_CHIP_MODEL);
	if (model != 0x8200)
		log_warning("jh7110-hdmi: unexpected display controller model %#x\n", model);

	jh7110_route_panel0(p);
	jh7110_hdmi_init_hw(p);

	/* vs_crtc mode_set_nofb: timing, then the pixel clock rate */
	jh7110_dc_timing(p);
	ret = jh7110_phy_pixel_clock(p);
	if (ret)
		return ret;

	/* vs_crtc atomic_enable: ungate pix0 (mux already on the PHY) */
	jh_clk_gate(p->voutcrg, VOUTCLK_DC8200_PIX0, true);

	/* Scan out black until U-Boot draws; clear through the alias */
	memset((void *)(uintptr_t)(p->fb_phys + JH7110_UNCACHED_OFFSET), 0,
	       p->fb_size);
	jh7110_dc_plane(p);
	jh7110_dc_output(p);

	ret = jh7110_hdmi_setup(p);
	if (ret)
		return ret;

	uc_priv->xsize = p->mode->hdisplay;
	uc_priv->ysize = p->mode->vdisplay;
	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;
	uc_priv->line_length = p->mode->hdisplay * FB_BYTES_PER_PIXEL;

	/*
	 * From here on the CPU, U-Boot's console, the EFI GOP and the OS use
	 * the uncached alias.  Nothing needs flushing.
	 */
	plat->base = p->fb_phys + JH7110_UNCACHED_OFFSET;
	video_set_flush_dcache(dev, false);

	jh7110_fb_phys = p->fb_phys;
	jh7110_fb_size = p->fb_size;
	jh7110_active = p;
	p->up = true;

	log_info("jh7110-hdmi: %ux%u@60, pixel clock from %s, fb %#lx (CPU view %#llx), DC%x rev %x\n",
		 p->mode->hdisplay, p->mode->vdisplay,
		 p->pixclk_pll2 ? "PLL2" : "HDMI PHY", p->fb_phys,
		 (unsigned long long)plat->base, model,
		 readl(p->dc + DC_TOP_CHIP_REV));

	return 0;
}

static int jh7110_hdmi_of_to_plat(struct udevice *dev)
{
	struct jh7110_hdmi_priv *p = dev_get_priv(dev);

	p->dc = dev_read_addr_name_ptr(dev, "dc");
	p->hdmi = dev_read_addr_name_ptr(dev, "hdmi");
	p->voutsys = dev_read_addr_name_ptr(dev, "vout-syscon");
	p->voutcrg = dev_read_addr_name_ptr(dev, "voutcrg");
	p->syscrg = dev_read_addr_name_ptr(dev, "syscrg");
	p->pmu = dev_read_addr_name_ptr(dev, "pmu");
	if (!p->dc || !p->hdmi || !p->voutsys || !p->voutcrg || !p->syscrg ||
	    !p->pmu)
		return -EINVAL;

	return 0;
}

static int jh7110_hdmi_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	/* Room for the largest mode, reserved before relocation */
	plat->size = FB_MAX_XSIZE * FB_MAX_YSIZE * FB_BYTES_PER_PIXEL;

	return 0;
}

static const struct udevice_id jh7110_hdmi_ids[] = {
	{ .compatible = "openbsd-hdmi-vf2,jh7110-hdmi-fb" },
	{ }
};

U_BOOT_DRIVER(jh7110_hdmi) = {
	.name		= "jh7110_hdmi",
	.id		= UCLASS_VIDEO,
	.of_match	= jh7110_hdmi_ids,
	.bind		= jh7110_hdmi_bind,
	.of_to_plat	= jh7110_hdmi_of_to_plat,
	.probe		= jh7110_hdmi_probe,
	.priv_auto	= sizeof(struct jh7110_hdmi_priv),
	.flags		= DM_FLAG_PRE_RELOC,
};

/*
 * Keep the OS away from the framebuffer RAM.  This lands in the device
 * tree the OS receives and, through efi_carve_out_dt_rsv(), in the EFI
 * memory map as reserved.
 */
static int jh7110_hdmi_ft_fixup(void *ctx, struct event *event)
{
	void *blob = oftree_lookup_fdt(event->data.ft_fixup.tree);
	struct fdt_memory fb;
	int ret;

	if (!jh7110_fb_size || !blob)
		return 0;

	fb.start = jh7110_fb_phys;
	fb.end = jh7110_fb_phys + jh7110_fb_size - 1;
	ret = fdtdec_add_reserved_memory(blob, "framebuffer", &fb, NULL, 0,
					 NULL, FDTDEC_RESERVED_MEMORY_NO_MAP);
	if (ret)
		log_err("jh7110-hdmi: could not reserve the framebuffer for the OS: %d\n",
			ret);

	return 0;
}
EVENT_SPY_FULL(EVT_FT_FIXUP, jh7110_hdmi_ft_fixup);

#if IS_ENABLED(CONFIG_CMD_JH7110_HDMI)
static void dump_regs(const char *name, void __iomem *base, const u32 *offs,
		      int n, int stride)
{
	int i;

	printf("%s:\n", name);
	for (i = 0; i < n; i++)
		printf("  +%#06x = %#010x\n", offs[i] * stride,
		       readl(base + offs[i] * stride));
}

static int do_hdmiregs(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	static const u32 syscrg[] = { 58, 59, 60, 61, 62, 63, 165 };
	static const u32 voutcrg[] = { 0, 1, 4, 5, 6, 7, 8, 15, 16, 17 };
	static const u32 voutsys[] = { 1, 2 };
	static const u32 dc[] = { 0x0020, 0x0024, 0x1400, 0x1408, 0x1418,
		0x1430, 0x1438, 0x1440, 0x1448, 0x14b8, 0x1518, 0x1810,
		0x1cc0, 0x1ccc, 0x1cd0, 0x24d8, 0x24e0, 0x2510, 0x2518 };
	static const u32 hdmi[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08,
		0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
		0x13, 0x14, 0x15, 0x52, 0xc8, 0xce, 0xe0, 0xe1, 0xe2, 0xe3,
		0xe7, 0xe8, 0xed, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a4, 0x1a5,
		0x1a6, 0x1a9, 0x1aa, 0x1ab, 0x1ac, 0x1ad, 0x1af, 0x1b0, 0x1b2,
		0x1b4, 0x1be, 0x1cc };
	struct jh7110_hdmi_priv *p = jh7110_active;
	int i;

	/* Only read the display window while it is powered and clocked */
	if (!p || !p->up) {
		printf("display pipeline is not up; not touching it\n");
		return CMD_RET_FAILURE;
	}

	dump_regs("syscrg clocks", p->syscrg, syscrg, ARRAY_SIZE(syscrg), 4);
	printf("syscrg resets: %#010x %#010x (status %#010x %#010x)\n",
	       readl(p->syscrg + SYSCRG_RST_ASSERT),
	       readl(p->syscrg + SYSCRG_RST_ASSERT + 4),
	       readl(p->syscrg + SYSCRG_RST_STATUS),
	       readl(p->syscrg + SYSCRG_RST_STATUS + 4));
	dump_regs("voutcrg clocks", p->voutcrg, voutcrg, ARRAY_SIZE(voutcrg),
		  4);
	printf("voutcrg resets: %#010x (status %#010x)\n",
	       readl(p->voutcrg + VOUTCRG_RST_ASSERT),
	       readl(p->voutcrg + VOUTCRG_RST_STATUS));
	dump_regs("vout syscon", p->voutsys, voutsys, ARRAY_SIZE(voutsys), 4);
	dump_regs("dc8200", p->dc, dc, ARRAY_SIZE(dc), 1);
	printf("hdmi (register index: value):\n");
	for (i = 0; i < ARRAY_SIZE(hdmi); i++)
		printf("  %#05x: %#04x\n", hdmi[i], hdmi_read(p, hdmi[i]));

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(hdmiregs, 1, 0, do_hdmiregs,
	   "dump the JH7110 display pipeline registers",
	   "");
#endif
