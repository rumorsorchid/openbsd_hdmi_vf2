/*	$OpenBSD$	*/
/*
 * StarFive JH7110 power-domain controller subset used by VOUT bring-up.
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
#include <dev/ofw/ofw_power.h>

#include "stfhdmivar.h"

#define JH7110_PMU_SW_TURN_ON		0x0c
#define JH7110_PMU_SW_TURN_OFF		0x10
#define JH7110_PMU_ENCOURAGE		0x44
#define JH7110_PMU_CURR_POWER_MODE	0x80

#define JH7110_PMU_ENCOURAGE_RESET	0xff
#define JH7110_PMU_ENCOURAGE_ON_LO	0x05
#define JH7110_PMU_ENCOURAGE_ON_HI	0x50
#define JH7110_PMU_ENCOURAGE_OFF_LO	0x0a
#define JH7110_PMU_ENCOURAGE_OFF_HI	0xa0

#define JH7110_PD_VOUT			4
#define JH7110_PMU_POLL_US		2
#define JH7110_PMU_TIMEOUT_US		200000

struct stfpmu_softc {
	struct device			sc_dev;
	bus_space_tag_t			sc_iot;
	bus_space_handle_t		sc_ioh;
	int				sc_node;
	struct power_domain_device	sc_pd;
};

static struct stfpmu_softc *stfpmu0;

int	stfpmu_match(struct device *, void *, void *);
void	stfpmu_attach(struct device *, struct device *, void *);
void	stfpmu_enable(void *, uint32_t *, int);

const struct cfattach stfpmu_ca = {
	sizeof(struct stfpmu_softc), stfpmu_match, stfpmu_attach
};

struct cfdriver stfpmu_cd = {
	NULL, "stfpmu", DV_DULL
};

static inline uint32_t
pmur(struct stfpmu_softc *sc, bus_size_t reg)
{
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh, reg);
}

static inline void
pmuw(struct stfpmu_softc *sc, bus_size_t reg, uint32_t val)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, reg, val);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, reg, 4,
	    BUS_SPACE_BARRIER_WRITE);
}

static int
stfpmu_set(struct stfpmu_softc *sc, unsigned int pd, int on)
{
	uint32_t mask, val;
	int waited;

	/* This bring-up driver intentionally owns only PD_VOUT. */
	if (pd != JH7110_PD_VOUT)
		return EINVAL;
	mask = 1U << pd;
	val = pmur(sc, JH7110_PMU_CURR_POWER_MODE);
	if (!!(val & mask) == !!on)
		return 0;

	pmuw(sc, on ? JH7110_PMU_SW_TURN_ON : JH7110_PMU_SW_TURN_OFF,
	    mask);
	/* Required JH7110 software-encourage state-machine sequence. */
	pmuw(sc, JH7110_PMU_ENCOURAGE, JH7110_PMU_ENCOURAGE_RESET);
	pmuw(sc, JH7110_PMU_ENCOURAGE,
	    on ? JH7110_PMU_ENCOURAGE_ON_LO : JH7110_PMU_ENCOURAGE_OFF_LO);
	pmuw(sc, JH7110_PMU_ENCOURAGE,
	    on ? JH7110_PMU_ENCOURAGE_ON_HI : JH7110_PMU_ENCOURAGE_OFF_HI);

	for (waited = 0; waited < JH7110_PMU_TIMEOUT_US;
	    waited += JH7110_PMU_POLL_US) {
		val = pmur(sc, JH7110_PMU_CURR_POWER_MODE);
		if (!!(val & mask) == !!on)
			return 0;
		delay(JH7110_PMU_POLL_US);
	}
	return ETIMEDOUT;
}

int
stfpmu_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "starfive,jh7110-pmu");
}

void
stfpmu_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfpmu_softc *sc = (struct stfpmu_softc *)self;
	struct fdt_attach_args *faa = aux;

	if (faa->fa_nreg < 1 || faa->fa_reg[0].size < 0x84) {
		printf(": invalid register window\n");
		return;
	}
	sc->sc_iot = faa->fa_iot;
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr,
	    faa->fa_reg[0].size, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}
	sc->sc_node = faa->fa_node;
	sc->sc_pd.pd_node = sc->sc_node;
	sc->sc_pd.pd_cookie = sc;
	sc->sc_pd.pd_enable = stfpmu_enable;
	power_domain_register(&sc->sc_pd);
	if (stfpmu0 == NULL)
		stfpmu0 = sc;
	printf(": JH7110 PMU\n");
}

void
stfpmu_enable(void *cookie, uint32_t *cells, int on)
{
	struct stfpmu_softc *sc = cookie;
	int error;

	error = stfpmu_set(sc, cells[0], on);
	if (error)
		printf("%s: power domain %u %s failed (%d)\n",
		    sc->sc_dev.dv_xname, cells[0], on ? "on" : "off", error);
}

int
stfpmu_vout_enable(void)
{
	if (stfpmu0 == NULL)
		return ENXIO;
	return stfpmu_set(stfpmu0, JH7110_PD_VOUT, 1);
}
