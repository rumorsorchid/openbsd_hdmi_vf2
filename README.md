# Blob-free HDMI firmware for OpenBSD on the VisionFive 2

This repository builds the VisionFive 2 boot firmware from source on your
Mac, with HDMI brought up by the firmware itself, like a PC BIOS. OpenBSD
then draws on that display with its **stock** `simplefb` and Xorg `wsfb`
drivers. The only OpenBSD change is a three-line kernel config, kept
current by one script.

```
mask ROM ─► U-Boot SPL ─► OpenSBI ─► U-Boot ─────────────► OpenBSD efiboot ─► kernel
 (in chip)  DDR init      M-mode     + jh7110_hdmi driver   GOP → simple-       simplefb
            (source)      firmware   HDMI up, console,      framebuffer node    wsdisplay
                                     EFI GOP                                    Xorg wsfb
```

| What runs on the board | Where it comes from |
|---|---|
| U-Boot SPL (incl. DDR init) | U-Boot `v2026.07`, built here from source |
| OpenSBI | OpenSBI `v1.9`, built here from source |
| U-Boot proper | U-Boot `v2026.07` + `firmware/` (this repo, GPL-2.0+) |
| OpenBSD | signed release sets and packages; kernel built on the board |

No blobs are flashed. The mask ROM in the SoC is the only code not built
here, and nothing can replace it.

What the screen shows, from power-on:

| Stage | HDMI | Serial console |
|---|---|---|
| U-Boot (2-second autoboot countdown, prompt) | yes, white on black | yes |
| OpenBSD boot loader (`boot>`) | yes | yes |
| kernel and `/etc/rc` | the loader's last screen, cleared when `simplefb` attaches | kernel messages |
| running system | X login (`xenodm`), text logins on Ctrl+Alt+F2 and up | serial login |

The kernel console stays on serial, because OpenBSD/riscv64 has no early
framebuffer console. Keep the USB serial adapter for emergencies.

## You need

- A Mac with Docker: Docker Desktop, or `brew install colima docker && colima start`.
- `python3` and `git`. macOS: `xcode-select --install`.
- The USB serial adapter on the VisionFive 2 debug UART (115200 8N1).
- OpenBSD 7.9 (or a later release) on the VisionFive 2's NVMe disk, with Xorg installed and internet access.

## The path

1. **Board:** prepare OpenBSD. Nothing changes in how it boots yet.
2. **Mac:** build the firmware.
3. **Test** the firmware over UART. Nothing is written to the board.
4. **Make it permanent:** select the new kernel, enable X, flash.

### 1. On the board: prepare OpenBSD

As root on the VisionFive 2, on the system as it is now (HFI or whatever
it runs):

```sh
cd /tmp
ftp -o vf2.tar.gz https://github.com/rumorsorchid/openbsd_hdmi_vf2/archive/refs/heads/main.tar.gz
tar -xzf vf2.tar.gz
cd openbsd_hdmi_vf2-main/openbsd
./setup.sh
vf2-kernel build
```

