/*	$OpenBSD$	*/
/*
 * StarFive JH7110 HDMI shared-register parent.
 *
 * The JH7110 exposes the Innosilicon HDMI controller and PHY through one
 * register window.  Child-visible logical 8-bit registers are spaced four
 * bytes apart and must be accessed with 32-bit bus transactions.
 *
 * This parent deliberately does not read the register window while attaching.
 * The window is not safe to touch until the controller's VOUT system clock and
 * reset have been enabled; the controller child marks it accessible only then.
 *
 * SPDX-License-Identifier: ISC
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/bus.h>
#include <machine/fdt.h>
#include <machine/simplebusvar.h>

#include <dev/ofw/openfirm.h>

#include "stfhdmivar.h"

struct stfhdmimfd_softc {
	struct simplebus_softc	sc_sbus;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_size_t		sc_size;
	int			sc_access_ok;
};

int	stfhdmimfd_match(struct device *, void *, void *);
void	stfhdmimfd_attach(struct device *, struct device *, void *);

const struct cfattach stfhdmimfd_ca = {
	sizeof(struct stfhdmimfd_softc), stfhdmimfd_match, stfhdmimfd_attach
};

struct cfdriver stfhdmimfd_cd = {
	NULL, "stfhdmimfd", DV_DULL
};

int
stfhdmimfd_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node,
	    "starfive,jh7110-hdmi-subsystem");
}

void
stfhdmimfd_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfhdmimfd_softc *sc = (struct stfhdmimfd_softc *)self;
	struct fdt_attach_args *faa = aux;

	if (faa->fa_nreg < 1 || faa->fa_reg[0].size < 0x750) {
		printf(": no registers\n");
		return;
	}

	sc->sc_iot = faa->fa_iot;
	sc->sc_size = faa->fa_reg[0].size;
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr, sc->sc_size, 0,
	    &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	/* No register reads here: the HDMI system clock is not guaranteed yet. */
	sc->sc_access_ok = 0;
	printf(": shared HDMI registers\n");

	/* Enumerate the controller and PHY children from the split DT node. */
	simplebus_attach(parent, &sc->sc_sbus.sc_dev, faa);
}

static inline struct stfhdmimfd_softc *
stfhdmimfd_sc(struct device *dev)
{
	return (struct stfhdmimfd_softc *)dev;
}

uint8_t
stfhdmimfd_read(struct device *dev, uint16_t reg)
{
	struct stfhdmimfd_softc *sc = stfhdmimfd_sc(dev);

	if (!sc->sc_access_ok || ((bus_size_t)reg * 4 + 4) > sc->sc_size)
		return 0;
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh,
	    (bus_size_t)reg * 4) & 0xff;
}

void
stfhdmimfd_write(struct device *dev, uint16_t reg, uint8_t val)
{
	struct stfhdmimfd_softc *sc = stfhdmimfd_sc(dev);

	if (!sc->sc_access_ok || ((bus_size_t)reg * 4 + 4) > sc->sc_size)
		return;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh,
	    (bus_size_t)reg * 4, val);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, (bus_size_t)reg * 4, 4,
	    BUS_SPACE_BARRIER_WRITE);
}

void
stfhdmimfd_update(struct device *dev, uint16_t reg, uint8_t mask, uint8_t val)
{
	uint8_t tmp;

	if (!stfhdmimfd_access_ok(dev))
		return;
	tmp = stfhdmimfd_read(dev, reg);
	tmp &= ~mask;
	tmp |= val & mask;
	stfhdmimfd_write(dev, reg, tmp);
}

void
stfhdmimfd_set_access(struct device *dev, int on)
{
	struct stfhdmimfd_softc *sc = stfhdmimfd_sc(dev);

	sc->sc_access_ok = !!on;
}

int
stfhdmimfd_access_ok(struct device *dev)
{
	struct stfhdmimfd_softc *sc = stfhdmimfd_sc(dev);

	return sc->sc_access_ok;
}


bus_space_tag_t
stfhdmimfd_iot(struct device *dev)
{
	struct stfhdmimfd_softc *sc = stfhdmimfd_sc(dev);

	return sc->sc_iot;
}
