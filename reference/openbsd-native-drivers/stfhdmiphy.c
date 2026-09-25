/*	$OpenBSD$	*/
/*
 * StarFive JH7110 Innosilicon HDMI PHY / pixel-clock provider.
 *
 * Initial bring-up scope is intentionally limited to the two integer pre-PLL
 * cases needed for CEA 720p60 and 1080p60.  Register accesses are expressed
 * independently for OpenBSD from the public JH7110/RK Innosilicon register
 * behaviour; this is not a Linux driver translation.
 *
 * The pre-PLL supplies the pixel clock.  The post-PLL and analog chain are
 * powered separately by the HDMI controller only after the pre-PLL has locked.
 * All lock waits are bounded at 100 ms.
 *
 * SPDX-License-Identifier: ISC
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/errno.h>

#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>

#include "stfhdmivar.h"

/* Pre-PLL: JH7110 PHY is the common Innosilicon block at logical +0x100. */
#define PHY_PRE_CTRL			0x1a0
#define  PHY_PRE_POWER_DOWN		(1U << 0)
#define  PHY_PRE_VCO_DIV5		(1U << 1)
#define PHY_PRE_DIV1			0x1a1
#define PHY_PRE_DIV2			0x1a2
#define  PHY_PRE_SS_DISABLE		(1U << 6)
#define  PHY_PRE_FRAC_DISABLE		(3U << 4)
#define PHY_PRE_DIV3			0x1a3
#define PHY_PRE_TMDS_DIV		0x1a4
#define PHY_PRE_PCLK_AB			0x1a5
#define PHY_PRE_PCLK_CD			0x1a6
#define PHY_PRE_LOCK			0x1a9
#define  PHY_PRE_LOCKED			(1U << 0)
#define PHY_PRE_FRAC_H			0x1d1
#define PHY_PRE_FRAC_M			0x1d2
#define PHY_PRE_FRAC_L			0x1d3

/* Post-PLL / analog. */
#define PHY_POST_DIV1			0x1aa
#define  PHY_POST_DIV_ENABLE(x)	(((x) & 3) << 2)
#define  PHY_POST_REF_TMDS		(1U << 1)
#define  PHY_POST_POWER_DOWN		(1U << 0)
#define PHY_POST_DIV2			0x1ab
#define PHY_POST_DIV3			0x1ac
#define PHY_POST_DIV4			0x1ad
#define PHY_POST_LOCK			0x1af
#define  PHY_POST_LOCKED		(1U << 0)
#define PHY_BIAS_CTRL			0x1b0
#define  PHY_BIAS_ENABLE		(1U << 2)
#define PHY_TMDS_CTRL			0x1b2
#define PHY_LDO_CTRL			0x1b4
#define  PHY_LDO_ENABLE			0x07
#define PHY_SERIALIZER_CTRL		0x1be
#define  PHY_SERIALIZER_ENABLE		0x71
#define PHY_RX_CTRL			0x1cc
#define  PHY_RX_ENABLE			0x0f

#define PHY_LOCK_POLL_US		1000
#define PHY_LOCK_TIMEOUT_US		100000

struct stfhdmiphy_cfg {
	uint32_t rate;
	uint8_t prediv;
	uint16_t fbdiv;
	uint8_t tmds_a, tmds_b, tmds_c;
	uint8_t pclk_a, pclk_b, pclk_c, pclk_d;
	uint8_t vco_div5;
};

static const struct stfhdmiphy_cfg stfhdmiphy_cfgs[] = {
	/* 74.25 MHz: integer mode, TMDS == pixel clock. */
	{ 74250000, 1, 99, 1, 2, 2, 1, 2, 3, 4, 0 },
	/* 148.5 MHz: integer mode, TMDS == pixel clock. */
	{ 148500000, 1, 99, 1, 1, 1, 1, 2, 2, 2, 0 },
};

struct stfhdmiphy_softc {
	struct device		sc_dev;
	struct device		*sc_parent;
	int			sc_node;
	struct clock_device	sc_cd;
	uint32_t		sc_rate;
	int			sc_locked;
	int			sc_post_on;
};

static struct stfhdmiphy_softc *stfhdmiphy0;

int	stfhdmiphy_match(struct device *, void *, void *);
void	stfhdmiphy_attach(struct device *, struct device *, void *);
uint32_t stfhdmiphy_get_frequency(void *, uint32_t *);
int	stfhdmiphy_set_frequency(void *, uint32_t *, uint32_t);
void	stfhdmiphy_enable(void *, uint32_t *, int);

const struct cfattach stfhdmiphy_ca = {
	sizeof(struct stfhdmiphy_softc), stfhdmiphy_match, stfhdmiphy_attach
};

struct cfdriver stfhdmiphy_cd = {
	NULL, "stfhdmiphy", DV_DULL
};