- `setup.sh` installs `vf2-kernel`, the `HDMIFB` kernel config and `xorg.conf`, and puts the stock `wsfb` driver back (your patched one expects the old DC8200 ioctl). It takes the driver from the release's signed `xserv` set. Anything it replaces is kept in `/root/vf2/`.
- `vf2-kernel build` fetches the kernel sources (the release's signed `sys.tar.gz` if `/usr/src/sys` is missing, then the `-stable` branch from anoncvs) and builds `/bsd-hdmi`: `GENERIC.MP` plus `simplefb`. The first build compiles the whole kernel and takes a while on the U74 cores; later builds only recompile what changed. It never touches `/bsd` and does not change what boots.
- anoncvs is reached over SSH. When `ssh` asks about the host key, compare it with <https://www.openbsd.org/anoncvs.html> first.
- If `/usr/src/sys` still carries changes of your own (the NATIVEBARS drivers, `sfcc.c`, `files.riscv64`), `vf2-kernel build` lists them and stops, because the kernel would no longer be stock. It prints the command that drops them (`cvs up -C`, which keeps each changed file as `.#name.revision`). To build with them anyway: `vf2-kernel build -m`.

### 2. On the Mac: build the firmware

```sh
git clone https://github.com/rumorsorchid/openbsd_hdmi_vf2.git
cd openbsd_hdmi_vf2
./vf2-firmware.sh build
```

- The first run creates the build container: a pinned Ubuntu image with packages from the `20260925T000000Z` Ubuntu snapshot. The build fetches OpenSBI and U-Boot and refuses to build if either tag is not the pinned commit.
- The images land in `out/`. The script compares their hashes with `firmware/EXPECTED-SHA256SUMS` and says `reproduced: ... bit for bit` when your build matches the reference exactly.
- `prebuilt/r2/` holds the same images. To use them without building, put `OUT=prebuilt/r2` in front of the commands below.
- Build options: `VF2_HDMI_MODE=720p` for 1280×720; `VF2_HDMI_PIXCLK=pll2` to clock the display controller the way HFI does. Non-default builds are not compared with the reference hashes.

### 3. Test over UART (nothing is written to the board)

1. Board **off**. Set the boot switches **RGPIO_0 and RGPIO_1 both to H**
   (UART boot). Close picocom or anything else holding the serial port.
2. Run, then **power the board on** when it says so:
   ```sh
   ls /dev/cu.usb*
   ./vf2-firmware.sh uart-boot /dev/cu.usbserial-140
   ```
   The mask ROM takes the SPL, the SPL brings up DRAM and takes
   `u-boot.itb` (about 2.5 minutes at 115200 baud), then you are in a serial
   console. Ctrl-] leaves it.
3. The serial log should show:
   ```
   jh7110-hdmi: 1920x1080@60, pixel clock from HDMI PHY, framebuffer 0xff800000
   jh7110-hdmi: running; CPU view 0x4ff800000 (uncached, DRAM + 16 GiB), DC8200 rev ...
   ```
   (the addresses may differ), and the HDMI screen shows the U-Boot
   console. A USB keyboard works there too. If it does not, type
   `usb reset` on the serial console: right after power-on a keyboard is
   sometimes missed.
4. U-Boot autoboots OpenBSD from the NVMe disk. At OpenBSD's `boot>` prompt
   (HDMI and serial) press space, then backspace, and type:
   ```
   boot /bsd-hdmi
   ```
5. The kernel's messages go to the serial console; the HDMI screen clears
   when `simplefb` attaches. Log in on serial, or on HDMI with Ctrl+Alt+F2,
   and check:
   ```sh
   dmesg | grep -E 'simplefb|wsdisplay'
   #   simplefb0 at mainbus0: 1920x1080, 32bpp
   #   wsdisplay0 at simplefb0 mux 1
   rcctl start xenodm
   ```
   X should come up on HDMI at 1920×1080, clean, with no stale lines.
   Full-screen redraws are slower than with cached memory; tell me how it
   feels.

A power cycle forgets the UART-booted firmware; with the switches still at
H you are back at step 3.2.

### 4. Make it permanent

While running `/bsd-hdmi` (step 3), on the board:

```sh
vf2-kernel activate        # boot.conf: set image /bsd-hdmi
rcctl enable xenodm        # graphical login on HDMI at every boot
```

Optionally, a text login on the first HDMI screen too (riscv64 leaves
ttyC0 without one):

```sh
sed -i '/^ttyC0[[:space:]]/s/[[:space:]]off[[:space:]]/ on  /' /etc/ttys
kill -HUP 1
```

Then flash the firmware:
1. Shut OpenBSD down, power the board off, and UART-boot again (step 3.2).
2. Press a key in the serial console to stop the autoboot at the
   `StarFive #` prompt, and leave the console with Ctrl-].
3. From the Mac, run:
   ```sh
   ./vf2-firmware.sh flash /dev/cu.usbserial-140
   ```

The flash command does the following:
- checks that the running U-Boot is this build;
- loads each image into RAM and compares its CRC32;
- writes it (SPL at `0x0`, FIT at `0x100000`);
- reads it back from flash and compares again.

