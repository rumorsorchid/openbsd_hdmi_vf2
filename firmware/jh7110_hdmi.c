// SPDX-License-Identifier: GPL-2.0+
/*
 * StarFive JH7110 HDMI framebuffer for U-Boot
 *
 * Brings the JH7110 display pipeline up from cold, with no firmware blob:
 *
 *   PMU PD_VOUT -> SYSCRG display clocks/resets -> VOUTCRG clocks/resets
 *   -> dom_vout_syscon routing (DC8200 panel 0 DPI into the transmitter)
 *   -> HDMI PHY pre- and post-PLL -> Innosilicon HDMI transmitter
 *   -> DC8200 timing, primary plane and DPI output.
 *
 * The transmitter is brought up completely before the display controller
 * starts, with the register sequence of StarFive's own drivers (U-Boot
 * drivers/video/starfive/sf_hdmi.c, Linux drivers/gpu/drm/verisilicon/
 * inno_hdmi.c), the sequences proven on this SoC from cold.  The DC8200 is
 * programmed the way the upstream Linux verisilicon driver does it, with
 * the values StarFive's U-Boot writes for 1080p.
 *
 * One fixed CEA mode: 1080p60 by default, 720p60 with hdmi_mode=720p or
 * CONFIG_JH7110_HDMI_DEFAULT_MODE="720p".  The DC8200 pixel clock comes
 * from the HDMI PHY, as in StarFive's Linux driver; with hdmi_pixclk=pll2
 * or CONFIG_JH7110_HDMI_PIXCLK_PLL2 it comes from PLL2, as in StarFive's
 * U-Boot and in HFI BIOS.  The transmitter runs in DVI mode, as HFI BIOS
 * does: every HDMI sink accepts it, and a framebuffer console needs
 * neither audio nor InfoFrames.
 *
 * The DC8200 does not snoop the CPU caches.  See jh7110_find_uncached()
 * for how the framebuffer stays coherent without cache maintenance.
 *
 * Copyright (c) 2026 the openbsd_hdmi_vf2 contributors
 */

#define LOG_CATEGORY UCLASS_VIDEO

#include <command.h>
#include <dm.h>
#include <env.h>
#include <event.h>
#include <fdtdec.h>
#include <interrupt.h>
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
 * The JH7110's cached DRAM window is [0x0_4000_0000, 0x4_4000_0000): the
 * 16 GiB that StarFive's own U-Boot flushes through the L2 controller
 * (STARFIVE_JH7110_L2CC_FLUSH_START/SIZE).  The same DRAM is decoded a
 * second time above it, uncached: physical address bit 34 set.  U-Boot's
 * doc/board/starfive notes place the uncached view at DRAM + 8 GiB
 * instead, which on an 8 GiB board is still inside the cached window.
 * Neither is taken on trust: jh7110_find_uncached() tests them in this
 * order on the running board.
 */
static const u64 jh7110_uncached_offsets[] = {
	0x400000000ULL,		/* DRAM + 16 GiB */
	0x200000000ULL,		/* DRAM + 8 GiB */
};

/* SiFive composable L2: a write to FLUSH64 writes back and drops a line */
#define CCACHE_FLUSH64			0x200
#define CCACHE_LINE			64

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

/* dom_vout_syscon: the transmitter's input mux (u2_display_panel_mux) */
#define VOUT_SYSCFG_4			0x4
#define VOUT_HDMI_DPI_BIT_DEPTH		GENMASK(29, 28)
#define VOUT_HDMI_DPI_DP_SEL		BIT(30)
#define VOUT_SYSCFG_8			0x8
#define VOUT_HDMI_PANEL_SEL		BIT(4)

