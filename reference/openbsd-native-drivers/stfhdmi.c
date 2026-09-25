/*	$OpenBSD$	*/
/*
 * StarFive JH7110 Innosilicon HDMI controller.
 *
 * First-boot implementation: fixed CEA 720p60/1080p60, RGB888, no audio,
 * EDID or interrupt-driven hotplug.  The shared HDMI window is not touched
 * until pclk/mclk/bclk are running and the controller reset is deasserted.
 * The digital controller is programmed only after the PHY pixel clock and
 * post-PLL/analog chain have reported lock.
 *
 * SPDX-License-Identifier: ISC
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/errno.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>
#include <dev/ofw/fdt.h>

#include "stfhdmivar.h"

/* Innosilicon logical byte registers; the parent applies the x4 spacing. */
#define HDMI_SYS_CTRL			0x00
#define HDMI_VIDEO_CTRL1		0x01
#define HDMI_VIDEO_CTRL2		0x02
#define HDMI_VIDEO_CTRL			0x03
#define HDMI_VIDEO_CTRL3		0x04
#define HDMI_AV_MUTE			0x05
#define HDMI_VIDEO_TIMING_CTL		0x08
#define HDMI_EXT_HTOTAL_L		0x09
#define HDMI_EXT_HTOTAL_H		0x0a
#define HDMI_EXT_HBLANK_L		0x0b
#define HDMI_EXT_HBLANK_H		0x0c
#define HDMI_EXT_HDELAY_L		0x0d
#define HDMI_EXT_HDELAY_H		0x0e
#define HDMI_EXT_HDURATION_L		0x0f
#define HDMI_EXT_HDURATION_H		0x10
#define HDMI_EXT_VTOTAL_L		0x11
#define HDMI_EXT_VTOTAL_H		0x12
#define HDMI_EXT_VBLANK			0x13
#define HDMI_EXT_VDELAY			0x14
#define HDMI_EXT_VDURATION		0x15
#define HDMI_HDCP_CTRL			0x52
#define HDMI_PACKET_BUF_INDEX		0x9f
#define HDMI_PACKET_BUF_ADDR		0xa0

#define HDMI_SYS_NOT_RST_ANALOG		(1U << 6)
#define HDMI_SYS_NOT_RST_DIGITAL	(1U << 5)
#define HDMI_SYS_INT_HIGH		(1U << 0)

#define HDMI_MUTE_AUDIO			(1U << 1)
#define HDMI_BLACK_VIDEO		(1U << 0)
#define HDMI_HDMI_MODE			(1U << 1)
#define HDMI_EXT_TIMING_ENABLE		(1U << 0)
#define HDMI_HSYNC_POSITIVE		(1U << 2)
#define HDMI_VSYNC_POSITIVE		(1U << 3)

#define HDMI_START_RETRIES		3
#define HDMI_RETRY_DELAY_US		2000

/* dom_vout_syscon HDMI route. */
#define VOUT_SYSCFG_4			0x04
#define VOUT_HDMI_DP_BIT_DEPTH		(1U << 25)
#define VOUT_HDMI_DP_YUV_MODE		(3U << 26)
#define VOUT_HDMI_DP_YUV_RGB		(3U << 26)
#define VOUT_HDMI_DPI_BIT_DEPTH		(3U << 28)
#define VOUT_HDMI_DPI_DP_SEL		(1U << 30)
#define VOUT_SYSCFG_8			0x08
#define VOUT_HDMI_PANEL_SEL		(1U << 4)

static const struct stf_video_mode stf_modes[] = {
	/* CEA VIC 4: 1280x720p60, +H/+V. */
	{ 74250000, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 4 },
	/* CEA VIC 16: 1920x1080p60, +H/+V. */
	{ 148500000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 16 },
};

struct stfhdmi_softc {
	struct device		sc_dev;
	struct device		*sc_parent;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_vout_ioh;
	bus_size_t		sc_vout_size;
	int			sc_vout_mapped;
	int			sc_node;
	int			sc_ready;
	int			sc_starting;
};

static struct stfhdmi_softc *stfhdmi0;
static const struct stf_video_mode *stf_mode = &stf_modes[1];

int	stfhdmi_match(struct device *, void *, void *);
void	stfhdmi_attach(struct device *, struct device *, void *);

const struct cfattach stfhdmi_ca = {
	sizeof(struct stfhdmi_softc), stfhdmi_match, stfhdmi_attach
};

