#!/usr/bin/env bash
#
# vf2-firmware.sh: blob-free boot firmware with HDMI for the StarFive
# VisionFive 2, built on macOS (or Linux) and loaded over the debug UART.
#
#   ./vf2-firmware.sh build              build out/u-boot-spl.bin.normal.out
#                                        and out/u-boot.itb (default command)
#   ./vf2-firmware.sh uart-boot PORT     run the build on the board over UART,
#                                        writing nothing to it (boot switches
#                                        RGPIO_0/RGPIO_1 both at H)
#   ./vf2-firmware.sh flash PORT         at the prompt of a UART-booted build:
#                                        write both images to SPI flash and
#                                        verify them by reading them back
#   ./vf2-firmware.sh console PORT       serial console (Ctrl-] quits)
#
# PORT is the USB serial adapter, e.g. /dev/cu.usbserial-140 on macOS.
#
# Build options (environment variables):
#   VF2_HDMI_MODE=720p      1280x720@60 instead of 1920x1080@60
#   VF2_HDMI_PIXCLK=pll2    clock the display controller from PLL2, as
#                           HFI BIOS does, instead of from the HDMI PHY
#
# Everything that ends up on the board is compiled here from source:
#   OpenSBI (M-mode firmware) and U-Boot (SPL with the open DDR init, and
#   U-Boot proper with the HDMI framebuffer driver in firmware/u-boot).
# The build runs in a container pinned by digest, with packages from a dated
# Ubuntu snapshot, and every source tree is checked against a pinned commit.
# Change the pins below only on purpose.
#
# Needs: Docker (Docker Desktop, or: brew install colima docker &&
# colima start), git is not needed on the host. python3 for the UART
# commands (macOS ships it with the Command Line Tools).

set -euo pipefail

# ---------------------------------------------------------------- pins ----
BUILDER_IMAGE='ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3'
UBUNTU_SNAPSHOT='20260925T000000Z'

OPENSBI_URL='https://github.com/riscv-software-src/opensbi.git'
OPENSBI_TAG='v1.9'
OPENSBI_COMMIT='cbf9f6734dd85a982c63e3cb5db7ffe09da839ca'

UBOOT_URL='https://github.com/u-boot/u-boot.git'
UBOOT_TAG='v2026.07'
UBOOT_COMMIT='ece349ade2973e220f524ce59e59711cc919263f'

# Build timestamp baked into the images: the U-Boot release commit's date.
SOURCE_DATE_EPOCH='1783381843'
# -------------------------------------------------------------------------

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/out}
# Build natively: an arm64 (Apple Silicon) and an amd64 container produce
# byte-identical firmware from the same pinned sources and snapshot.
case "$(uname -m)" in
arm64|aarch64)	NATIVE=linux/arm64 ;;
*)		NATIVE=linux/amd64 ;;
esac
PLATFORM=${VF2_PLATFORM:-$NATIVE}
HDMI_MODE=${VF2_HDMI_MODE:-1080p}
HDMI_PIXCLK=${VF2_HDMI_PIXCLK:-phy}
case "$HDMI_MODE" in 1080p|720p) ;; *) echo "VF2_HDMI_MODE: 1080p or 720p" >&2; exit 64 ;; esac
case "$HDMI_PIXCLK" in phy|pll2) ;; *) echo "VF2_HDMI_PIXCLK: phy or pll2" >&2; exit 64 ;; esac
PROXY_HTTP=${http_proxy:-${HTTP_PROXY:-}}
PROXY_HTTPS=${https_proxy:-${HTTPS_PROXY:-}}
# Only behind a local proxy do the containers need the host's network.
NETFLAG=
if [ -n "$PROXY_HTTP$PROXY_HTTPS" ]; then
	NETFLAG=--network=host
fi

die() { printf 'vf2-firmware: %s\n' "$*" >&2; exit 1; }
say() { printf '>>> %s\n' "$*"; }

need_docker() {
	command -v docker >/dev/null 2>&1 ||
	    die "docker not found. Install Docker Desktop, or: brew install colima docker && colima start"
	docker info >/dev/null 2>&1 ||
	    die "docker is installed but not running (colima start, or open Docker Desktop)"
}

need_python() {
	command -v python3 >/dev/null 2>&1 ||
	    die "python3 not found (macOS: xcode-select --install)"
}

builder_tag() {
	# The image tag follows its recipe, so a pin change rebuilds it.
	printf '%s %s %s' "$BUILDER_IMAGE" "$UBUNTU_SNAPSHOT" "$PLATFORM" |
	    { shasum -a 256 2>/dev/null || sha256sum; } | cut -c1-12
}