/* Innosilicon HDMI transmitter: 8-bit registers, 4 bytes apart */
#define HDMI_SYS_CTRL			0x00
#define  SYS_NOT_RST_ANALOG		BIT(6)
#define  SYS_NOT_RST_DIGITAL		BIT(5)
#define  SYS_PWR_OFF			BIT(1)
#define  SYS_INT_POL_HIGH		BIT(0)
#define HDMI_AV_MUTE			0x05
#define  AV_VIDEO_BLACK			BIT(0)
#define HDMI_VIDEO_TIMING_CTL		0x08
#define  TIMING_VSYNC_POSITIVE		BIT(3)
#define  TIMING_HSYNC_POSITIVE		BIT(2)
#define  TIMING_EXTERNAL_VIDEO		BIT(0)
#define HDMI_VIDEO_EXT_HTOTAL_L		0x09
#define HDMI_VIDEO_EXT_HBLANK_L		0x0b
#define HDMI_VIDEO_EXT_HDELAY_L		0x0d
#define HDMI_VIDEO_EXT_HDURATION_L	0x0f
#define HDMI_VIDEO_EXT_VTOTAL_L		0x11
#define HDMI_VIDEO_EXT_VBLANK		0x13
#define HDMI_VIDEO_EXT_VDELAY		0x14
#define HDMI_VIDEO_EXT_VDURATION	0x15
#define HDMI_HDCP_CTRL			0x52
#define HDMI_COLORBAR			0xc9
#define  COLORBAR_BIST			0x00	/* the transmitter's own bars */
#define  COLORBAR_NORMAL		0x10	/* video from the DC8200 */
#define HDMI_PHY_SYNC			0xce

/* Innosilicon HDMI PHY, in the same window from register index 0x1a0 */
#define PHY_PRE_PLL_CONTROL		0x1a0
#define  PRE_PLL_POWER_DOWN		BIT(0)
#define PHY_PRE_PLL_DIV_1		0x1a1
#define PHY_PRE_PLL_DIV_2		0x1a2
#define  PRE_PLL_INTEGER		0xf0	/* no spread spectrum, no fraction */
#define PHY_PRE_PLL_DIV_3		0x1a3
#define PHY_PRE_PLL_TMDSCLK_DIV		0x1a4
#define PHY_PCLK_DIV_AB			0x1a5
#define PHY_PCLK_DIV_CD			0x1a6
#define PHY_PRE_PLL_LOCK_STATUS		0x1a9
#define PHY_POST_PLL_DIV_1		0x1aa
#define  POST_PLL_OFF			0x0f
#define  POST_PLL_ON			0x0e	/* post divider, TMDS reference */
#define PHY_POST_PLL_DIV_2		0x1ab
#define PHY_POST_PLL_DIV_3		0x1ac
#define PHY_POST_PLL_DIV_4		0x1ad
#define PHY_POST_PLL_LOCK_STATUS	0x1af
#define PHY_BIAS_CONTROL		0x1b0
#define  BIAS_ENABLE			BIT(2)
#define PHY_TMDS_CONTROL		0x1b2
#define  TMDS_DRIVERS_ON		0x8f
#define PHY_LDO_CONTROL			0x1b4
#define  LDO_ON				0x07
#define PHY_SERIALIZER_CONTROL		0x1be
#define  SERIALIZER_ON			0x71
#define PHY_DRIVE_1			0x1bf
#define PHY_DRIVE_2			0x1c0
#define PHY_RX_CONTROL			0x1cc
#define  RX_ON				0x0f

/* VeriSilicon DC8200, output 0 (register names of the upstream driver) */
#define DC_TOP_CHIP_MODEL		0x0020
#define DC_TOP_CHIP_REV			0x0024
#define DC_FB_ADDRESS			0x1400
#define DC_FB_STRIDE			0x1408
#define DC_DISP_DITHER_CONFIG		0x1410
#define DC_DISP_PANEL_CONFIG		0x1418
#define  PANEL_DE_EN			BIT(0)
#define  PANEL_DAT_EN			BIT(4)
#define  PANEL_CLK_EN			BIT(8)
#define  PANEL_RUNNING			BIT(12)
#define DC_DISP_HSIZE			0x1430
#define DC_DISP_HSYNC			0x1438
#define DC_DISP_VSIZE			0x1440
#define DC_DISP_VSYNC			0x1448
#define  SYNC_EN			BIT(30)
#define DC_DISP_DPI_CONFIG		0x14b8
#define  DPI_FMT_RGB888			5
#define DC_FB_CONFIG			0x1518
#define  FB_CONFIG_FMT_X8R8G8B8		(5 << 26)
#define DC_FB_SIZE			0x1810
#define DC_FB_CONFIG_EX			0x1cc0
#define  FB_EX_COMMIT			BIT(12)
#define  FB_EX_FB_EN			BIT(13)
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
	/* HDMI PHY pre-PLL: StarFive's pre_pll_cfg_table entry for the clock */
	u8 prediv;
	u16 fbdiv;
	u8 tmds_a, tmds_b, tmds_c;
	u8 pclk_a, pclk_b, pclk_c, pclk_d;
	/* TMDS drive: StarFive's inno_hdmi_improve_eye_diagram() */
	u8 drive_1, drive_2;
};

