# Prebuilt firmware, release r2

Built by `./vf2-firmware.sh build` with default options (1920×1080@60, pixel
clock from the HDMI PHY, DVI mode) from the pinned sources listed in
`BUILD-INFO`. The build is reproducible: running it yourself must produce
files with exactly the hashes in `SHA256SUMS` (also in
`../../firmware/EXPECTED-SHA256SUMS`).

| File | Goes to |
|---|---|
| `u-boot-spl.bin.normal.out` | SPI flash offset `0x0` (the mask ROM loads it) |
| `u-boot.itb` | SPI flash offset `0x100000` (OpenSBI + U-Boot + device trees) |
| `fw_dynamic.bin` | already inside `u-boot.itb`; here for reference |
| `u-boot.config` | the U-Boot configuration used |

Use them without building:

```sh
shasum -a 256 -c SHA256SUMS
cd ../..
OUT=prebuilt/r2 ./vf2-firmware.sh uart-boot /dev/cu.usbserial-XXXX
OUT=prebuilt/r2 ./vf2-firmware.sh flash /dev/cu.usbserial-XXXX
```

## Changes since r1

Only `u-boot.itb` changed; the SPL and OpenSBI are bit-identical to r1.

- **HDMI bring-up follows StarFive's own drivers**:
  - The PHY PLLs are locked first, then the transmitter comes out of reset, gets its timing, powers up, and the PHY FIFO is resynchronised.
  - The sync-polarity bits are corrected, and the 1080p drive-strength settings are added.
  - The transmitter is explicitly switched to video from the display controller (not its built-in test pattern), as StarFive's U-Boot does.
- **The uncached view of DRAM is tested on the board.** r1 assumed DRAM + 16 GiB. r2 tries DRAM + 16 GiB, then DRAM + 8 GiB, and a view that faults is caught. If neither works, U-Boot draws through the cache and flushes it.
- **The OS no longer sees the driver's U-Boot-only device tree node.**
- **New `hdmibars on|off` command** (the transmitter's own colour bars), and `hdmiregs` shows more registers.
- **Console:** white on black, scrolling 10 lines at a time.

The host tools changed too (not part of these images):
- `vf2uart.py` waits out the mask ROM's `(C)StarFive` greeting before sending the SPL; r1's tool could start too early.
- `vf2uart.py` also refuses an SPL without the sfspl header.
