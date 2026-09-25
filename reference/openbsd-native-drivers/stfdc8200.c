/*	$OpenBSD$	*/
/*
 * Minimal VeriSilicon DC8200 scanout for StarFive JH7110.
 *
 * One output (panel 0), one XRGB8888 primary plane, one linear DMA buffer.
 * The buffer is mapped uncached and exposed through wsdisplay/wsfb.  This is
 * deliberately a console/framebuffer bring-up driver, not a DRM/KMS driver.
 *
 * SPDX-License-Identifier: ISC
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/errno.h>

#include <uvm/uvm_extern.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsdisplayvar.h>
#include <dev/rasops/rasops.h>

#include "stfhdmivar.h"

/* wsdisplay.c exports this so a late framebuffer does not claim console twice. */
extern int wsdisplay_console_initted;

#define DC_FB_ADDRESS			0x1400
#define DC_FB_STRIDE			0x1408
#define DC_DISP_HSIZE			0x1430
#define DC_DISP_HSYNC			0x1438
#define DC_DISP_VSIZE			0x1440
#define DC_DISP_VSYNC			0x1448
#define DC_DISP_PANEL_CONFIG		0x1418
#define DC_DISP_DPI_CONFIG		0x14b8
#define DC_FB_CONFIG			0x1518
#define DC_FB_SIZE			0x1810
#define DC_FB_CONFIG_EX			0x1cc0
#define DC_DISP_PANEL_START		0x1ccc
#define DC_DISP_DP_CONFIG		0x1cd0
#define DC_FB_TOP_LEFT			0x24d8
#define DC_FB_BOTTOM_RIGHT		0x24e0
#define DC_FB_BLEND_CONFIG		0x2510
#define DC_DISP_PANEL_CONFIG_EX	0x2518

#define DC_SYNC_ENABLE			(1U << 30)
#define DC_PANEL_DE_EN			(1U << 0)
#define DC_PANEL_DAT_EN			(1U << 4)
#define DC_PANEL_CLK_EN			(1U << 8)
#define DC_PANEL_RUNNING		(1U << 12)
#define DC_PANEL_GAMMA_ENABLE		(1U << 13)
#define DC_PANEL_OUTPUT_YUV		(1U << 16)
#define DC_DPI_RGB888			5
#define DC_DP_ENABLE			(1U << 3)
#define DC_PANEL_START_RUNNING0	(1U << 0)
#define DC_PANEL_START_MULTISYNC	(1U << 3)
#define DC_FB_FMT_X8R8G8B8		5
#define DC_FB_ENABLE			(1U << 13)
#define DC_FB_COMMIT			(1U << 12)
#define DC_FB_DISPLAY_ID		(1U << 19)
#define DC_BLEND_DISABLE		(1U << 1)
#define DC_PANEL_COMMIT			(1U << 0)

#define STFDC_ROWS			50
#define STFDC_COLS			160

struct stfdc8200_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_size_t		sc_regsize;
	bus_dma_tag_t		sc_dmat;
	bus_dma_segment_t	sc_seg;
	bus_dmamap_t		sc_map;
	caddr_t			sc_kva;
	bus_size_t		sc_fbsize;
	bus_size_t		sc_stride;
	int			sc_nsegs;
	int			sc_node;
	int			sc_started;
	int			sc_wsattached;
	int			sc_mode;
	int			sc_video;

	struct rasops_info	sc_ri;
	struct wsscreen_descr	sc_wsd;
	struct wsscreen_list	sc_wsl;
	struct wsscreen_descr	*sc_scrlist[1];
};

static struct stfdc8200_softc *stfdc0;

int	stfdc8200_match(struct device *, void *, void *);
void	stfdc8200_attach(struct device *, struct device *, void *);
int	stfdc8200_wsioctl(void *, u_long, caddr_t, int, struct proc *);
paddr_t	stfdc8200_wsmmap(void *, off_t, int);
int	stfdc8200_alloc_screen(void *, const struct wsscreen_descr *,
	    void **, int *, int *, uint32_t *);

const struct cfattach stfdc8200_ca = {
	sizeof(struct stfdc8200_softc), stfdc8200_match, stfdc8200_attach
};

struct cfdriver stfdc8200_cd = {
	NULL, "stfdc8200", DV_DULL
};

