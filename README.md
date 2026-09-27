# Blob-free HDMI firmware for OpenBSD on the VisionFive 2

One script on your Mac builds the VisionFive 2 boot firmware from source,
with HDMI brought up in the firmware itself. OpenBSD then draws on that
display with its **stock** `simplefb` and Xorg `wsfb` drivers. The only
OpenBSD change is a three-line kernel config.

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
| U-Boot proper | U-Boot `v2026.07` + `firmware/u-boot/` (this repo, GPL-2.0+) |
| OpenBSD | signed release sets and packages; kernel built on the board |

No blobs are flashed. The mask ROM in the SoC is the only code not built
here, and nothing can replace it.

What the screen shows, from power-on:

| Stage | HDMI | Serial console |
|---|---|---|
| U-Boot (autoboot countdown, prompt) | yes, white on black | yes |
| OpenBSD boot loader (`boot>`) | yes | yes |
| kernel and `/etc/rc` | the loader's last screen, cleared when `simplefb` attaches | kernel messages |
| running system | X login (`xenodm`), text logins on Ctrl+Alt+F2 and up | serial login |

The kernel console stays on serial: OpenBSD/riscv64 has no early framebuffer
console (see section 3).

## Status

| Part | State |
|---|---|
| Build script | Tested. Clean builds are bit-identical, and an arm64 build (as on Apple Silicon) gives the same bytes as an amd64 build. The hashes are in `firmware/EXPECTED-SHA256SUMS`; the images themselves are in `prebuilt/r2/`. |
| UART boot and flash tooling | XMODEM-1K and YMODEM tested against `lrzsz`. The UART boot was tested end to end against a simulated mask ROM that greets the way the real one does (`(C)StarFive` before it asks for the SPL), with `lrzsz` receiving `u-boot.itb`. The flash commands (version check, `sf probe`, `sf update`, read-back verification) were tested against real U-Boot code (the `sandbox` build with emulated SPI flash) for r1 and have not changed since. Corrupted transfers are caught before anything is written. |
| HDMI driver | Compiles warning-free (`-Werror`). The uncached-view self-test, including recovery from an address that faults, was run under QEMU with OpenSBI v1.9. **Not yet run on hardware.** That is what the UART boot below is for: it writes nothing to the board. |
| OpenBSD kernel config and `vf2-kernel` | `vf2-kernel` was tested with OpenBSD 7.9's `ksh` (portable `oksh`), with stand-ins for `cvs`, `config`, `make` and `sysupgrade`: `boot.conf` handling, the check for local changes in `/usr/src/sys`, rebuild detection, upgrade. The console, memory and boot-loader behaviour described here was checked against the OpenBSD 7.9 sources. |

## How the HDMI driver was put together

`firmware/u-boot/files/drivers/video/jh7110_hdmi.c` brings the display
pipeline up from cold:
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
- **Pixel clock:** from the HDMI PHY by default, as in StarFive's Linux driver. StarFive's U-Boot and HFI clock the DC8200 from PLL2 / 8 instead; build with `VF2_HDMI_PIXCLK=pll2` for that.
- **DVI mode**, as HFI uses: every HDMI monitor accepts it, and a console needs no audio or InfoFrames.
- **What your OpenBSD drivers taught us** (kept in `reference/openbsd-native-drivers/`):
  - The pixel clock mux must select the HDMI PHY (`0x81000000`).
  - Your PHY tables match Linux register for register.
  - The DC8200 does not snoop the CPU caches, which is the key fact below.

## Caches: solved in firmware, once

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
memory or is itself cached fails the test. The serial log says which view
was found:

```
jh7110-hdmi: running; CPU view 0x4ff800000 (uncached, DRAM + 16 GiB), DC8200 rev ...
```

If neither works, the log says `no uncached view of DRAM found`. U-Boot then
still works, because it writes its changes back to DRAM after each update,
but OpenBSD would get a cached framebuffer and show stale lines. Send me the
log in that case.

The framebuffer RAM is marked reserved (`no-map`) in the device tree handed
to the OS, and through that in the EFI memory map, so OpenBSD never reuses
it. The driver also removes its own node, which only U-Boot needs, from that
device tree.

## You need

- A Mac with Docker: Docker Desktop, or `brew install colima docker && colima start`.
  - On Apple Silicon the build runs natively in an arm64 container; no Rosetta needed. An arm64 and an amd64 build produce byte-identical firmware (checked), so either matches `firmware/EXPECTED-SHA256SUMS`.
- `python3`. macOS: `xcode-select --install`.
- The USB serial adapter on the VisionFive 2 debug UART (115200 8N1).

## 1. Build (or use the prebuilt images)

