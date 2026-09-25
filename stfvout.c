/*	$OpenBSD$	*/
/* StarFive JH7110 VOUT domain parent.  SPDX-License-Identifier: ISC */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/fdt.h>
#include <machine/simplebusvar.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>

#include "stfhdmivar.h"

struct stfvout_softc {
	struct simplebus_softc sc_sbus;
};

int	stfvout_match(struct device *, void *, void *);
void	stfvout_attach(struct device *, struct device *, void *);

const struct cfattach stfvout_ca = {
	sizeof(struct stfvout_softc), stfvout_match, stfvout_attach
};
struct cfdriver stfvout_cd = { NULL, "stfvout", DV_DULL };

int
stfvout_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;
	return OF_is_compatible(faa->fa_node, "starfive,jh7110-vout-subsystem");
}

void
stfvout_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfvout_softc *sc = (struct stfvout_softc *)self;
	struct fdt_attach_args *faa = aux;
	int error;

	/* Do not enumerate children until the domain and NoC path are safe. */
	error = stfpmu_vout_enable();
	if (error) {
		printf(": can't enable PD_VOUT (%d)\n", error);
		return;
	}
	clock_enable_all(faa->fa_node);
	reset_deassert_all(faa->fa_node);
	delay(10);
	if (clock_get_frequency_idx(faa->fa_node, 0) == 0) {
		printf(": display NoC clock did not start\n");
		return;
	}
	printf(": PD_VOUT on, display NoC open\n");
	simplebus_attach(parent, &sc->sc_sbus.sc_dev, faa);
}
