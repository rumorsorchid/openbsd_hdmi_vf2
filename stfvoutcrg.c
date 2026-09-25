/*	$OpenBSD$	*/
/* StarFive JH7110 VOUT clock/reset controller subset. SPDX-License-Identifier: ISC */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/errno.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>
#include <dev/ofw/ofw_power.h>

#include "stfhdmivar.h"

#define VOUT_CLK_APB		0
#define VOUT_CLK_DC8200_PIX	1
#define VOUT_CLK_DC8200_AXI	4
#define VOUT_CLK_DC8200_CORE	5
#define VOUT_CLK_DC8200_AHB	6
#define VOUT_CLK_DC8200_PIX0	7
#define VOUT_CLK_DC8200_PIX1	8
#define VOUT_CLK_HDMI_MCLK	15
#define VOUT_CLK_HDMI_BCLK	16
#define VOUT_CLK_HDMI_SYS	17

#define VOUT_CLK_ENABLE		(1U << 31)
#define VOUT_CLK_MUX_MASK	(3U << 24)
#define VOUT_CLK_MUX_SHIFT	24
#define VOUT_CLK_MUX_HDMI	1U
#define VOUT_CLK_DIV_MASK	0x00ffffffU

#define VOUT_RST_ASSERT		0x48
#define VOUT_RST_STATUS		0x4c
#define VOUT_RST_COUNT		12
#define VOUT_RST_MASK		((1U << VOUT_RST_COUNT) - 1)

struct stfvoutcrg_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_size_t		sc_size;
	int			sc_node;
	struct clock_device	sc_cd;
	struct reset_device	sc_rd;
	int			sc_ready;
};

static struct stfvoutcrg_softc *stfvoutcrg0;

int	stfvoutcrg_match(struct device *, void *, void *);
void	stfvoutcrg_attach(struct device *, struct device *, void *);
uint32_t stfvoutcrg_get_frequency(void *, uint32_t *);
int	stfvoutcrg_set_frequency(void *, uint32_t *, uint32_t);
int	stfvoutcrg_set_parent(void *, uint32_t *, uint32_t *);
void	stfvoutcrg_enable(void *, uint32_t *, int);
void	stfvoutcrg_reset(void *, uint32_t *, int);

const struct cfattach stfvoutcrg_ca = {
	sizeof(struct stfvoutcrg_softc), stfvoutcrg_match, stfvoutcrg_attach
};
struct cfdriver stfvoutcrg_cd = { NULL, "stfvoutcrg", DV_DULL };

static inline uint32_t
vcr(struct stfvoutcrg_softc *sc, uint32_t idx)
{
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh, idx * 4);
}

static inline void
vcw(struct stfvoutcrg_softc *sc, uint32_t idx, uint32_t val)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, idx * 4, val);
}

static int
stfvoutcrg_select_hdmi(struct stfvoutcrg_softc *sc, uint32_t idx)
{
	uint32_t reg;

	reg = vcr(sc, idx);
	reg &= ~VOUT_CLK_MUX_MASK;
	reg |= VOUT_CLK_MUX_HDMI << VOUT_CLK_MUX_SHIFT;
	vcw(sc, idx, reg);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, idx * 4, 4,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	reg = vcr(sc, idx);
	if (((reg & VOUT_CLK_MUX_MASK) >> VOUT_CLK_MUX_SHIFT) !=
	    VOUT_CLK_MUX_HDMI)
		return EIO;
	return 0;
}

int
stfvoutcrg_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;
	return OF_is_compatible(faa->fa_node, "starfive,jh7110-voutcrg");
}