Power off, set **RGPIO_0 and RGPIO_1 back to L** (flash boot), power on.
From now on every power-on shows the firmware on HDMI and boots into X.

## Keeping it current

- **Syspatches:** `syspatch`, then `vf2-kernel status`. It says whether a rebuild is due. If so, run `vf2-kernel build` and reboot. The previous kernel stays as `/bsd-hdmi.prev`. If the new one misbehaves, type `boot /bsd-hdmi.prev` at `boot>`, then run `vf2-kernel rollback`.
- **Packages** (Firefox etc.): `pkg_add -u`, prebuilt as usual.
- **A new release:** `vf2-kernel upgrade` instead of plain `sysupgrade` (arguments are passed on). It points `boot.conf` back at the stock `/bsd` first: the boot loader applies `set image` after it has chosen `/bsd.upgrade`, so with `/bsd-hdmi` still selected the upgrade would silently not happen. After the upgrade the stock kernel runs, which has no `simplefb`, so the HDMI screen keeps the boot loader's text. Log in on serial or over SSH, run `vf2-kernel build`, reboot, type `boot /bsd-hdmi` at `boot>`, then `vf2-kernel activate`.
- The firmware does not need to change for OpenBSD updates.

Never put `set tty fb0` in `/etc/boot.conf`. It makes the boot loader name
the framebuffer as the console. The riscv64 kernel only takes a serial port
as its console, so it would run with no console at all: no kernel messages,
no single-user shell. `vf2-kernel activate` removes such a line, and
`vf2-kernel status` warns about it.

### Optional: no kernel build at all

You build a kernel only because riscv64 `GENERIC` lacks `simplefb` (arm64
has it). `openbsd/upstream-mail.txt` is a ready proposal for
tech@openbsd.org that adds those two lines. Send it once HDMI works on your
board. If it is accepted, the stock `/bsd` shows HDMI with this firmware,
and `syspatch` and `sysupgrade` are all you need.

## If something goes wrong

**The screen stays dark in step 3:**
1. The serial log says why: a `jh7110-hdmi:` line names the step that failed
   (power domain, a reset, a PLL lock). Send it to me.
2. At the U-Boot prompt, type `hdmibars on`. The HDMI transmitter then sends
   its own colour bars, whatever the display controller does.
   - Bars on the monitor: the PHY, cable and monitor work, and the problem is in the display controller.
   - Still dark: the problem is in the transmitter, the PHY or the cable.

   `hdmibars off` goes back to the framebuffer.