static const struct stfhdmiphy_cfg *
stfhdmiphy_lookup(uint32_t rate)
{
	int i;

	for (i = 0; i < nitems(stfhdmiphy_cfgs); i++)
		if (stfhdmiphy_cfgs[i].rate == rate)
			return &stfhdmiphy_cfgs[i];
	return NULL;
}

static inline uint8_t
phyr(struct stfhdmiphy_softc *sc, uint16_t reg)
{
	return stfhdmimfd_read(sc->sc_parent, reg);
}

static inline void
phyw(struct stfhdmiphy_softc *sc, uint16_t reg, uint8_t val)
{
	stfhdmimfd_write(sc->sc_parent, reg, val);
}

static inline void
phym(struct stfhdmiphy_softc *sc, uint16_t reg, uint8_t mask, uint8_t val)
{
	stfhdmimfd_update(sc->sc_parent, reg, mask, val);
}

static int
stfhdmiphy_wait(struct stfhdmiphy_softc *sc, uint16_t reg, uint8_t mask,
    const char *what)
{
	int waited;

	for (waited = 0; waited < PHY_LOCK_TIMEOUT_US;
	    waited += PHY_LOCK_POLL_US) {
		if (phyr(sc, reg) & mask)
			return 0;
		delay(PHY_LOCK_POLL_US);
	}
	printf("%s: timeout waiting for %s lock\n", sc->sc_dev.dv_xname, what);
	return ETIMEDOUT;
}

static void
stfhdmiphy_pre_power(struct stfhdmiphy_softc *sc, int on)
{
	phym(sc, PHY_PRE_CTRL, PHY_PRE_POWER_DOWN,
	    on ? 0 : PHY_PRE_POWER_DOWN);
}

static void
stfhdmiphy_program_pre(struct stfhdmiphy_softc *sc,
    const struct stfhdmiphy_cfg *cfg)
{
	uint8_t val;

	/* Program only while the pre-PLL is held down. */
	stfhdmiphy_pre_power(sc, 0);
	phym(sc, PHY_PRE_CTRL, PHY_PRE_VCO_DIV5,
	    cfg->vco_div5 ? PHY_PRE_VCO_DIV5 : 0);

	phyw(sc, PHY_PRE_DIV1, cfg->prediv & 0x3f);
	val = PHY_PRE_SS_DISABLE | PHY_PRE_FRAC_DISABLE |
	    ((cfg->fbdiv >> 8) & 0x0f);
	phyw(sc, PHY_PRE_DIV2, val);
	phyw(sc, PHY_PRE_DIV3, cfg->fbdiv & 0xff);

	phyw(sc, PHY_PRE_PCLK_AB,
	    ((cfg->pclk_b & 3) << 5) | (cfg->pclk_a & 0x1f));
	phyw(sc, PHY_PRE_PCLK_CD,
	    ((cfg->pclk_c & 3) << 5) | (cfg->pclk_d & 0x1f));
	phyw(sc, PHY_PRE_TMDS_DIV,
	    ((cfg->tmds_a & 3) << 4) | ((cfg->tmds_b & 3) << 2) |
	    (cfg->tmds_c & 3));

	/* Both initial rates are integer cases. */
	phyw(sc, PHY_PRE_FRAC_H, 0);
	phyw(sc, PHY_PRE_FRAC_M, 0);
	phyw(sc, PHY_PRE_FRAC_L, 0);
}

int
stfhdmiphy_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node,
	    "starfive,jh7110-inno-hdmi-phy");
}

void
stfhdmiphy_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfhdmiphy_softc *sc = (struct stfhdmiphy_softc *)self;
	struct fdt_attach_args *faa = aux;
	uint32_t ref;

	sc->sc_parent = parent;
	sc->sc_node = faa->fa_node;

	/* The PHY itself depends only on xin24m; no shared HDMI register access. */
	clock_enable_all(sc->sc_node);
	ref = clock_get_frequency_idx(sc->sc_node, 0);
	if (ref != 24000000) {
		printf(": unsupported reference clock %u Hz (need 24000000)\n", ref);
		return;
	}

	sc->sc_cd.cd_node = sc->sc_node;
	sc->sc_cd.cd_cookie = sc;
	sc->sc_cd.cd_get_frequency = stfhdmiphy_get_frequency;
	sc->sc_cd.cd_set_frequency = stfhdmiphy_set_frequency;
	sc->sc_cd.cd_set_parent = NULL;
	sc->sc_cd.cd_enable = stfhdmiphy_enable;
	clock_register(&sc->sc_cd);

	if (stfhdmiphy0 == NULL)
		stfhdmiphy0 = sc;

	printf(": Innosilicon HDMI PHY, ref %u Hz, 74.25/148.5 MHz only\n",
	    ref);
	stfhdmi_try_start();
}


int
stfhdmiphy_available(void)
{
	return stfhdmiphy0 != NULL;
}

