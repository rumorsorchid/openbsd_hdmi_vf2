/*	$OpenBSD$	*/
/* StarFive JH7110 native HDMI/display bring-up glue. SPDX-License-Identifier: ISC */
#ifndef _RISCV64_DEV_STFHDMIVAR_H_
#define _RISCV64_DEV_STFHDMIVAR_H_

#include <sys/types.h>
#include <sys/device.h>
#include <machine/bus.h>

struct stf_video_mode {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint8_t vic;
};

/* PMU/VOUT providers. */
int	stfpmu_vout_enable(void);
int	stfvoutcrg_available(void);
int	stfvoutcrg_resets_deasserted(uint32_t);

/* Shared HDMI register parent. Logical byte registers are 32-bit spaced. */
uint8_t stfhdmimfd_read(struct device *, uint16_t);
void	stfhdmimfd_write(struct device *, uint16_t, uint8_t);
void	stfhdmimfd_update(struct device *, uint16_t, uint8_t, uint8_t);
void	stfhdmimfd_set_access(struct device *, int);
int	stfhdmimfd_access_ok(struct device *);
bus_space_tag_t stfhdmimfd_iot(struct device *);

/* Innosilicon PHY. */
int	stfhdmiphy_available(void);
int	stfhdmiphy_power_on(uint32_t);
void	stfhdmiphy_power_off(void);
int	stfhdmiphy_clock_stable(uint32_t);

/* Attach-order-independent HDMI/DC handoff. */
const struct stf_video_mode *stfhdmi_mode(void);
int	stfhdmi_is_ready(void);
void	stfhdmi_try_start(void);
int	stfhdmi_set_video(int);
int	stfdc8200_is_ready(void);
void	stfdc8200_hdmi_ready(const struct stf_video_mode *);

#endif