struct cfdriver stfhdmi_cd = {
	NULL, "stfhdmi", DV_DULL
};

static void
stfhdmi_mode_select(void)
{
	int chosen;
	char name[16];

	chosen = OF_finddevice("/chosen");
	if (chosen == 0 || chosen == -1)
		return;
	memset(name, 0, sizeof(name));
	if (OF_getprop(chosen, "openbsd,hdmi-mode", name,
	    sizeof(name) - 1) <= 0)
		return;
	if (strcmp(name, "720p60") == 0)
		stf_mode = &stf_modes[0];
	else if (strcmp(name, "1080p60") == 0)
		stf_mode = &stf_modes[1];
}

const struct stf_video_mode *
stfhdmi_mode(void)
{
	/* DC may attach before the HDMI controller. Select before allocation. */
	stfhdmi_mode_select();
	return stf_mode;
}

int
stfhdmi_is_ready(void)
{
	return stfhdmi0 != NULL && stfhdmi0->sc_ready;
}

int
stfhdmi_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node,
	    "starfive,jh7110-inno-hdmi-controller");
}

void
stfhdmi_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfhdmi_softc *sc = (struct stfhdmi_softc *)self;
	struct fdt_attach_args *faa = aux;

	sc->sc_parent = parent;
	sc->sc_node = faa->fa_node;
	sc->sc_iot = stfhdmimfd_iot(parent);
	if (stfhdmi0 == NULL)
		stfhdmi0 = sc;
	stfhdmi_mode_select();

	printf(": fixed %ux%u@60 bring-up\n",
	    stf_mode->hdisplay, stf_mode->vdisplay);
	stfhdmi_try_start();
}

static int
stfhdmi_map_vout_syscon(struct stfhdmi_softc *sc)
{
	struct fdt_reg reg;
	uint32_t phandle;
	void *node;

	if (sc->sc_vout_mapped)
		return 0;
	phandle = OF_getpropint(sc->sc_node, "starfive,vout-syscon", 0);
	if (phandle == 0)
		return ENXIO;
	node = fdt_find_phandle(phandle);
	if (node == NULL || fdt_get_reg(node, 0, &reg))
		return ENXIO;
	if (bus_space_map(sc->sc_iot, reg.addr, reg.size, 0,
	    &sc->sc_vout_ioh))
		return ENXIO;
	sc->sc_vout_size = reg.size;
	sc->sc_vout_mapped = 1;
	return 0;
}

static int
stfhdmi_route_panel0_dpi(struct stfhdmi_softc *sc)
{
	uint32_t reg, mask;

	if (!sc->sc_vout_mapped || sc->sc_vout_size < VOUT_SYSCFG_8 + 4)
		return ENXIO;

	mask = VOUT_HDMI_DPI_DP_SEL | VOUT_HDMI_DP_BIT_DEPTH |
	    VOUT_HDMI_DP_YUV_MODE | VOUT_HDMI_DPI_BIT_DEPTH;
	reg = bus_space_read_4(sc->sc_iot, sc->sc_vout_ioh, VOUT_SYSCFG_4);
	reg &= ~mask;
	reg |= VOUT_HDMI_DP_YUV_RGB;       /* RGB, DPI, 8-bit. */
	bus_space_write_4(sc->sc_iot, sc->sc_vout_ioh, VOUT_SYSCFG_4, reg);

	reg = bus_space_read_4(sc->sc_iot, sc->sc_vout_ioh, VOUT_SYSCFG_8);
	reg &= ~VOUT_HDMI_PANEL_SEL;        /* DC8200 panel 0. */
	bus_space_write_4(sc->sc_iot, sc->sc_vout_ioh, VOUT_SYSCFG_8, reg);
	return 0;
}

static void
stfhdmi_write16(struct stfhdmi_softc *sc, uint16_t lo, uint16_t val)
{
	stfhdmimfd_write(sc->sc_parent, lo, val & 0xff);
	stfhdmimfd_write(sc->sc_parent, lo + 1, val >> 8);
}