struct wsdisplay_accessops stfdc8200_accessops = {
	.ioctl = stfdc8200_wsioctl,
	.mmap = stfdc8200_wsmmap,
	.alloc_screen = stfdc8200_alloc_screen,
	.free_screen = rasops_free_screen,
	.show_screen = rasops_show_screen,
	.getchar = rasops_getchar,
	.load_font = rasops_load_font,
	.list_font = rasops_list_font,
	.scrollback = rasops_scrollback,
};

static inline uint32_t
dcr(struct stfdc8200_softc *sc, bus_size_t reg)
{
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh, reg);
}

static inline void
dcw(struct stfdc8200_softc *sc, bus_size_t reg, uint32_t val)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, reg, val);
}

int
stfdc8200_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "starfive,jh7110-dc8200");
}

static int
stfdc8200_alloc_fb(struct stfdc8200_softc *sc,
    const struct stf_video_mode *m)
{
	int error;

	sc->sc_stride = m->hdisplay * 4;
	sc->sc_fbsize = round_page(sc->sc_stride * m->vdisplay);

	error = bus_dmamap_create(sc->sc_dmat, sc->sc_fbsize, 1,
	    sc->sc_fbsize, 0, BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, &sc->sc_map);
	if (error)
		return error;

	error = bus_dmamem_alloc_range(sc->sc_dmat, sc->sc_fbsize, PAGE_SIZE,
	    0, &sc->sc_seg, 1, &sc->sc_nsegs, BUS_DMA_WAITOK | BUS_DMA_ZERO,
	    0, (paddr_t)0xffffffffU);
	if (error)
		goto fail_map;

	error = bus_dmamem_map(sc->sc_dmat, &sc->sc_seg, sc->sc_nsegs,
	    sc->sc_fbsize, &sc->sc_kva, BUS_DMA_WAITOK | BUS_DMA_NOCACHE);
	if (error)
		goto fail_mem;

	error = bus_dmamap_load(sc->sc_dmat, sc->sc_map, sc->sc_kva,
	    sc->sc_fbsize, NULL, BUS_DMA_WAITOK | BUS_DMA_WRITE);
	if (error)
		goto fail_kva;

	if (sc->sc_map->dm_nsegs != 1 ||
	    sc->sc_map->dm_segs[0].ds_addr > 0xffffffffU ||
	    sc->sc_map->dm_segs[0].ds_addr + sc->sc_fbsize - 1 > 0xffffffffU) {
		error = EFBIG;
		goto fail_load;
	}
	return 0;

fail_load:
	bus_dmamap_unload(sc->sc_dmat, sc->sc_map);
fail_kva:
	bus_dmamem_unmap(sc->sc_dmat, sc->sc_kva, sc->sc_fbsize);
fail_mem:
	bus_dmamem_free(sc->sc_dmat, &sc->sc_seg, sc->sc_nsegs);
fail_map:
	bus_dmamap_destroy(sc->sc_dmat, sc->sc_map);
	return error;
}

static int
stfdc8200_console_requested(void)
{
	int chosen;

	chosen = OF_finddevice("/chosen");
	if (chosen == 0 || chosen == -1)
		return 1;
	return OF_getpropint(chosen, "openbsd,hdmi-console", 1) != 0;
}

static void
stfdc8200_paint_bars(struct stfdc8200_softc *sc,
    const struct stf_video_mode *m)
{
	static const uint32_t colors[8] = {
		0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
		0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000
	};
	uint32_t *fb = (uint32_t *)sc->sc_kva;
	unsigned int x, y, n;

	for (y = 0; y < m->vdisplay; y++) {
		for (x = 0; x < m->hdisplay; x++) {
			n = ((uint64_t)x * 8) / m->hdisplay;
			fb[y * (sc->sc_stride / 4) + x] = colors[n > 7 ? 7 : n];
		}
	}
	bus_dmamap_sync(sc->sc_dmat, sc->sc_map, 0, sc->sc_fbsize,
	    BUS_DMASYNC_PREWRITE);
}

