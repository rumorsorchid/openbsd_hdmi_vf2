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

## Status

| Part | State |
|---|---|
| Build script | Tested. Two clean builds are bit-identical; the hashes are in `firmware/EXPECTED-SHA256SUMS`. |
| UART boot and flash tooling | XMODEM-1K and YMODEM tested against `lrzsz`. The flash sequence (version check, `sf probe`, `sf update`, read-back verification) was tested against real U-Boot code (the `sandbox` build with emulated SPI flash). Corrupted transfers are caught before anything is written. |
| HDMI driver | Compiles warning-free (`-Werror`). **Not yet run on hardware.** That is what the UART boot below is for: it writes nothing to the board. |
| OpenBSD kernel config and `vf2-kernel` | Syntax-checked; `boot.conf` handling tested. |

## How the HDMI driver was put together

`firmware/u-boot/files/drivers/video/jh7110_hdmi.c` brings the display
pipeline up from cold:
1. PMU power domain PD_VOUT.
2. SYSCRG display clocks and resets.
3. VOUTCRG clocks and resets.
4. vout-syscon routing (DC8200 panel 0 DPI into HDMI, RGB888).
5. Innosilicon HDMI controller and PHY (pre-PLL, then post-PLL).
6. DC8200 timing, primary plane and output.

Where the values come from:
- **Register values and order:** the Linux JH7110 display series v4 (September 2026: `jh7110-vout-subsystem`, `jh7110-inno-hdmi`, `phy-jh7110-inno-hdmi`, `inno-hdmi`) and the upstream VeriSilicon DC driver.
- **Cross-check against hardware-proven code:** HFI BIOS 1.4's VisionFive 2 VideoBIOS module, disassembled. Its power, clock and reset sequence uses the same registers and bits as this driver, down to the same timeout. It differs in two places, and both are available here as fallbacks:
  - HFI clocks the DC8200 from PLL2 / 8 rather than from the HDMI PHY (`VF2_HDMI_PIXCLK=pll2`).
  - HFI runs the transmitter in DVI mode, and programs `SYS_CTRL` only after the PHY PLLs lock.
- **What your OpenBSD drivers taught us** (kept in `reference/openbsd-native-drivers/`):
  - The pixel clock mux must select the HDMI PHY (`0x81000000`).
  - Your PHY tables match Linux register for register.
  - The DC8200 does not snoop the CPU caches, which is the key fact below.

**Caches: solved in firmware, once.** The DC8200 is given the framebuffer's
real address (below 4 GiB). Everything on the CPU side (U-Boot's console, the
EFI GOP and so OpenBSD and X) uses the JH7110's uncached view of the same
RAM at +8 GiB. Every write reaches memory immediately, so there are no
flushes, no `sfcc` changes, no private ioctl and no patched `wsfb`.

The framebuffer RAM is marked reserved in the device tree handed to the OS,
and through that in the EFI memory map, so OpenBSD never reuses it.

## You need

- A Mac with Docker: Docker Desktop, or `brew install colima docker && colima start`.
  - On Apple Silicon the build runs as `linux/amd64` so the output matches the reference hashes. It needs Rosetta or qemu in Docker, which Docker Desktop has.
  - With colima, use `colima start --vm-type vz --vz-rosetta`.
- `python3`. macOS: `xcode-select --install`.
- The USB serial adapter on the VisionFive 2 debug UART (115200 8N1).

## 1. Build

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
3. When it says so, **power the board on**. The mask ROM takes the SPL over
   XMODEM, the SPL brings up DRAM and takes `u-boot.itb` over YMODEM (about
   2.5 minutes at 115200 baud), then you are in a console.
4. On the serial console look for:
   ```
   jh7110-hdmi: 1920x1080@60, pixel clock from HDMI PHY, fb 0xfe..., DC8200 rev ...
   ```
   and the U-Boot console on the HDMI screen. A USB keyboard works at the
   U-Boot prompt too.
5. Let it autoboot (or type `boot`): U-Boot finds OpenBSD's EFI loader on the
   NVMe disk. This firmware keeps no settings in flash; it always starts from
   its built-in defaults.

A power cycle forgets all of this. With the switches still at H, you're back to
step 2.

## 3. OpenBSD on the new firmware

Your old `/bsd-hfi79` expects HFI's display state and its own drivers; boot
the stock `/bsd` (serial console) or, better, build the HDMIFB kernel once.
On the board, as root:

```sh
mkdir -p /root/vf2
cp HDMIFB /root/vf2/            # from openbsd/ in this repo
install -m 755 vf2-kernel /usr/local/sbin/vf2-kernel
echo "alias vf2-kernel=/usr/local/sbin/vf2-kernel" >> /root/.profile
vf2-kernel build                # /usr/src/sys at -stable, builds /bsd-hdmi
```

The first build talks to anoncvs over SSH. Accept its host key once by hand
(`ssh anoncvs@anoncvs.ca.openbsd.org`) after comparing the fingerprint with
<https://www.openbsd.org/anoncvs.html>.

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

Reboot into the UART-booted firmware (section 2), and at OpenBSD's `boot>`
prompt press space, backspace, and type `boot /bsd-hdmi`. Check:

```sh
dmesg | grep -E 'simplefb|wsdisplay'
#   simplefb0 at mainbus0: 1920x1080, 32bpp
#   wsdisplay0 at simplefb0 mux 1
rcctl start xenodm
```

Typing on the HDMI console and working in FVWM should look clean with no
cache artifacts. Full-screen redraws are slower than with cached memory; tell
me how it feels. When happy: `vf2-kernel activate` and `rcctl enable xenodm`.

Later updates: `syspatch`, then `vf2-kernel check` / `vf2-kernel build`.
Packages (Firefox etc.) keep coming prebuilt from `pkg_add -u`. For a new
release: `vf2-kernel pre-upgrade`, `sysupgrade`, `vf2-kernel build`, test at
`boot>`, `vf2-kernel activate`.

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
2. Build with `VF2_HDMI_PIXCLK=pll2` (HFI's pixel clock) and UART-boot again.
3. Compare registers with HFI, where it works. In **HFI's** System Console
   (Ctrl-G on serial during POST) and at **this** U-Boot's prompt, run the same
   commands and send me both outputs:
   ```
   md.l 0x130200e8 6
   md.l 0x13020294 1
   md.l 0x295c0000 12
   md.l 0x295c0048 2
   md.l 0x295b0004 2
   md.l 0x29400020 2
   md.l 0x29401400 30
   md.l 0x29401cc0 8
   md.l 0x294024d8 12
   md.l 0x29590000 40
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
tools/vf2uart.py                XMODEM/YMODEM, UART boot, flash, console
tools/hfifw-extract.py          split an HFI package for re-flashing HFI
openbsd/HDMIFB                  kernel config: GENERIC.MP + simplefb
openbsd/vf2-kernel              on-board kernel rebuild/activation helper
openbsd/xorg.conf               stock wsfb
reference/openbsd-native-drivers/  your earlier OpenBSD HDMI drivers
```