/* CEA-861 modes, both +hsync/+vsync */
static const struct jh7110_mode jh7110_modes[] = {
	/* VIC 16 */
	{ "1080p", 148500000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125,
	  1, 99, 1, 1, 1, 1, 2, 2, 2, 0x02, 0x22 },
	/* VIC 4 */
	{ "720p", 74250000, 1280, 1390, 1430, 1650, 720, 725, 730, 750,
	  1, 99, 1, 2, 2, 1, 2, 3, 4, 0x00, 0x00 },
};

struct jh7110_hdmi_priv {
	void __iomem *dc;
	void __iomem *hdmi;
	void __iomem *voutsys;
	void __iomem *voutcrg;
	void __iomem *syscrg;
	void __iomem *pmu;
	void __iomem *ccache;
	const struct jh7110_mode *mode;
	bool pixclk_pll2;
	ulong fb_phys;
	ulong fb_size;
	u64 uncached_offset;	/* 0: the CPU uses the cached framebuffer */
	bool up;
};

/*
 * What the OS device tree fixup and the diagnostic commands need.  Kept
 * outside the device's private data, which driver model frees when probe
 * fails.
 */
static struct {
	void __iomem *dc, *hdmi, *voutsys, *voutcrg, *syscrg;
	bool vout_on;		/* the display window may be read */
	bool tx_on;		/* the transmitter is running */
	ulong fb_phys, fb_size;	/* scanned-out RAM, kept away from the OS */
	u64 uncached_offset;
} jh7110_state;

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

/* Write back and drop every L2 line of [start, end) */
static void jh7110_l2_flush(struct jh7110_hdmi_priv *p, ulong start, ulong end)
{
	ulong line;

	mb();
	for (line = start & ~(ulong)(CCACHE_LINE - 1); line < end;
	     line += CCACHE_LINE)
		writeq_relaxed(line, p->ccache + CCACHE_FLUSH64);
	mb();
}

/*
 * Whether CPU accesses at @pa + @off reach the DRAM word at @pa directly,
 * past the caches: a word written through the cached address and flushed
 * must read back through the other view, and a word written through the
 * other view must read back through the cached address once that line has
 * been flushed again.  A view that faults, reaches other memory or is
 * itself cached fails.  Nothing is written through the other view before
 * a read has shown that it reaches this very word.
 */
static bool jh7110_view_is_uncached(struct jh7110_hdmi_priv *p, ulong pa,
				    u64 off)
{
	volatile u64 *cached = (volatile u64 *)pa;
	volatile u64 *view = (volatile u64 *)(uintptr_t)(pa + off);
	const u64 a = 0x4a48373131304846ULL, b = ~a;
	struct resume_data resume;
	volatile bool ok = false;
	int i;

	*cached = a;
	jh7110_l2_flush(p, pa, pa + 8);
	udelay(10);

	set_resume(&resume);
	if (!setjmp(resume.jump) && *view == a) {
		*view = b;
		mb();
		for (i = 0; i < 3 && !ok; i++) {
			udelay(10);
			jh7110_l2_flush(p, pa, pa + 8);
			ok = *cached == b;
		}
	}
	set_resume(NULL);

	/* Leave no line of either view in the cache */
	jh7110_l2_flush(p, pa, pa + 8);
	jh7110_l2_flush(p, pa + off, pa + off + 8);

	return ok;
}