static void
stfhdmi_write_avi(struct stfhdmi_softc *sc, uint8_t vic)
{
	uint8_t frame[17];
	uint8_t sum = 0;
	int i;

	memset(frame, 0, sizeof(frame));
	frame[0] = 0x82;             /* AVI InfoFrame. */
	frame[1] = 0x02;             /* version 2. */
	frame[2] = 0x0d;             /* 13-byte payload. */
	frame[5] = 0xa0;             /* BT.709, coded aspect 16:9. */
	frame[7] = vic;
	for (i = 0; i < nitems(frame); i++)
		sum += frame[i];
	frame[3] = (uint8_t)(0 - sum);

	stfhdmimfd_write(sc->sc_parent, HDMI_PACKET_BUF_INDEX, 0x06);
	for (i = 0; i < nitems(frame); i++)
		stfhdmimfd_write(sc->sc_parent, HDMI_PACKET_BUF_ADDR + i,
		    frame[i]);
}

static int
stfhdmi_program(struct stfhdmi_softc *sc, const struct stf_video_mode *m)
{
	uint16_t val;

	/* Keep A/V muted while the timing generator is changed. */
	stfhdmimfd_write(sc->sc_parent, HDMI_AV_MUTE,
	    HDMI_MUTE_AUDIO | HDMI_BLACK_VIDEO);

	/*
	 * Reset release with TMDS register-clock source and no clock inversion.
	 * This deliberately does not use the older generic 0x75 programming.
	 */
	stfhdmimfd_write(sc->sc_parent, HDMI_SYS_CTRL,
	    HDMI_SYS_NOT_RST_DIGITAL | HDMI_SYS_INT_HIGH);
	delay(150);
	stfhdmimfd_write(sc->sc_parent, HDMI_SYS_CTRL,
	    HDMI_SYS_NOT_RST_ANALOG | HDMI_SYS_NOT_RST_DIGITAL |
	    HDMI_SYS_INT_HIGH);
	delay(150);

	/* External DE, SDR RGB444 input, 8 bits/component, RGB output. */
	stfhdmimfd_write(sc->sc_parent, HDMI_VIDEO_CTRL1, 0x01);
	stfhdmimfd_write(sc->sc_parent, HDMI_VIDEO_CTRL2, 0x30);
	stfhdmimfd_write(sc->sc_parent, HDMI_VIDEO_CTRL, 0x01);
	stfhdmimfd_write(sc->sc_parent, HDMI_VIDEO_CTRL3, 0x18);

	/* JH7110 bit 3 is VSYNC polarity and bit 2 is HSYNC polarity. */
	stfhdmimfd_write(sc->sc_parent, HDMI_VIDEO_TIMING_CTL,
	    HDMI_EXT_TIMING_ENABLE | HDMI_VSYNC_POSITIVE |
	    HDMI_HSYNC_POSITIVE);

	stfhdmi_write16(sc, HDMI_EXT_HTOTAL_L, m->htotal);
	val = m->htotal - m->hdisplay;
	stfhdmi_write16(sc, HDMI_EXT_HBLANK_L, val);
	val = m->htotal - m->hsync_start;
	stfhdmi_write16(sc, HDMI_EXT_HDELAY_L, val);
	val = m->hsync_end - m->hsync_start;
	stfhdmi_write16(sc, HDMI_EXT_HDURATION_L, val);

	stfhdmi_write16(sc, HDMI_EXT_VTOTAL_L, m->vtotal);
	stfhdmimfd_write(sc->sc_parent, HDMI_EXT_VBLANK,
	    m->vtotal - m->vdisplay);
	stfhdmimfd_write(sc->sc_parent, HDMI_EXT_VDELAY,
	    m->vtotal - m->vsync_start);
	stfhdmimfd_write(sc->sc_parent, HDMI_EXT_VDURATION,
	    m->vsync_end - m->vsync_start);

	stfhdmimfd_write(sc->sc_parent, HDMI_HDCP_CTRL, HDMI_HDMI_MODE);
	stfhdmi_write_avi(sc, m->vic);

	/* DC/wsdisplay owns the final unblank, after successful setup. */
	stfhdmimfd_write(sc->sc_parent, HDMI_AV_MUTE,
	    HDMI_MUTE_AUDIO | HDMI_BLACK_VIDEO);
	return 0;
}