void
stfvoutcrg_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfvoutcrg_softc *sc = (struct stfvoutcrg_softc *)self;
	struct fdt_attach_args *faa = aux;

	if (faa->fa_nreg < 1 || faa->fa_reg[0].size < VOUT_RST_STATUS + 4) {
		printf(": invalid register window\n");
		return;
	}
	sc->sc_iot = faa->fa_iot;
	sc->sc_size = faa->fa_reg[0].size;
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr, sc->sc_size, 0,
	    &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}
	sc->sc_node = faa->fa_node;

	/*
	 * Keep the VOUT domain and the CRG's real SYSCRG parents live.  Do not
	 * enable hdmitx0_pixelclk here: the PHY has no selected rate yet and the
	 * HDMI controller owns that clock's set-rate/enable sequence.
	 */
	power_domain_enable(sc->sc_node);
	clock_enable(sc->sc_node, "vout_src");
	clock_enable(sc->sc_node, "vout_top_ahb");
	clock_enable(sc->sc_node, "vout_top_axi");
	clock_enable(sc->sc_node, "vout_top_hdmitx0_mclk");
	clock_enable(sc->sc_node, "i2stx0_bclk");
	reset_deassert_all(sc->sc_node);

	sc->sc_cd.cd_node = sc->sc_node;
	sc->sc_cd.cd_cookie = sc;
	sc->sc_cd.cd_get_frequency = stfvoutcrg_get_frequency;
	sc->sc_cd.cd_set_frequency = stfvoutcrg_set_frequency;
	sc->sc_cd.cd_set_parent = stfvoutcrg_set_parent;
	sc->sc_cd.cd_enable = stfvoutcrg_enable;
	clock_register(&sc->sc_cd);

	sc->sc_rd.rd_node = sc->sc_node;
	sc->sc_rd.rd_cookie = sc;
	sc->sc_rd.rd_reset = stfvoutcrg_reset;
	reset_register(&sc->sc_rd);
	clock_set_assigned(sc->sc_node);

	/*
	 * HDMI-only bring-up: PIX0/PIX1 must use the HDMI PHY pixel clock.  This
	 * is the September 2026 Linux v4 fix for a mux left at 0x80000000 by the
	 * bootloader instead of the required 0x81000000 selection.  The DC
	 * consumer also applies assigned-clock-parents; this direct pin plus
	 * readback is a bootloader-state-independent safety net.
	 */
	if (stfvoutcrg_select_hdmi(sc, VOUT_CLK_DC8200_PIX0) != 0 ||
	    stfvoutcrg_select_hdmi(sc, VOUT_CLK_DC8200_PIX1) != 0) {
		printf(": can't select/read back HDMI PHY pixel source\n");
		return;
	}
	sc->sc_ready = 1;
	if (stfvoutcrg0 == NULL)
		stfvoutcrg0 = sc;
	printf(": JH7110 VOUT CRG, DC8200 pixel source = HDMI PHY\n");
	stfhdmi_try_start();
}

uint32_t
stfvoutcrg_get_frequency(void *cookie, uint32_t *cells)
{
	struct stfvoutcrg_softc *sc = cookie;
	uint32_t idx = cells[0], reg, div, parent, pidx;

	if (idx > VOUT_CLK_HDMI_SYS)
		return 0;
	reg = vcr(sc, idx);
	switch (idx) {
	case VOUT_CLK_APB:
		div = reg & VOUT_CLK_DIV_MASK;
		parent = clock_get_frequency(sc->sc_node, "vout_top_ahb");
		return div ? parent / div : 0;
	case VOUT_CLK_DC8200_PIX:
		div = reg & VOUT_CLK_DIV_MASK;
		parent = clock_get_frequency(sc->sc_node, "vout_src");
		return div ? parent / div : 0;
	case VOUT_CLK_DC8200_AXI:
	case VOUT_CLK_DC8200_CORE:
		return clock_get_frequency(sc->sc_node, "vout_top_axi");
	case VOUT_CLK_DC8200_AHB:
		return clock_get_frequency(sc->sc_node, "vout_top_ahb");
	case VOUT_CLK_DC8200_PIX0:
	case VOUT_CLK_DC8200_PIX1:
		if (((reg & VOUT_CLK_MUX_MASK) >> VOUT_CLK_MUX_SHIFT) ==
		    VOUT_CLK_MUX_HDMI)
			return clock_get_frequency(sc->sc_node, "hdmitx0_pixelclk");
		pidx = VOUT_CLK_DC8200_PIX;
		return stfvoutcrg_get_frequency(sc, &pidx);
	case VOUT_CLK_HDMI_MCLK:
		return clock_get_frequency(sc->sc_node, "vout_top_hdmitx0_mclk");
	case VOUT_CLK_HDMI_BCLK:
		return clock_get_frequency(sc->sc_node, "i2stx0_bclk");
	case VOUT_CLK_HDMI_SYS:
		pidx = VOUT_CLK_APB;
		return stfvoutcrg_get_frequency(sc, &pidx);
	default:
		return 0;
	}
}

int
stfvoutcrg_set_frequency(void *cookie, uint32_t *cells, uint32_t freq)
{
	struct stfvoutcrg_softc *sc = cookie;
	uint32_t idx = cells[0], parent, maxdiv, div, reg;

	if (freq == 0)
		return -1;
	switch (idx) {
	case VOUT_CLK_APB:
		parent = clock_get_frequency(sc->sc_node, "vout_top_ahb");
		maxdiv = 8;
		break;
	case VOUT_CLK_DC8200_PIX:
		parent = clock_get_frequency(sc->sc_node, "vout_src");
		maxdiv = 63;
		break;
	default:
		return -1;
	}
	if (parent == 0)
		return -1;
	div = (parent + freq / 2) / freq;
	if (div < 1) div = 1;
	if (div > maxdiv) div = maxdiv;
	reg = vcr(sc, idx);
	reg = (reg & ~VOUT_CLK_DIV_MASK) | div;
	vcw(sc, idx, reg);
	return 0;
}