build_image() {
	local tag ctx
	tag="vf2-fw-builder:$(builder_tag)"
	if docker image inspect "$tag" >/dev/null 2>&1; then
		printf '%s' "$tag"
		return
	fi
	ctx=$(mktemp -d)
	# Optional extra CA for networks with a TLS-intercepting proxy.
	if [ -n "${VF2_EXTRA_CA:-}" ]; then
		cp "$VF2_EXTRA_CA" "$ctx/extra-ca.crt"
	else
		: > "$ctx/extra-ca.crt"
	fi
	cat > "$ctx/Dockerfile" <<EOF
FROM $BUILDER_IMAGE
ARG http_proxy
ARG https_proxy
COPY extra-ca.crt /usr/local/share/ca-certificates/extra-ca.crt
RUN set -e; export DEBIAN_FRONTEND=noninteractive; \\
    apt-get update -q; \\
    apt-get install -y -q --no-install-recommends ca-certificates; \\
    update-ca-certificates >/dev/null 2>&1 || true; \\
    apt-get update -q --snapshot $UBUNTU_SNAPSHOT; \\
    apt-get install -y -q --snapshot $UBUNTU_SNAPSHOT --no-install-recommends \\
        gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu gcc libc6-dev make \\
        bison flex bc libssl-dev libgnutls28-dev uuid-dev python3 \\
        python3-dev python3-setuptools python3-pyelftools swig \\
        device-tree-compiler git xz-utils file; \\
    rm -rf /var/lib/apt/lists/*
EOF
	say "creating the build container (once; pinned image + Ubuntu snapshot $UBUNTU_SNAPSHOT)" >&2
	docker build --platform "$PLATFORM" $NETFLAG \
	    --build-arg "http_proxy=$PROXY_HTTP" \
	    --build-arg "https_proxy=$PROXY_HTTPS" \
	    -t "$tag" "$ctx" >&2
	rm -rf "$ctx"
	printf '%s' "$tag"
}

cmd_build() {
	local tag
	need_docker
	[ -f "$HERE/firmware/u-boot/files/drivers/video/jh7110_hdmi.c" ] ||
	    die "run this from a checkout of the openbsd_hdmi_vf2 repository"
	tag=$(build_image)
	mkdir -p "$OUT"
	say "building OpenSBI $OPENSBI_TAG and U-Boot $UBOOT_TAG with the HDMI driver"
	docker run --rm --platform "$PLATFORM" $NETFLAG \
	    -e "http_proxy=$PROXY_HTTP" -e "https_proxy=$PROXY_HTTPS" \
	    -e "OPENSBI_URL=$OPENSBI_URL" -e "OPENSBI_TAG=$OPENSBI_TAG" \
	    -e "OPENSBI_COMMIT=$OPENSBI_COMMIT" \
	    -e "UBOOT_URL=$UBOOT_URL" -e "UBOOT_TAG=$UBOOT_TAG" \
	    -e "UBOOT_COMMIT=$UBOOT_COMMIT" \
	    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
	    -e "BUILDER_IMAGE=$BUILDER_IMAGE" -e "UBUNTU_SNAPSHOT=$UBUNTU_SNAPSHOT" \
	    -e "PLATFORM=$PLATFORM" \
	    -e "HDMI_MODE=$HDMI_MODE" -e "HDMI_PIXCLK=$HDMI_PIXCLK" \
	    -v "$HERE:/repo:ro" -v "$OUT:/out" \
	    "$tag" bash /repo/firmware/build-in-container.sh
	echo
	cat "$OUT/SHA256SUMS"
	compare_expected
	echo
	say "next: set the boot switches to UART (RGPIO_0 and RGPIO_1 at H), then"
	say "      ./vf2-firmware.sh uart-boot /dev/cu.usbserial-XXXX"
}

compare_expected() {
	local want="$HERE/firmware/EXPECTED-SHA256SUMS"
	[ -f "$want" ] || return 0
	if [ "$HDMI_MODE" != 1080p ] || [ "$HDMI_PIXCLK" != phy ]; then
		say "not comparing with the reference hashes (non-default build)"
		return 0
	fi
	if diff -q <(grep -v '^#' "$want") "$OUT/SHA256SUMS" >/dev/null; then
		say "reproduced: the images match firmware/EXPECTED-SHA256SUMS bit for bit"
	else
		say "WARNING: the images differ from firmware/EXPECTED-SHA256SUMS."
		say "         The build is still from the pinned sources; the reference"
		say "         hashes may be from a different revision of this repository."
	fi
}

need_images() {
	[ -f "$OUT/u-boot-spl.bin.normal.out" ] && [ -f "$OUT/u-boot.itb" ] ||
	    die "no images in $OUT; run: ./vf2-firmware.sh build"
}

need_port() {
	[ -n "${1:-}" ] || die "which serial port? ls /dev/cu.usb* (macOS) or /dev/ttyUSB* (Linux)"
	[ -e "$1" ] || die "$1 does not exist"
}

cmd_uart_boot() {
	need_port "${1:-}"; need_images; need_python
	say "Board OFF, boot switches RGPIO_0 and RGPIO_1 at H, no other program"
	say "holding $1. Power the board on when asked."
	exec python3 "$HERE/tools/vf2uart.py" boot "$1" \
	    "$OUT/u-boot-spl.bin.normal.out" "$OUT/u-boot.itb"
}

cmd_flash() {
	local port=${1:-}
	need_port "$port"; need_images; need_python
	shift
	say "This writes SPI flash. The board must be at the 'StarFive #' prompt"
	say "of THIS build, started with ./vf2-firmware.sh uart-boot."
	printf 'Type YES to continue: '
	read -r answer
	[ "$answer" = YES ] || die "not flashing"
	exec python3 "$HERE/tools/vf2uart.py" flash "$port" \
	    "$OUT/u-boot-spl.bin.normal.out" "$OUT/u-boot.itb" "$@"
}

case "${1:-build}" in
build)		cmd_build ;;
uart-boot)	shift; cmd_uart_boot "$@" ;;
flash)		shift; cmd_flash "$@" ;;
console)	shift; need_port "${1:-}"; need_python
		exec python3 "$HERE/tools/vf2uart.py" console "$1" ;;
*)		sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 64 ;;
esac