static int
stfdc8200_attach_wsdisplay(struct stfdc8200_softc *sc,
    const struct stf_video_mode *m)
{
	struct rasops_info *ri = &sc->sc_ri;
	struct wsemuldisplaydev_attach_args waa;
	uint32_t defattr = 0;
	int console, error;

	if (sc->sc_wsattached)
		return 0;
	console = stfdc8200_console_requested();
	if (console && wsdisplay_console_initted) {
		printf("%s: wsdisplay console already exists; attaching HDMI as non-console\n",
		    sc->sc_dev.dv_xname);
		console = 0;
	}

	ri->ri_width = m->hdisplay;
	ri->ri_height = m->vdisplay;
	ri->ri_stride = sc->sc_stride;
	ri->ri_depth = 32;
	ri->ri_rpos = 16; ri->ri_rnum = 8;
	ri->ri_gpos = 8;  ri->ri_gnum = 8;
	ri->ri_bpos = 0;  ri->ri_bnum = 8;
	ri->ri_bits = sc->sc_kva;
	ri->ri_hw = sc;
	/* Start the console clean; software bars alone cannot prove scanout. */
	ri->ri_flg = RI_CENTER | RI_CLEAR | RI_FULLCLEAR | RI_WRONLY | RI_VCONS;
	error = rasops_init(ri, STFDC_ROWS, STFDC_COLS);
	if (error)
		return error;
	if (ri->ri_font == NULL || ri->ri_ops.pack_attr == NULL)
		return ENXIO;

	strlcpy(sc->sc_wsd.name, "std", sizeof(sc->sc_wsd.name));
	sc->sc_wsd.capabilities = ri->ri_caps;
	sc->sc_wsd.nrows = ri->ri_rows;
	sc->sc_wsd.ncols = ri->ri_cols;
	sc->sc_wsd.textops = &ri->ri_ops;
	sc->sc_wsd.fontwidth = ri->ri_font->fontwidth;
	sc->sc_wsd.fontheight = ri->ri_font->fontheight;
	sc->sc_scrlist[0] = &sc->sc_wsd;
	sc->sc_wsl.nscreens = 1;
	sc->sc_wsl.screens = (const struct wsscreen_descr **)sc->sc_scrlist;

	if (console) {
		error = ri->ri_ops.pack_attr(ri->ri_active, 0, 0, 0, &defattr);
		if (error)
			return error;
		wsdisplay_cnattach(&sc->sc_wsd, ri->ri_active, 0, 0, defattr);
	}

	memset(&waa, 0, sizeof(waa));
	waa.scrdata = &sc->sc_wsl;
	waa.accessops = &stfdc8200_accessops;
	waa.accesscookie = ri;
	waa.console = console;
	if (config_found_sm(&sc->sc_dev, &waa, wsemuldisplaydevprint,
	    wsemuldisplaydevsubmatch) == NULL)
		return ENXIO;
	sc->sc_wsattached = 1;
	/* Retain an observable test image when UART remains the console. */
	if (!console)
		stfdc8200_paint_bars(sc, m);
	printf("%s: wsdisplay attached%s\n", sc->sc_dev.dv_xname,
	    console ? " as HDMI console" : "");
	return 0;
}

void
stfdc8200_attach(struct device *parent, struct device *self, void *aux)
{
	struct stfdc8200_softc *sc = (struct stfdc8200_softc *)self;
	struct fdt_attach_args *faa = aux;
	const struct stf_video_mode *m = stfhdmi_mode();
	int error;

	if (faa->fa_nreg < 1 || faa->fa_reg[0].size < 0x2520) {
		printf(": invalid register window\n");
		return;
	}
	sc->sc_iot = faa->fa_iot;
	sc->sc_dmat = faa->fa_dmat;
	sc->sc_node = faa->fa_node;
	sc->sc_regsize = faa->fa_reg[0].size;
	sc->sc_mode = WSDISPLAYIO_MODE_EMUL;
	sc->sc_video = WSDISPLAYIO_VIDEO_OFF;
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr,
	    sc->sc_regsize, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	/* Apply the Linux v4 PIX0/PIX1 HDMI-PHY parent assignment on its owner. */
	clock_set_assigned(sc->sc_node);

	error = stfdc8200_alloc_fb(sc, m);
	if (error) {
		printf(": can't allocate 32-bit scanout DMA (%d)\n", error);
		return;
	}

	printf(": %ux%u xrgb8888 fb 0x%llx\n", m->hdisplay, m->vdisplay,
	    (unsigned long long)sc->sc_map->dm_segs[0].ds_addr);
	stfdc8200_paint_bars(sc, m);
	if (stfdc0 == NULL)
		stfdc0 = sc;
	if (stfhdmi_is_ready())
		stfdc8200_hdmi_ready(m);
	else
		printf("%s: waiting for locked HDMI PHY/controller\n",
		    sc->sc_dev.dv_xname);
}

static uint32_t
stfdc_pos(uint32_t x, uint32_t y)
{
	return (x & 0x7fff) | ((y & 0x7fff) << 15);
}

