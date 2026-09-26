# Prebuilt firmware, release r1

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
OUT=prebuilt/r1 ./vf2-firmware.sh uart-boot /dev/cu.usbserial-XXXX
OUT=prebuilt/r1 ./vf2-firmware.sh flash /dev/cu.usbserial-XXXX
```