Prebuilt images of the default build are in [`prebuilt/r2/`](prebuilt/r2/);
to use them, prefix the commands below with `OUT=prebuilt/r2`. Building them
yourself takes a few minutes and must give the same hashes.

```sh
./vf2-firmware.sh build
```

- The first run creates the build container. It uses a pinned Ubuntu image and packages from the `20260925T000000Z` Ubuntu snapshot.
- The build fetches OpenSBI and U-Boot and refuses to build if either tag is not the pinned commit.
- Output lands in `out/`:
  - `u-boot-spl.bin.normal.out`
  - `u-boot.itb`
  - `fw_dynamic.bin`
  - `u-boot.config`
  - `SHA256SUMS`
  - `BUILD-INFO`
- With default options, the script compares `SHA256SUMS` against `firmware/EXPECTED-SHA256SUMS` and tells you if your build reproduced the reference bit for bit.

Build options:
- `VF2_HDMI_MODE=720p` for 1280×720 instead of 1920×1080.
- `VF2_HDMI_PIXCLK=pll2` to clock the DC8200 the way HFI does.

## 2. Try it over UART (nothing is written to the board)

1. Board **off**. Set the boot switches **RGPIO_0 and RGPIO_1 both to H**
   (UART boot). Close picocom or anything else holding the serial port.
2. Run:
   ```sh
   ls /dev/cu.usb*
   ./vf2-firmware.sh uart-boot /dev/cu.usbserial-140
   ```
3. When it says so, **power the board on**. The mask ROM greets first; the
   tool waits until the ROM really asks, then sends the SPL over XMODEM. The
   SPL brings up DRAM and takes `u-boot.itb` over YMODEM (about 2.5 minutes
   at 115200 baud), then you are in a console.
4. On the serial console look for:
   ```
   jh7110-hdmi: 1920x1080@60, pixel clock from HDMI PHY, framebuffer 0xff800000
   jh7110-hdmi: running; CPU view 0x4ff800000 (uncached, DRAM + 16 GiB), DC8200 rev ...
   ```
   (the addresses may differ a little), and the U-Boot console on the HDMI
   screen: white text on black.
5. A USB keyboard works at the U-Boot prompt too. If it does not, type
   `usb reset` on the serial console: right after power-on a keyboard is
   sometimes missed.
6. Let it autoboot (or type `boot`): U-Boot finds OpenBSD's EFI loader on the
   NVMe disk, and OpenBSD's `boot>` prompt shows on HDMI and serial. This
   firmware keeps no settings in flash; it always starts from its built-in
   defaults.

A power cycle forgets all of this. With the switches still at H, you're back to
step 2.

## 3. OpenBSD on the new firmware

The stock `simplefb` driver draws on the framebuffer the firmware hands
over, but riscv64 `GENERIC` does not include it. So you build one kernel,
`HDMIFB` (`GENERIC.MP` plus `simplefb`), and `vf2-kernel` keeps it in step
with syspatches and releases. Your old `/bsd-hfi79` expects HFI's display
state and its own drivers; don't use it with this firmware.

Build it first, on the system as it is now (the build only compiles). On the
board, as root:

```sh
mkdir -p /root/vf2
cp HDMIFB /root/vf2/            # from openbsd/ in this repo
install -m 755 vf2-kernel /usr/local/sbin/vf2-kernel
vf2-kernel build                # /usr/src/sys at -stable, builds /bsd-hdmi
```

- The first build talks to anoncvs over SSH. Accept its host key once by hand (`ssh anoncvs@anoncvs.ca.openbsd.org`) after comparing the fingerprint with <https://www.openbsd.org/anoncvs.html>.
- If `/usr/src/sys` still carries changes of your own (the NATIVEBARS drivers, `sfcc.c`, `files.riscv64`), `vf2-kernel build` lists them and stops, because the kernel would no longer be stock. It prints the command that drops them (`cvs up -C`, which keeps each changed file as `.#name.revision`). To build with them anyway: `vf2-kernel build -m`.

Put the **stock** `wsfb` driver back (your patched one expects the DC8200
ioctl), and use `openbsd/xorg.conf`:

```sh
cp -p /usr/X11R6/lib/modules/drivers/wsfb_drv.so /root/vf2/wsfb_drv.so.dc8200
cd /tmp
ftp https://cdn.openbsd.org/pub/OpenBSD/7.9/riscv64/SHA256.sig
ftp https://cdn.openbsd.org/pub/OpenBSD/7.9/riscv64/xserv79.tgz
signify -C -p /etc/signify/openbsd-79-base.pub -x SHA256.sig xserv79.tgz
tar -xzpf xserv79.tgz -C / ./usr/X11R6/lib/modules/drivers/wsfb_drv.so
cp /path/to/openbsd/xorg.conf /etc/X11/xorg.conf
```