/*
 * The DC8200 reads the framebuffer from DRAM and does not snoop the CPU
 * caches, and the U74 cores ignore page-table memory types.  Everything on
 * the CPU side (U-Boot's console, the EFI GOP, and so the OS and X) uses an
 * uncached view of the framebuffer RAM when one is found: every write
 * reaches DRAM at once and nothing ever needs flushing.  The DC8200 itself
 * is given the real address.
 *
 * Without a working uncached view U-Boot draws through the cache and
 * flushes the L2 after every update (jh7110_hdmi_sync()), and the OS gets
 * the cached framebuffer, which will show stale lines.
 */
static u64 jh7110_find_uncached(struct jh7110_hdmi_priv *p)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(jh7110_uncached_offsets); i++)
		if (jh7110_view_is_uncached(p, p->fb_phys,
					    jh7110_uncached_offsets[i]))
			return jh7110_uncached_offsets[i];

	log_err("jh7110-hdmi: no uncached view of DRAM found; drawing through the cache, the OS framebuffer will show stale lines\n");

	return 0;
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
 * can wedge the bus.
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

	/* The NoC display bus clock, then its reset */
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
	/* Feeds hdmi_tx_bclk; StarFive's drivers and HFI ungate it too */
	jh_clk_gate(p->syscrg, SYSCLK_I2STX0_BCLK, true);
	ret = jh_reset_deassert(p->syscrg, SYSCRG_RST_ASSERT, SYSCRG_RST_STATUS,
				SYSRST_VOUT_TOP_SRC);
	if (ret) {
		log_err("jh7110-hdmi: VOUT top reset stuck\n");
		return ret;
	}

	/*
	 * The DC8200 pixel clocks.  By default both take the HDMI PHY's
	 * pixel clock, as in StarFive's Linux driver, so pixel data and TMDS
	 * share one PLL.  With hdmi_pixclk=pll2 they take vout_src (PLL2)
	 * divided down to the pixel rate instead, as StarFive's U-Boot and
	 * HFI BIOS do.  The gates stay off until the clock is running.
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

	/* HDMI transmitter: pclk (hdmi_tx_sys), mclk, bclk, then its reset */
	jh_clk_gate(p->voutcrg, VOUTCLK_HDMI_TX_SYS, true);
	jh_clk_gate(p->voutcrg, VOUTCLK_HDMI_TX_MCLK, true);
	jh_clk_gate(p->voutcrg, VOUTCLK_HDMI_TX_BCLK, true);
	ret = jh_reset_deassert(p->voutcrg, VOUTCRG_RST_ASSERT,
				VOUTCRG_RST_STATUS, VOUTRST_HDMI_TX);
	if (ret) {
		log_err("jh7110-hdmi: HDMI transmitter reset stuck\n");
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

	jh7110_state.vout_on = true;

	return 0;
}

/*
 * The transmitter takes DC8200 panel 0 as parallel DPI, 8 bits per
 * component.  This is the reset state, which StarFive's drivers rely on;
 * set it anyway in case earlier software routed it elsewhere.  The DP-bus
 * fields of the same register are left alone.
 */
static void jh7110_route_panel0(struct jh7110_hdmi_priv *p)
{
	clrbits_le32(p->voutsys + VOUT_SYSCFG_4,
		     VOUT_HDMI_DPI_DP_SEL | VOUT_HDMI_DPI_BIT_DEPTH);
	clrbits_le32(p->voutsys + VOUT_SYSCFG_8, VOUT_HDMI_PANEL_SEL);
}

/*
 * HDMI PHY: pre-PLL (pixel and TMDS clocks from the 24 MHz reference) and
 * post-PLL (serializer clock), then the LDO, the serializer and the drive
 * settings for the mode.  StarFive's inno_hdmi_config_pll() and the first
 * half of its inno_hdmi_setup(), register for register.
 */