static int
stfdc8200_start(struct stfdc8200_softc *sc,
    const struct stf_video_mode *m)
{
	uint32_t reg, freq;
	int error;

	if (sc->sc_started)
		return 0;
	if (!stfhdmi_is_ready())
		return EAGAIN;

	/* Do not call set_frequency here: HDMI already owns a locked PHY rate. */
	freq = clock_get_frequency(sc->sc_node, "pix0");
	if (freq != m->clock) {
		printf("%s: pix0 is %u Hz, expected %u Hz\n",
		    sc->sc_dev.dv_xname, freq, m->clock);
		return EINVAL;
	}

	clock_enable(sc->sc_node, "core");
	clock_enable(sc->sc_node, "axi");
	clock_enable(sc->sc_node, "ahb");
	clock_enable(sc->sc_node, "pix0");
	reset_deassert_all(sc->sc_node);
	if (stfvoutcrg_resets_deasserted(7U)) {
		printf("%s: DC reset incomplete; refusing DC MMIO\n",
		    sc->sc_dev.dv_xname);
		return ETIMEDOUT;
	}
	delay(10);

	/* Output 0 timing. Positive sync => DC polarity bits remain clear. */
	dcw(sc, DC_DISP_HSIZE, m->hdisplay | ((uint32_t)m->htotal << 16));
	dcw(sc, DC_DISP_HSYNC, m->hsync_start |
	    ((uint32_t)m->hsync_end << 15) | DC_SYNC_ENABLE);
	dcw(sc, DC_DISP_VSIZE, m->vdisplay | ((uint32_t)m->vtotal << 16));
	dcw(sc, DC_DISP_VSYNC, m->vsync_start |
	    ((uint32_t)m->vsync_end << 15) | DC_SYNC_ENABLE);

	reg = dcr(sc, DC_DISP_DP_CONFIG);
	reg &= ~DC_DP_ENABLE;
	dcw(sc, DC_DISP_DP_CONFIG, reg);
	reg = dcr(sc, DC_DISP_DPI_CONFIG);
	reg = (reg & ~7U) | DC_DPI_RGB888;
	dcw(sc, DC_DISP_DPI_CONFIG, reg);

	reg = dcr(sc, DC_DISP_PANEL_CONFIG);
	reg &= ~((1U << 1) | (1U << 5) | (1U << 9) |
	    DC_PANEL_GAMMA_ENABLE | DC_PANEL_OUTPUT_YUV);
	reg |= DC_PANEL_DE_EN | DC_PANEL_DAT_EN | DC_PANEL_CLK_EN |
	    DC_PANEL_RUNNING;
	dcw(sc, DC_DISP_PANEL_CONFIG, reg);
	reg = dcr(sc, DC_DISP_PANEL_START);
	reg &= ~DC_PANEL_START_MULTISYNC;
	reg |= DC_PANEL_START_RUNNING0;
	dcw(sc, DC_DISP_PANEL_START, reg);
	reg = dcr(sc, DC_DISP_PANEL_CONFIG_EX);
	dcw(sc, DC_DISP_PANEL_CONFIG_EX, reg | DC_PANEL_COMMIT);

	/* Make all diagnostic/rasops writes visible to non-coherent scanout. */
	bus_dmamap_sync(sc->sc_dmat, sc->sc_map, 0, sc->sc_fbsize,
	    BUS_DMASYNC_PREWRITE);

	/* One primary XRGB8888 plane, output 0. */
	dcw(sc, DC_FB_ADDRESS, (uint32_t)sc->sc_map->dm_segs[0].ds_addr);
	dcw(sc, DC_FB_STRIDE, sc->sc_stride);
	dcw(sc, DC_FB_CONFIG, DC_FB_FMT_X8R8G8B8 << 26);
	dcw(sc, DC_FB_TOP_LEFT, stfdc_pos(0, 0));
	dcw(sc, DC_FB_BOTTOM_RIGHT, stfdc_pos(m->hdisplay, m->vdisplay));
	dcw(sc, DC_FB_SIZE, stfdc_pos(m->hdisplay, m->vdisplay));
	dcw(sc, DC_FB_BLEND_CONFIG, DC_BLEND_DISABLE);
	reg = dcr(sc, DC_FB_CONFIG_EX);
	reg &= ~DC_FB_DISPLAY_ID;
	reg |= DC_FB_ENABLE | DC_FB_COMMIT;
	dcw(sc, DC_FB_CONFIG_EX, reg);

	bus_space_barrier(sc->sc_iot, sc->sc_ioh, 0, sc->sc_regsize,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	printf("%s: DC8200 scanout registers programmed\n", sc->sc_dev.dv_xname);
	error = stfdc8200_attach_wsdisplay(sc, m);
	if (error)
		goto fail;
	bus_dmamap_sync(sc->sc_dmat, sc->sc_map, 0, sc->sc_fbsize,
	    BUS_DMASYNC_PREWRITE);
	sc->sc_started = 1;
	error = stfhdmi_set_video(1);
	if (error)
		goto fail;
	sc->sc_video = WSDISPLAYIO_VIDEO_ON;
	printf("%s: HDMI video enabled; verify picture on monitor\n",
	    sc->sc_dev.dv_xname);
	return 0;

fail:
	sc->sc_started = 0;
	sc->sc_video = WSDISPLAYIO_VIDEO_OFF;
	(void)stfhdmi_set_video(0);
	/* Retain DMA storage: hardware may still have outstanding reads. */
	reg = dcr(sc, DC_FB_CONFIG_EX);
	dcw(sc, DC_FB_CONFIG_EX, (reg & ~DC_FB_ENABLE) | DC_FB_COMMIT);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, 0, sc->sc_regsize,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	return error;
}

void
stfdc8200_hdmi_ready(const struct stf_video_mode *m)
{
	int error;

	if (stfdc0 != NULL && (error = stfdc8200_start(stfdc0, m)) != 0)
		printf("%s: HDMI setup failed (%d); video remains blanked\n",
		    stfdc0->sc_dev.dv_xname, error);
}

int
stfdc8200_is_ready(void)
{
	return stfdc0 != NULL && stfdc0->sc_started && stfdc0->sc_wsattached;
}

int
stfdc8200_wsioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct rasops_info *ri = v;
	struct stfdc8200_softc *sc = ri->ri_hw;
	struct wsdisplay_fbinfo *wdf;

	switch (cmd) {
	case WSDISPLAYIO_GTYPE:
		/* Native DC8200 framebuffer, not firmware EFI framebuffer memory. */
		*(u_int *)data = WSDISPLAY_TYPE_UNKNOWN;
		return 0;
	case WSDISPLAYIO_GINFO:
		wdf = (struct wsdisplay_fbinfo *)data;
		wdf->width = ri->ri_width;
		wdf->height = ri->ri_height;
		wdf->depth = ri->ri_depth;
		wdf->stride = ri->ri_stride;
		wdf->offset = 0;
		wdf->cmsize = 0;
		return 0;
	case WSDISPLAYIO_LINEBYTES:
		*(u_int *)data = ri->ri_stride;
		return 0;
	case WSDISPLAYIO_GETSUPPORTEDDEPTH:
		*(u_int *)data = WSDISPLAYIO_DEPTH_24_32;
		return 0;
	case WSDISPLAYIO_GMODE:
		*(u_int *)data = sc->sc_mode;
		return 0;
	case WSDISPLAYIO_SMODE:
		switch (*(u_int *)data) {
		case WSDISPLAYIO_MODE_EMUL:
		case WSDISPLAYIO_MODE_MAPPED:
		case WSDISPLAYIO_MODE_DUMBFB:
			sc->sc_mode = *(u_int *)data;
			return 0;
		default:
			return EINVAL;
		}
	case WSDISPLAYIO_GVIDEO:
		*(u_int *)data = sc->sc_video;
		return 0;
	case WSDISPLAYIO_SVIDEO:
		if (*(u_int *)data != WSDISPLAYIO_VIDEO_ON &&
		    *(u_int *)data != WSDISPLAYIO_VIDEO_OFF)
			return EINVAL;
		if (stfhdmi_set_video(*(u_int *)data == WSDISPLAYIO_VIDEO_ON) != 0)
			return EIO;
		sc->sc_video = *(u_int *)data;
		return 0;
	default:
		return -1;
	}
}

paddr_t
stfdc8200_wsmmap(void *v, off_t off, int prot)
{
	struct rasops_info *ri = v;
	struct stfdc8200_softc *sc = ri->ri_hw;

	if (off < 0 || off >= sc->sc_fbsize || (off & PAGE_MASK) != 0)
		return -1;
	return bus_dmamem_mmap(sc->sc_dmat, &sc->sc_seg, sc->sc_nsegs,
	    off, prot, BUS_DMA_NOCACHE);
}

int
stfdc8200_alloc_screen(void *v, const struct wsscreen_descr *type,
    void **cookiep, int *curxp, int *curyp, uint32_t *attrp)
{
	return rasops_alloc_screen(v, cookiep, curxp, curyp, attrp);
}
