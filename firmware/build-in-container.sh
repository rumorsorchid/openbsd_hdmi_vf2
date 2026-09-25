#!/bin/bash
#
# Runs inside the pinned build container started by vf2-firmware.sh.
# /repo is this repository (read-only), /out receives the images.

set -euo pipefail
export TZ=UTC LC_ALL=C SOURCE_DATE_EPOCH
CROSS=riscv64-linux-gnu-
JOBS=$(nproc)

fetch() {	# url tag commit dir
	git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" \
	    "$1" "$4"
	local got
	got=$(git -C "$4" rev-parse HEAD)
	if [ "$got" != "$3" ]; then
		echo "!!! $4: tag $2 is commit $got, expected $3 -- refusing to build" >&2
		exit 1
	fi
	echo ">>> $4: $2 = $3 (verified)"
}

mkdir -p /build
cd /build

fetch "$OPENSBI_URL" "$OPENSBI_TAG" "$OPENSBI_COMMIT" opensbi
fetch "$UBOOT_URL" "$UBOOT_TAG" "$UBOOT_COMMIT" u-boot

echo ">>> OpenSBI (generic platform, fw_dynamic at 0x40000000)"
make -s -C opensbi CROSS_COMPILE=$CROSS PLATFORM=generic \
    FW_TEXT_START=0x40000000 FW_OPTIONS=0 -j"$JOBS"
FW=/build/opensbi/build/platform/generic/firmware/fw_dynamic.bin

echo ">>> U-Boot: adding the JH7110 HDMI framebuffer driver"
cd u-boot
cp /repo/firmware/u-boot/files/drivers/video/jh7110_hdmi.c drivers/video/
for p in /repo/firmware/u-boot/patches/*.patch; do
	git apply --check "$p"
	git apply "$p"
	echo "    applied $(basename "$p")"
done
# The tree was verified against its pinned commit above. Without git
# metadata U-Boot's version string carries no "-dirty"/"+" decoration.
rm -rf .git

make -s CROSS_COMPILE=$CROSS starfive_visionfive2_defconfig
cat /repo/firmware/u-boot/vf2-hdmi.config >> .config
./scripts/config --set-str LOCALVERSION "-openbsd-hdmi-vf2" \
    --disable LOCALVERSION_AUTO \
    --set-str JH7110_HDMI_DEFAULT_MODE "${HDMI_MODE:-1080p}"
if [ "${HDMI_PIXCLK:-phy}" = pll2 ]; then
	./scripts/config --enable JH7110_HDMI_PIXCLK_PLL2
fi
make -s CROSS_COMPILE=$CROSS olddefconfig

# Every line of the fragment must have survived olddefconfig.
grep -E '^(CONFIG_|# CONFIG_)' /repo/firmware/u-boot/vf2-hdmi.config |
while read -r line; do
	grep -qxF "$line" .config || {
		echo "!!! config did not stick: $line" >&2; exit 1; }
done

echo ">>> U-Boot (SPL + OpenSBI/U-Boot FIT)"
make -s CROSS_COMPILE=$CROSS OPENSBI="$FW" -j"$JOBS"

version=$(strings u-boot.bin | grep "^U-Boot 20" | head -1)
echo "    $version"
case "$version" in
"U-Boot 2026.07-openbsd-hdmi-vf2 ("*) ;;
*) echo "!!! unexpected U-Boot version string" >&2; exit 1 ;;
esac

cp spl/u-boot-spl.bin.normal.out u-boot.itb /out/
cp .config /out/u-boot.config
cp "$FW" /out/fw_dynamic.bin
cd /out
sha256sum u-boot-spl.bin.normal.out u-boot.itb fw_dynamic.bin > SHA256SUMS
{
	echo "builder image    $BUILDER_IMAGE ($PLATFORM)"
	echo "ubuntu snapshot  $UBUNTU_SNAPSHOT"
	echo "compiler         $(${CROSS}gcc --version | head -1)"
	echo "opensbi          $OPENSBI_TAG $OPENSBI_COMMIT"
	echo "u-boot           $UBOOT_TAG $UBOOT_COMMIT"
	echo "source date      $SOURCE_DATE_EPOCH"
	echo "hdmi             ${HDMI_MODE:-1080p}, pixel clock from ${HDMI_PIXCLK:-phy}"
	echo "hdmi driver      $(sha256sum /repo/firmware/u-boot/files/drivers/video/jh7110_hdmi.c | cut -c1-64)"
	for p in /repo/firmware/u-boot/patches/*.patch; do
		echo "patch            $(sha256sum "$p" | cut -c1-64) $(basename "$p")"
	done
} > BUILD-INFO
cat BUILD-INFO