static int jh7110_hdmi_phy(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;

	hdmi_modify(p, PHY_BIAS_CONTROL, BIAS_ENABLE, BIAS_ENABLE);
	hdmi_write(p, PHY_RX_CONTROL, RX_ON);

	hdmi_write(p, PHY_PRE_PLL_CONTROL, PRE_PLL_POWER_DOWN);
	hdmi_write(p, PHY_POST_PLL_DIV_1, POST_PLL_OFF);
	hdmi_write(p, PHY_PRE_PLL_DIV_1, m->prediv);
	hdmi_write(p, PHY_PRE_PLL_DIV_2, PRE_PLL_INTEGER | (m->fbdiv >> 8));
	hdmi_write(p, PHY_PRE_PLL_DIV_3, m->fbdiv & 0xff);
	hdmi_write(p, PHY_PRE_PLL_TMDSCLK_DIV,
		   (m->tmds_a << 4) | (m->tmds_b << 2) | m->tmds_c);
	hdmi_write(p, PHY_PCLK_DIV_AB, (m->pclk_b << 5) | m->pclk_a);
	hdmi_write(p, PHY_PCLK_DIV_CD, (m->pclk_c << 5) | m->pclk_d);
	/* Post-PLL: prediv 1, fbdiv 20, postdiv 1 for both modes */
	hdmi_write(p, PHY_POST_PLL_DIV_2, 1);
	hdmi_write(p, PHY_POST_PLL_DIV_3, 20);
	hdmi_write(p, PHY_POST_PLL_DIV_4, 1);
	hdmi_write(p, PHY_POST_PLL_DIV_1, POST_PLL_ON);
	hdmi_write(p, PHY_PRE_PLL_CONTROL, 0);

	if (hdmi_poll(p, PHY_PRE_PLL_LOCK_STATUS, BIT(0))) {
		log_err("jh7110-hdmi: HDMI PHY pre-PLL did not lock at %u Hz\n",
			m->clock);
		return -ETIMEDOUT;
	}
	if (hdmi_poll(p, PHY_POST_PLL_LOCK_STATUS, BIT(0))) {
		log_err("jh7110-hdmi: HDMI PHY post-PLL did not lock\n");
		return -ETIMEDOUT;
	}

	hdmi_write(p, PHY_LDO_CONTROL, LDO_ON);
	hdmi_write(p, PHY_SERIALIZER_CONTROL, SERIALIZER_ON);
	hdmi_write(p, PHY_DRIVE_1, m->drive_1);
	hdmi_write(p, PHY_DRIVE_2, m->drive_2);

	return 0;
}

/*
 * The transmitter, once the PHY runs: the rest of StarFive's
 * inno_hdmi_setup().  Its registers are clocked from TMDS (SYS_CTRL bit 2
 * clear, as StarFive's drivers and HFI BIOS leave it), which is running
 * now.
 */