void
stfhdmi_try_start(void)
{
	struct stfhdmi_softc *sc = stfhdmi0;
	const struct stf_video_mode *m;
	uint32_t sysfreq;
	int attempt, error;

	if (sc == NULL || sc->sc_ready || sc->sc_starting)
		return;
	if (!stfhdmiphy_available() || !stfvoutcrg_available())
		return;

	sc->sc_starting = 1;
	stfhdmi_mode_select();
	m = stf_mode;

	/* Make the shared 0x29590000 window safe before any register read. */
	clock_enable(sc->sc_node, "pclk");
	clock_enable(sc->sc_node, "mclk");
	clock_enable(sc->sc_node, "bclk");
	reset_deassert_all(sc->sc_node);
	if (stfvoutcrg_resets_deasserted(1U << 9)) {
		printf("%s: HDMI reset incomplete; refusing shared MMIO\n",
		    sc->sc_dev.dv_xname);
		goto out;
	}
	delay(10);

	sysfreq = clock_get_frequency(sc->sc_node, "pclk");
	if (sysfreq == 0) {
		printf("%s: HDMI pclk unavailable; shared window remains gated\n",
		    sc->sc_dev.dv_xname);
		goto out;
	}
	stfhdmimfd_set_access(sc->sc_parent, 1);
	stfhdmimfd_write(sc->sc_parent, HDMI_AV_MUTE,
	    HDMI_MUTE_AUDIO | HDMI_BLACK_VIDEO);

	error = stfhdmi_map_vout_syscon(sc);
	if (error || stfhdmi_route_panel0_dpi(sc)) {
		printf("%s: can't route DC8200 panel 0 DPI to HDMI\n",
		    sc->sc_dev.dv_xname);
		goto out;
	}

	/*
	 * A provider-ready callback is one-shot on OpenBSD.  Retry transient PLL
	 * lock failures here so a single marginal lock attempt does not leave HDMI
	 * permanently dark until the next reboot.
	 */
	for (attempt = 1; attempt <= HDMI_START_RETRIES; attempt++) {
		error = clock_set_frequency(sc->sc_node, "pixel", m->clock);
		if (error) {
			printf("%s: unsupported pixel clock %u Hz\n",
			    sc->sc_dev.dv_xname, m->clock);
			goto out;
		}
		clock_enable(sc->sc_node, "pixel");
		if (!stfhdmiphy_clock_stable(m->clock)) {
			clock_disable(sc->sc_node, "pixel");
			if (attempt < HDMI_START_RETRIES) {
				delay(HDMI_RETRY_DELAY_US);
				continue;
			}
			printf("%s: pixel pre-PLL did not lock after %d attempts\n",
			    sc->sc_dev.dv_xname, attempt);
			goto out;
		}

		error = stfhdmiphy_power_on(m->clock);
		if (error) {
			stfhdmiphy_power_off();
			clock_disable(sc->sc_node, "pixel");
			if (attempt < HDMI_START_RETRIES) {
				delay(HDMI_RETRY_DELAY_US);
				continue;
			}
			printf("%s: HDMI PHY analog/post-PLL power-on failed after %d attempts (%d)\n",
			    sc->sc_dev.dv_xname, attempt, error);
			goto out;
		}

		if (stfhdmi_program(sc, m) == 0)
			break;
		stfhdmiphy_power_off();
		clock_disable(sc->sc_node, "pixel");
		if (attempt < HDMI_START_RETRIES)
			delay(HDMI_RETRY_DELAY_US);
	}
	if (attempt > HDMI_START_RETRIES)
		goto out;

	sc->sc_ready = 1;
	sc->sc_starting = 0;
	printf("%s: PHY locked, HDMI %ux%u@60 prepared (video blanked)\n",
	    sc->sc_dev.dv_xname, m->hdisplay, m->vdisplay);
	stfdc8200_hdmi_ready(m);
	return;

out:
	stfhdmiphy_power_off();
	clock_disable(sc->sc_node, "pixel");
	if (stfhdmimfd_access_ok(sc->sc_parent))
		stfhdmimfd_write(sc->sc_parent, HDMI_AV_MUTE,
		    HDMI_MUTE_AUDIO | HDMI_BLACK_VIDEO);
	/* Keep bus clocks live after opening the shared register window. */
	sc->sc_starting = 0;
}

int
stfhdmi_set_video(int on)
{
	struct stfhdmi_softc *sc = stfhdmi0;

	if (on && !stfdc8200_is_ready())
		return EAGAIN;
	if (sc == NULL || !sc->sc_ready ||
	    !stfhdmimfd_access_ok(sc->sc_parent))
		return ENXIO;
	stfhdmimfd_write(sc->sc_parent, HDMI_AV_MUTE,
	    on ? HDMI_MUTE_AUDIO : HDMI_MUTE_AUDIO | HDMI_BLACK_VIDEO);
	return 0;
}