uint32_t
stfhdmiphy_get_frequency(void *cookie, uint32_t *cells)
{
	struct stfhdmiphy_softc *sc = cookie;

	/* Never read PLL registers here; an unclocked shared-window read can hang. */
	return sc->sc_rate;
}

int
stfhdmiphy_set_frequency(void *cookie, uint32_t *cells, uint32_t rate)
{
	struct stfhdmiphy_softc *sc = cookie;
	const struct stfhdmiphy_cfg *cfg;

	cfg = stfhdmiphy_lookup(rate);
	if (cfg == NULL)
		return -1;
	if (!stfhdmimfd_access_ok(sc->sc_parent))
		return -1;

	/* Bias/RX are needed while the PLL is configured. */
	phym(sc, PHY_BIAS_CTRL, PHY_BIAS_ENABLE, PHY_BIAS_ENABLE);
	phyw(sc, PHY_RX_CTRL, PHY_RX_ENABLE);

	stfhdmiphy_program_pre(sc, cfg);
	stfhdmiphy_pre_power(sc, 1);

	sc->sc_rate = rate;
	sc->sc_locked = 0;
	return 0;
}

void
stfhdmiphy_enable(void *cookie, uint32_t *cells, int on)
{
	struct stfhdmiphy_softc *sc = cookie;

	if (!stfhdmimfd_access_ok(sc->sc_parent))
		return;

	if (!on) {
		stfhdmiphy_pre_power(sc, 0);
		sc->sc_locked = 0;
		return;
	}

	if (stfhdmiphy_lookup(sc->sc_rate) == NULL) {
		printf("%s: enable without supported pixel rate\n",
		    sc->sc_dev.dv_xname);
		return;
	}

	stfhdmiphy_pre_power(sc, 1);
	if (stfhdmiphy_wait(sc, PHY_PRE_LOCK, PHY_PRE_LOCKED, "pre-PLL") != 0) {
		stfhdmiphy_pre_power(sc, 0);
		sc->sc_locked = 0;
		return;
	}
	sc->sc_locked = 1;
}

int
stfhdmiphy_clock_stable(uint32_t rate)
{
	return stfhdmiphy0 != NULL && stfhdmiphy0->sc_rate == rate &&
	    stfhdmiphy0->sc_locked;
}

static void
stfhdmiphy_analog_off(struct stfhdmiphy_softc *sc)
{
	phyw(sc, PHY_TMDS_CTRL, 0x00);
	phyw(sc, PHY_SERIALIZER_CTRL, 0x00);
	phyw(sc, PHY_LDO_CTRL, 0x00);
	phym(sc, PHY_BIAS_CTRL, PHY_BIAS_ENABLE, 0x00);
	phyw(sc, PHY_RX_CTRL, 0x00);
	phym(sc, PHY_POST_DIV1, PHY_POST_POWER_DOWN, PHY_POST_POWER_DOWN);
	sc->sc_post_on = 0;
}

int
stfhdmiphy_power_on(uint32_t rate)
{
	struct stfhdmiphy_softc *sc = stfhdmiphy0;
	uint8_t post_div1;

	if (sc == NULL || !stfhdmimfd_access_ok(sc->sc_parent))
		return ENXIO;
	if (!stfhdmiphy_clock_stable(rate))
		return EIO;
	if (stfhdmiphy_lookup(rate) == NULL)
		return EINVAL;

	/* Both 74.25 and 148.5 MHz use post: prediv=1, fbdiv=20, postdiv=1. */
	phym(sc, PHY_BIAS_CTRL, PHY_BIAS_ENABLE, PHY_BIAS_ENABLE);
	phyw(sc, PHY_RX_CTRL, PHY_RX_ENABLE);
	phyw(sc, PHY_POST_DIV2, 1);
	phyw(sc, PHY_POST_DIV3, 20);
	phyw(sc, PHY_POST_DIV4, 1);

	post_div1 = PHY_POST_DIV_ENABLE(3) | PHY_POST_REF_TMDS;
	phyw(sc, PHY_POST_DIV1, post_div1);

	if (stfhdmiphy_wait(sc, PHY_POST_LOCK, PHY_POST_LOCKED,
	    "post-PLL") != 0) {
		stfhdmiphy_analog_off(sc);
		return ETIMEDOUT;
	}

	phyw(sc, PHY_LDO_CTRL, PHY_LDO_ENABLE);
	phyw(sc, PHY_SERIALIZER_CTRL, PHY_SERIALIZER_ENABLE);
	/* Upper drive-control bit plus all four TMDS drivers. */
	phyw(sc, PHY_TMDS_CTRL, 0x8f);
	sc->sc_post_on = 1;
	return 0;
}

void
stfhdmiphy_power_off(void)
{
	if (stfhdmiphy0 != NULL &&
	    stfhdmimfd_access_ok(stfhdmiphy0->sc_parent))
		stfhdmiphy_analog_off(stfhdmiphy0);
}