static void jh7110_hdmi_tx(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;

	/* Out of reset, digital then analog, still powered down */
	hdmi_write(p, HDMI_SYS_CTRL,
		   SYS_NOT_RST_DIGITAL | SYS_PWR_OFF | SYS_INT_POL_HIGH);
	udelay(150);
	hdmi_write(p, HDMI_SYS_CTRL, SYS_NOT_RST_ANALOG | SYS_NOT_RST_DIGITAL |
		   SYS_PWR_OFF | SYS_INT_POL_HIGH);
	udelay(150);

	/* Video from the DC8200, not the colour bars; DVI; not blanked */
	hdmi_write(p, HDMI_COLORBAR, COLORBAR_NORMAL);
	hdmi_write(p, HDMI_HDCP_CTRL, 0);
	hdmi_modify(p, HDMI_AV_MUTE, AV_VIDEO_BLACK, 0);

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
	hdmi_write(p, HDMI_VIDEO_TIMING_CTL, TIMING_EXTERNAL_VIDEO |
		   TIMING_HSYNC_POSITIVE | TIMING_VSYNC_POSITIVE);

	/* Power up, TMDS drivers on, then resynchronise the PHY FIFO */
	hdmi_write(p, HDMI_SYS_CTRL, SYS_NOT_RST_ANALOG | SYS_NOT_RST_DIGITAL |
		   SYS_INT_POL_HIGH);
	hdmi_write(p, PHY_TMDS_CONTROL, TMDS_DRIVERS_ON);
	mdelay(50);
	hdmi_write(p, HDMI_PHY_SYNC, 0);
	hdmi_write(p, HDMI_PHY_SYNC, 1);

	jh7110_state.tx_on = true;
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

/* vs_primary_plane_atomic_update(): linear X8R8G8B8, full screen */
static void jh7110_dc_plane(struct jh7110_hdmi_priv *p)
{
	const struct jh7110_mode *m = p->mode;
	u32 pos_br = (m->hdisplay & 0x7fff) | ((m->vdisplay & 0x7fff) << 15);

	writel(FB_CONFIG_FMT_X8R8G8B8, p->dc + DC_FB_CONFIG);
	writel(lower_32_bits(p->fb_phys), p->dc + DC_FB_ADDRESS);
	writel(m->hdisplay * FB_BYTES_PER_PIXEL, p->dc + DC_FB_STRIDE);
	writel(0, p->dc + DC_FB_TOP_LEFT);
	writel(pos_br, p->dc + DC_FB_BOTTOM_RIGHT);
	writel(pos_br, p->dc + DC_FB_SIZE);
	writel(BLEND_DISABLE, p->dc + DC_FB_BLEND_CONFIG);
	writel(FB_EX_FB_EN | FB_EX_COMMIT, p->dc + DC_FB_CONFIG_EX);
}

/* vs_bridge_atomic_enable_dpi(): RGB888 DPI, no dither, no gamma */
static void jh7110_dc_output(struct jh7110_hdmi_priv *p)
{
	clrbits_le32(p->dc + DC_DISP_DP_CONFIG, DP_EN);
	writel(DPI_FMT_RGB888, p->dc + DC_DISP_DPI_CONFIG);
	writel(0, p->dc + DC_DISP_DITHER_CONFIG);
	writel(PANEL_DE_EN | PANEL_DAT_EN | PANEL_CLK_EN,
	       p->dc + DC_DISP_PANEL_CONFIG);
	setbits_le32(p->dc + DC_DISP_PANEL_CONFIG, PANEL_RUNNING);
	clrbits_le32(p->dc + DC_DISP_PANEL_START, PANEL_START_MULTI_DISP_SYNC);
	setbits_le32(p->dc + DC_DISP_PANEL_START, PANEL_START_RUNNING0);
	setbits_le32(p->dc + DC_DISP_PANEL_CONFIG_EX, PANEL_EX_COMMIT);
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

/*
 * Only drawing through the cache needs this: write the rows U-Boot changed
 * back to DRAM, where the DC8200 reads them.
 */
static int jh7110_hdmi_sync(struct udevice *dev)
{
	struct jh7110_hdmi_priv *p = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	int y0 = 0, y1 = uc_priv->ysize;

	if (!p->up || p->uncached_offset)
		return 0;

	if (IS_ENABLED(CONFIG_VIDEO_DAMAGE)) {
		y0 = uc_priv->damage.ystart;
		y1 = min_t(int, uc_priv->damage.yend, uc_priv->ysize);
	}
	if (y1 > y0)
		jh7110_l2_flush(p, p->fb_phys + y0 * uc_priv->line_length,
				p->fb_phys + y1 * uc_priv->line_length);

	return 0;
}

static int jh7110_hdmi_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct jh7110_hdmi_priv *p = dev_get_priv(dev);
	const struct jh7110_mode *m;
	u32 model;
	int ret;

	p->mode = m = jh7110_pick_mode();
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

	log_info("jh7110-hdmi: %ux%u@60, pixel clock from %s, framebuffer %#lx\n",
		 m->hdisplay, m->vdisplay,
		 p->pixclk_pll2 ? "PLL2" : "HDMI PHY", p->fb_phys);

	p->uncached_offset = jh7110_find_uncached(p);

	ret = jh7110_vout_enable(p);
	if (ret)
		return ret;

	model = readl(p->dc + DC_TOP_CHIP_MODEL);
	if (model != 0x8200)
		log_warning("jh7110-hdmi: unexpected display controller model %#x\n",
			    model);

	/* The transmitter first, completely */
	jh7110_route_panel0(p);
	ret = jh7110_hdmi_phy(p);
	if (ret)
		return ret;
	jh7110_hdmi_tx(p);

	/* Then the scanout, of a black framebuffer until U-Boot draws */
	jh7110_dc_timing(p);
	jh_clk_gate(p->voutcrg, VOUTCLK_DC8200_PIX0, true);
	memset((void *)(uintptr_t)(p->fb_phys + p->uncached_offset), 0,
	       p->fb_size);
	if (!p->uncached_offset)
		jh7110_l2_flush(p, p->fb_phys, p->fb_phys + p->fb_size);
	jh7110_dc_plane(p);
	jh7110_dc_output(p);

	uc_priv->xsize = m->hdisplay;
	uc_priv->ysize = m->vdisplay;
	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;
	uc_priv->line_length = m->hdisplay * FB_BYTES_PER_PIXEL;

	/* From here on U-Boot, the EFI GOP and the OS use the CPU view */
	plat->base = p->fb_phys + p->uncached_offset;
	video_set_flush_dcache(dev, false);

	jh7110_state.fb_phys = p->fb_phys;
	jh7110_state.fb_size = p->fb_size;
	jh7110_state.uncached_offset = p->uncached_offset;
	p->up = true;

	if (p->uncached_offset)
		log_info("jh7110-hdmi: running; CPU view %#llx (uncached, DRAM + %llu GiB), DC%x rev %x\n",
			 (unsigned long long)plat->base,
			 (unsigned long long)(p->uncached_offset >> 30), model,
			 readl(p->dc + DC_TOP_CHIP_REV));
	else
		log_info("jh7110-hdmi: running; CPU view %#llx (cached), DC%x rev %x\n",
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
	p->ccache = dev_read_addr_name_ptr(dev, "ccache");
	if (!p->dc || !p->hdmi || !p->voutsys || !p->voutcrg || !p->syscrg ||
	    !p->pmu || !p->ccache)
		return -EINVAL;

	jh7110_state.dc = p->dc;
	jh7110_state.hdmi = p->hdmi;
	jh7110_state.voutsys = p->voutsys;
	jh7110_state.voutcrg = p->voutcrg;
	jh7110_state.syscrg = p->syscrg;

	return 0;
}

static int jh7110_hdmi_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	/* Room for the largest mode, reserved before relocation */
	plat->size = FB_MAX_XSIZE * FB_MAX_YSIZE * FB_BYTES_PER_PIXEL;

	return 0;
}

static const struct video_ops jh7110_hdmi_ops = {
	.video_sync	= jh7110_hdmi_sync,
};

static const struct udevice_id jh7110_hdmi_ids[] = {
	{ .compatible = "openbsd-hdmi-vf2,jh7110-hdmi-fb" },
	{ }
};

U_BOOT_DRIVER(jh7110_hdmi) = {
	.name		= "jh7110_hdmi",
	.id		= UCLASS_VIDEO,
	.of_match	= jh7110_hdmi_ids,
	.ops		= &jh7110_hdmi_ops,
	.bind		= jh7110_hdmi_bind,
	.of_to_plat	= jh7110_hdmi_of_to_plat,
	.probe		= jh7110_hdmi_probe,
	.priv_auto	= sizeof(struct jh7110_hdmi_priv),
	.flags		= DM_FLAG_PRE_RELOC,
};

/*
 * The device tree the OS receives: drop this driver's node, which describes
 * registers of other devices to U-Boot only, and keep the OS away from the
 * scanned-out RAM.  The reservation also lands in the EFI memory map as
 * reserved, through efi_carve_out_dt_rsv().
 */
static int jh7110_hdmi_ft_fixup(void *ctx, struct event *event)
{
	void *blob = oftree_lookup_fdt(event->data.ft_fixup.tree);
	struct fdt_memory fb;
	int node, ret;

	if (!blob) {
		if (jh7110_state.fb_size)
			log_err("jh7110-hdmi: no flat device tree to reserve the framebuffer in\n");
		return 0;
	}

	node = fdt_node_offset_by_compatible(blob, -1,
					     "openbsd-hdmi-vf2,jh7110-hdmi-fb");
	if (node >= 0)
		fdt_del_node(blob, node);

	if (!jh7110_state.fb_size)
		return 0;

	fb.start = jh7110_state.fb_phys;
	fb.end = jh7110_state.fb_phys + jh7110_state.fb_size - 1;
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
	static const u32 dc[] = { 0x0020, 0x0024, 0x1400, 0x1408, 0x1410,
		0x1418, 0x1430, 0x1438, 0x1440, 0x1448, 0x14b8, 0x1518, 0x1810,
		0x1cc0, 0x1ccc, 0x1cd0, 0x24d8, 0x24e0, 0x2510, 0x2518 };
	static const u32 hdmi[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08,
		0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
		0x13, 0x14, 0x15, 0x52, 0xc8, 0xc9, 0xce, 0x1a0, 0x1a1, 0x1a2,
		0x1a3, 0x1a4, 0x1a5, 0x1a6, 0x1a9, 0x1aa, 0x1ab, 0x1ac, 0x1ad,
		0x1af, 0x1b0, 0x1b2, 0x1b4, 0x1be, 0x1bf, 0x1c0, 0x1cc };
	int i;

	/* Only read the display window while it is powered and clocked */
	if (!jh7110_state.vout_on) {
		printf("display pipeline is not up; not touching it\n");
		return CMD_RET_FAILURE;
	}

	if (jh7110_state.fb_size)
		printf("framebuffer %#lx, CPU view %#llx (%s)\n",
		       jh7110_state.fb_phys,
		       (unsigned long long)(jh7110_state.fb_phys +
					    jh7110_state.uncached_offset),
		       jh7110_state.uncached_offset ? "uncached" : "cached");
	dump_regs("syscrg clocks", jh7110_state.syscrg, syscrg,
		  ARRAY_SIZE(syscrg), 4);
	printf("syscrg resets: %#010x %#010x (status %#010x %#010x)\n",
	       readl(jh7110_state.syscrg + SYSCRG_RST_ASSERT),
	       readl(jh7110_state.syscrg + SYSCRG_RST_ASSERT + 4),
	       readl(jh7110_state.syscrg + SYSCRG_RST_STATUS),
	       readl(jh7110_state.syscrg + SYSCRG_RST_STATUS + 4));
	dump_regs("voutcrg clocks", jh7110_state.voutcrg, voutcrg,
		  ARRAY_SIZE(voutcrg), 4);
	printf("voutcrg resets: %#010x (status %#010x)\n",
	       readl(jh7110_state.voutcrg + VOUTCRG_RST_ASSERT),
	       readl(jh7110_state.voutcrg + VOUTCRG_RST_STATUS));
	dump_regs("vout syscon", jh7110_state.voutsys, voutsys,
		  ARRAY_SIZE(voutsys), 4);
	dump_regs("dc8200", jh7110_state.dc, dc, ARRAY_SIZE(dc), 1);
	printf("hdmi (register index: value):\n");
	for (i = 0; i < ARRAY_SIZE(hdmi); i++)
		printf("  %#05x: %#04x\n", hdmi[i],
		       readl(jh7110_state.hdmi + hdmi[i] * 4) & 0xff);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(hdmiregs, 1, 0, do_hdmiregs,
	   "dump the JH7110 display pipeline registers",
	   "");

/*
 * The transmitter's own colour bars, independent of the DC8200: bars on
 * the monitor mean the PHY, PLLs, cable and sink all work and any problem
 * is in the display controller; no bars mean the problem is before it.
 */
static int do_hdmibars(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	bool on;

	if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off")))
		return CMD_RET_USAGE;
	if (!jh7110_state.tx_on) {
		printf("the HDMI transmitter is not running\n");
		return CMD_RET_FAILURE;
	}

	on = !strcmp(argv[1], "on");
	writel(on ? COLORBAR_BIST : COLORBAR_NORMAL,
	       jh7110_state.hdmi + HDMI_COLORBAR * 4);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(hdmibars, 2, 0, do_hdmibars,
	   "show the HDMI transmitter's colour bars instead of the framebuffer",
	   "on|off");
#endif