Then UART-boot the firmware (section 2). At OpenBSD's `boot>` prompt press
space, then backspace, and type `boot /bsd-hdmi`. The kernel's messages go to the serial
console; the HDMI screen clears when `simplefb` attaches. Log in on serial,
or on HDMI with Ctrl+Alt+F2, and check:

```sh
dmesg | grep -E 'simplefb|wsdisplay'
#   simplefb0 at mainbus0: 1920x1080, 32bpp
#   wsdisplay0 at simplefb0 mux 1
rcctl start xenodm
```

Typing on the HDMI console and working in FVWM should look clean with no
cache artifacts. Full-screen redraws are slower than with cached memory; tell
me how it feels. When happy:

```sh
vf2-kernel activate        # boot.conf: set image /bsd-hdmi
rcctl enable xenodm        # graphical login on HDMI at every boot
```

Optionally, a text login on the first HDMI screen as well (riscv64 leaves
ttyC0 without one):

```sh
sed -i '/^ttyC0[[:space:]]/s/[[:space:]]off[[:space:]]/ on  /' /etc/ttys
kill -HUP 1
```

Never put `set tty fb0` in `/etc/boot.conf`. It makes the boot loader name
the framebuffer as the console, and the riscv64 kernel only takes a serial
port as its console, so it would run with no console at all: no kernel
messages, no single-user shell. `vf2-kernel activate` removes such a line,
and `vf2-kernel status` warns about it.

### Keeping it current

- **Syspatches:** `syspatch`, then `vf2-kernel status`. It says whether a rebuild is due. If so, run `vf2-kernel build` and reboot. The previous kernel stays as `/bsd-hdmi.prev`. If the new one misbehaves, type `boot /bsd-hdmi.prev` at `boot>`, then run `vf2-kernel rollback`.
- **Packages** (Firefox etc.) keep coming prebuilt from `pkg_add -u`.
- **A new release:** `vf2-kernel upgrade` (instead of plain `sysupgrade`; arguments are passed on). It points `boot.conf` back at the stock `/bsd` first. The boot loader applies `set image` after it has chosen `/bsd.upgrade`, so with `/bsd-hdmi` still selected the upgrade would silently not happen. After the upgrade the stock kernel runs, which has no `simplefb`, so the HDMI screen keeps the boot loader's text. Log in on serial or over SSH, run `vf2-kernel build`, reboot, type `boot /bsd-hdmi` at `boot>`, then `vf2-kernel activate`.

## 4. Flash it

Only after the UART boot worked on your board. At the `StarFive #` prompt
of the UART-booted build (interrupt autoboot), from the Mac:

```sh
./vf2-firmware.sh flash /dev/cu.usbserial-140
```

It checks that the running U-Boot is this build, switches U-Boot's input to
serial only for the transfer, loads each image with `loady`, compares its
CRC32 in RAM, writes it (`sf update`: SPL at `0x0`, FIT at `0x100000`), reads it
back from flash and compares again. Then power off, set **RGPIO_0 and RGPIO_1
back to L** (flash boot) and power on.

## If the screen stays dark

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

## Going back to HFI

UART boot always works, whatever is in flash. To put HFI back into flash:

```sh
python3 tools/hfifw-extract.py HFI-StarFive-VisionFive2-SysBIOS-1.4.hfifw hfi/
# UART-boot this build (section 2), stop at the prompt, then:
python3 tools/vf2uart.py flash /dev/cu.usbserial-140 hfi/hfi-spl.bin hfi/hfi-u-boot.itb --force
```

## Layout

```
vf2-firmware.sh                 the one entry point on the Mac
firmware/build-in-container.sh  what runs inside the pinned container
firmware/u-boot/files/          jh7110_hdmi.c (the HDMI driver)
firmware/u-boot/patches/        Kconfig, Makefile, DT node, console env
firmware/u-boot/vf2-hdmi.config config fragment (video on, env not in flash)
firmware/EXPECTED-SHA256SUMS    reference hashes of the default build
prebuilt/r2/                    the default build's images, ready to UART-boot/flash
legacy/hfi-simplefb-kit/        the earlier HFI + patched simplefb approach
tools/vf2uart.py                XMODEM/YMODEM, UART boot, flash, console
tools/hfifw-extract.py          split an HFI package for re-flashing HFI
openbsd/HDMIFB                  kernel config: GENERIC.MP + simplefb
openbsd/vf2-kernel              on-board kernel rebuild/activation helper
openbsd/xorg.conf               stock wsfb
reference/openbsd-native-drivers/  your earlier OpenBSD HDMI drivers
```