int
stfvoutcrg_set_parent(void *cookie, uint32_t *cells, uint32_t *pcells)
{
	struct stfvoutcrg_softc *sc = cookie;
	uint32_t idx = cells[0], reg, mux;
	int pnode;

	if (idx != VOUT_CLK_DC8200_PIX0 && idx != VOUT_CLK_DC8200_PIX1)
		return -1;
	/* pcells[0] is a provider phandle, not a numeric mux selector. */
	if (pcells[0] == OF_getpropint(sc->sc_node, "phandle", 0)) {
		if (pcells[1] != VOUT_CLK_DC8200_PIX)
			return -1;
		mux = 0;
	}
	else {
		pnode = OF_getnodebyphandle(pcells[0]);
		if (pnode == 0 || !OF_is_compatible(pnode,
		    "starfive,jh7110-inno-hdmi-phy"))
			return -1;
		mux = VOUT_CLK_MUX_HDMI;
	}
	reg = vcr(sc, idx) & ~VOUT_CLK_MUX_MASK;
	reg |= mux << VOUT_CLK_MUX_SHIFT;
	vcw(sc, idx, reg);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, idx * 4, 4,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	reg = vcr(sc, idx);
	if (((reg & VOUT_CLK_MUX_MASK) >> VOUT_CLK_MUX_SHIFT) != mux)
		return -1;
	return 0;
}

void
stfvoutcrg_enable(void *cookie, uint32_t *cells, int on)
{
	struct stfvoutcrg_softc *sc = cookie;
	uint32_t idx = cells[0], reg;

	switch (idx) {
	case VOUT_CLK_DC8200_PIX0:
	case VOUT_CLK_DC8200_PIX1:
		if (on && stfvoutcrg_select_hdmi(sc, idx) != 0)
			printf("%s: can't select HDMI PHY for pixel clock %u\n",
			    sc->sc_dev.dv_xname, idx);
		/* FALLTHROUGH */
	case VOUT_CLK_DC8200_AXI:
	case VOUT_CLK_DC8200_CORE:
	case VOUT_CLK_DC8200_AHB:
	case VOUT_CLK_HDMI_MCLK:
	case VOUT_CLK_HDMI_BCLK:
	case VOUT_CLK_HDMI_SYS:
		reg = vcr(sc, idx);
		if (on) reg |= VOUT_CLK_ENABLE;
		else reg &= ~VOUT_CLK_ENABLE;
		vcw(sc, idx, reg);
		break;
	case VOUT_CLK_APB:
	case VOUT_CLK_DC8200_PIX:
		break;
	default:
		printf("%s: unknown clock %u\n", sc->sc_dev.dv_xname, idx);
		break;
	}
}

/* JH7110 status is 0 while asserted and 1 after deassertion. */
static int
stfvoutcrg_reset_wait(struct stfvoutcrg_softc *sc, uint32_t mask,
    int assert)
{
	uint32_t done = assert ? 0 : mask;
	int i;

	for (i = 0; i < 1000; i++) {
		if ((bus_space_read_4(sc->sc_iot, sc->sc_ioh,
		    VOUT_RST_STATUS) & mask) == done)
			return 0;
		delay(1);
	}
	return ETIMEDOUT;
}

void
stfvoutcrg_reset(void *cookie, uint32_t *cells, int assert)
{
	struct stfvoutcrg_softc *sc = cookie;
	uint32_t idx = cells[0], bit, reg;

	/* This controller has one bank of 12 resets, not arbitrary banks. */
	if (idx >= VOUT_RST_COUNT) {
		printf("%s: invalid VOUT reset %u\n", sc->sc_dev.dv_xname, idx);
		return;
	}
	bit = 1U << idx;
	reg = bus_space_read_4(sc->sc_iot, sc->sc_ioh, VOUT_RST_ASSERT);
	if (assert) reg |= bit;
	else reg &= ~bit;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, VOUT_RST_ASSERT, reg);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, VOUT_RST_ASSERT, 8,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	if (stfvoutcrg_reset_wait(sc, bit, assert))
		printf("%s: VOUT reset %u %s timed out\n", sc->sc_dev.dv_xname,
		    idx, assert ? "assert" : "deassert");
}

/* The framework callback is void; consumers must check completion too. */
int
stfvoutcrg_resets_deasserted(uint32_t mask)
{
	if (!stfvoutcrg_available())
		return ENXIO;
	if (mask == 0 || (mask & ~VOUT_RST_MASK) != 0)
		return EINVAL;
	return stfvoutcrg_reset_wait(stfvoutcrg0, mask, 0);
}

int
stfvoutcrg_available(void)
{
	return stfvoutcrg0 != NULL && stfvoutcrg0->sc_ready;
}