3. Build with `VF2_HDMI_PIXCLK=pll2` (HFI's pixel clock) and UART-boot again.
4. Compare registers with HFI, where it works. In **HFI's** System Console
   (Ctrl-G on serial during POST) and at **this** U-Boot's prompt, run the same
   commands and send me both outputs:
   ```
   md.l 0x17030080 1
   md.l 0x130200e8 6
   md.l 0x13020294 1
   md.l 0x130202f8 6
   md.l 0x295c0000 12
   md.l 0x295c0048 2
   md.l 0x295b0004 2
   md.l 0x29400020 2
   md.l 0x29401400 30
   md.l 0x29401518 1
   md.l 0x29401810 1
   md.l 0x29401cc0 8
   md.l 0x294024d8 12
   md.l 0x29590000 16
   md.l 0x29590148 1
   md.l 0x29590320 3
   md.l 0x29590338 1
   md.l 0x29590680 34
   ```
   (`md` counts are hexadecimal. `hdmiregs` in this U-Boot prints the same
   registers with names.)

**The log says `no uncached view of DRAM found`:** U-Boot's own screen
still works, but OpenBSD's picture would show stale lines. Send me the log.

**OpenBSD misbehaves:** at `boot>`, type `boot /bsd` (stock, serial console).
- `vf2-kernel rollback` selects the previous kernel.
- Your old `wsfb` driver and `xorg.conf` are in `/root/vf2/`, and `boot.conf`'s previous version is `/etc/boot.conf.vf2-bak`.

**Back to HFI:** UART boot always works, whatever is in flash. To put HFI
back into flash:

```sh
python3 tools/hfifw-extract.py HFI-StarFive-VisionFive2-SysBIOS-1.4.hfifw hfi/
# UART-boot this build (step 3), stop at the prompt, leave with Ctrl-], then:
python3 tools/vf2uart.py flash /dev/cu.usbserial-140 hfi/hfi-spl.bin hfi/hfi-u-boot.itb --force
```

## Auditing

Every file, where it runs and what it may change:

| File | Runs | Does | Writes |
|---|---|---|---|
| `vf2-firmware.sh` | Mac | the one entry point: `build`, `uart-boot`, `flash`, `console` | `out/`; SPI flash only with `flash`, after you type `YES` |
| `firmware/build-in-container.sh` | build container | fetches OpenSBI and U-Boot (commits verified), adds the files below, builds | `out/` |
| `firmware/jh7110_hdmi.c` | in U-Boot, on the board | the HDMI driver | display registers; the framebuffer's reservation in the OS device tree |
| `firmware/u-boot.patch` | U-Boot source | Kconfig and Makefile entries, the driver's device-tree node, console on HDMI and USB keyboard | |
| `firmware/vf2-hdmi.config` | U-Boot config | video on, no settings read from flash, console look | |
| `firmware/EXPECTED-SHA256SUMS` | | the hashes a default build must reproduce | |
| `prebuilt/r2/` | | the default build's images, same bytes | |
| `tools/vf2uart.py` | Mac | XMODEM/YMODEM, UART boot, flash, console | the serial port; SPI flash through U-Boot's `sf` (flash only) |
| `tools/hfifw-extract.py` | Mac | splits an HFI package; only for going back to HFI | its output folder |
| `openbsd/setup.sh` | board, once | installs the three files below; stock `wsfb` from the signed `xserv` set | `/usr/local/sbin/vf2-kernel`, `/root/vf2/`, `/etc/X11/xorg.conf`, `wsfb_drv.so` |
| `openbsd/vf2-kernel` | board | builds and selects the HDMIFB kernel | `/usr/src/sys`, `/bsd-hdmi*`, `/etc/boot.conf` (keeps `.vf2-bak`), `/var/db/vf2-kernel.stamp`, `/var/log/vf2-kernel.log` |
| `openbsd/HDMIFB` | board | kernel config: `GENERIC.MP` + `simplefb` | |
| `openbsd/xorg.conf` | board | stock `wsfb` with ShadowFB | |
| `openbsd/upstream-mail.txt` | | optional proposal to OpenBSD | |

Network access:
- **Firmware build:** the Ubuntu image by digest, Ubuntu snapshot packages, and OpenSBI and U-Boot from GitHub.
- **`setup.sh`:** `cdn.openbsd.org`, signature-checked.
- **`vf2-kernel`:** `cdn.openbsd.org` for the signed `sys.tar.gz` (first run only), then anoncvs over SSH.

The earlier HFI-based kit and your own OpenBSD display drivers are no longer
in the tree. They are in the history: `legacy/` and `reference/` at
[750c736](https://github.com/rumorsorchid/openbsd_hdmi_vf2/tree/750c73691d6f132d62accb0e4d31cd46d111b5df).

What has been tested:

| Part | State |
|---|---|
| Firmware build | Clean builds are bit-identical, and an arm64 build (as on Apple Silicon) gives the same bytes as an amd64 build. No compiler warnings. |
| UART boot and flash tooling | XMODEM-1K and YMODEM were tested against `lrzsz`. The UART boot was tested end to end against a simulated mask ROM that greets the way the real one does (`(C)StarFive` before it asks for the SPL). The flash commands (version check, `sf probe`, `sf update`, read-back verification) were tested against real U-Boot code (`sandbox` with emulated SPI flash) for r1 and have not changed since. Corrupted transfers are caught before anything is written. |
| HDMI driver | The uncached-view self-test, including recovery from an address that faults, was run under QEMU with OpenSBI v1.9. **Not yet run on hardware:** that is what step 3 is for. |
| OpenBSD side | `setup.sh` and `vf2-kernel` were tested with OpenBSD 7.9's `ksh` (portable `oksh`), with stand-ins for `ftp`, `signify`, `cvs`, `config`, `make` and `sysupgrade`. The console, memory and boot-loader behaviour described here was checked against the OpenBSD 7.9 sources. |

## How it works

### The HDMI driver

`firmware/jh7110_hdmi.c` brings the display pipeline up from cold:
1. PMU power domain PD_VOUT.
2. SYSCRG display clocks and resets.
3. VOUTCRG clocks and resets.
4. vout-syscon routing (DC8200 panel 0 DPI into HDMI, RGB888).
5. HDMI PHY: pre-PLL and post-PLL, wait for both to lock, then LDO,
   serializer and TMDS drive strength.
6. HDMI transmitter: out of reset, DVI mode, CEA timing, power up, TMDS
   drivers on.
7. DC8200 timing, primary plane and output.

The transmitter is completely up before the display controller starts
sending pixels, in the order StarFive's own drivers use.

Where the values come from:
- **HDMI PHY and transmitter:** StarFive's Linux driver (`inno_hdmi.c` in the VisionFive 2 kernel) and StarFive's U-Boot (`sf_hdmi.c`), register for register, including the 1080p drive-strength settings. These are the sequences that bring HDMI up from cold on this SoC.
- **Power, clocks and resets:** the Linux JH7110 display series v4 (September 2026: `jh7110-vout-subsystem`, `jh7110-inno-hdmi`, `phy-jh7110-inno-hdmi`). HFI BIOS 1.4's VisionFive 2 VideoBIOS module, disassembled, uses the same registers and bits, down to the same timeout.
- **DC8200:** the upstream Linux VeriSilicon driver, with the values StarFive's U-Boot writes for 1080p.
- **Pixel clock:** from the HDMI PHY by default, as in StarFive's Linux driver. StarFive's U-Boot and HFI clock the DC8200 from PLL2 / 8 instead (`VF2_HDMI_PIXCLK=pll2`).
- **DVI mode**, as HFI uses: every HDMI monitor accepts it, and a console needs no audio or InfoFrames.
- **Your earlier OpenBSD drivers** confirmed three facts:
  - The pixel clock mux must select the HDMI PHY (`0x81000000`).
  - The PHY tables match Linux register for register.
  - The DC8200 does not snoop the CPU caches.

### Caches: solved in firmware, once

The DC8200 reads the framebuffer straight from DRAM and does not see what
sits in the CPU caches. The U74 cores ignore page-table memory types, so an
OS cannot simply map the framebuffer uncached. But the JH7110 decodes DRAM a
second time, uncached. The DC8200 is given the framebuffer's real address
(below 4 GiB). Everything on the CPU side (U-Boot's console, the EFI GOP and
so OpenBSD and X) uses the uncached view of the same RAM. Every write
reaches memory immediately, so there are no flushes, no `sfcc` changes, no
private ioctl and no patched `wsfb`.

Where the uncached view starts is not documented consistently:

| Candidate | Evidence |
|---|---|
| DRAM + 16 GiB (`0x4_4000_0000`: physical address bit 34 set) | StarFive's own U-Boot treats `0x0_4000_0000`–`0x4_3fff_ffff` (16 GiB) as the cached range it flushes; the uncached decode would start right above it |
| DRAM + 8 GiB (`0x2_4000_0000`) | U-Boot's JH7110 notes (`doc/board/starfive/jh7110_common.rst`) |

So the driver takes neither on trust. At power-on it writes a test word
through the cache and flushes it with the L2 controller. It then tries each
candidate in the order above. The word must read back through the candidate
view, and a word written through that view must reach DRAM with no flush. A
view that faults (OpenSBI passes the fault back to U-Boot), reaches other
memory or is itself cached fails the test. The `running; CPU view` log line
says which view was found. Without one, U-Boot writes its changes back to
DRAM after each update instead.

The framebuffer RAM is marked reserved (`no-map`) in the device tree handed
to the OS, and through that in the EFI memory map, so OpenBSD never reuses
it. The driver also removes its own node, which only U-Boot needs, from that
device tree.
