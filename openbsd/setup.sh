#!/bin/ksh
#
# setup.sh: run once on the VisionFive 2, as root, from the openbsd/ folder
# of this repository.  It installs
#
#   vf2-kernel  as /usr/local/sbin/vf2-kernel
#   HDMIFB      as /root/vf2/HDMIFB
#   xorg.conf   as /etc/X11/xorg.conf
#
# and puts the stock Xorg wsfb driver back, taken from this release's
# signed xserv set.  A file it replaces is kept in /root/vf2/ with the date
# appended.  It builds nothing; afterwards run: vf2-kernel build

set -eu
umask 022
PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/sbin:/usr/local/bin

HERE=$(cd "$(dirname "$0")" && pwd)
MIRROR=${VF2_MIRROR:-https://cdn.openbsd.org/pub/OpenBSD}
SAVE=/root/vf2
DRV=/usr/X11R6/lib/modules/drivers/wsfb_drv.so
NOW=$(date +%Y%m%d-%H%M%S)

die() { print -u2 -- "setup: $*"; exit 2; }
say() { print -- ">>> $*"; }

[ "$(id -u)" -eq 0 ] || die "run as root"
[ "$(uname -s) $(uname -m)" = "OpenBSD riscv64" ] ||
    die "this is for OpenBSD/riscv64"
case "$(sysctl -n kern.version | head -1)" in
*-current*|*-beta*)	die "this is a snapshot; use a release (-stable)" ;;
esac
rel=$(uname -r); nodot=$(print -- "$rel" | tr -d .)

# Install $1 as $2 with mode $3, keeping a different file already there
put() {
	if [ -f "$2" ] && ! cmp -s "$1" "$2"; then
		cp -p "$2" "$SAVE/${2##*/}.$NOW"
		say "kept your $2 as $SAVE/${2##*/}.$NOW"
	fi
	install -m "$3" "$1" "$2"
	say "installed $2"
}

mkdir -p "$SAVE"
put "$HERE/vf2-kernel" /usr/local/sbin/vf2-kernel 755
put "$HERE/HDMIFB" "$SAVE/HDMIFB" 644

if [ ! -d "${DRV%/*}" ]; then
	say "Xorg is not installed; skipping wsfb and xorg.conf"
else
	put "$HERE/xorg.conf" /etc/X11/xorg.conf 644

	set=xserv$nodot.tgz
	tmp=$(mktemp -d /tmp/vf2-setup.XXXXXXXX)
	trap 'rm -rf "$tmp"' EXIT
	say "fetching and verifying $set of $rel for the stock wsfb driver"
	ftp -o "$tmp/SHA256.sig" "$MIRROR/$rel/riscv64/SHA256.sig"
	ftp -o "$tmp/$set" "$MIRROR/$rel/riscv64/$set"
	(cd "$tmp" && signify -C -p "/etc/signify/openbsd-$nodot-base.pub" \
	    -x SHA256.sig "$set")
	tar -xzpf "$tmp/$set" -C "$tmp" ".$DRV"
	if cmp -s "$tmp$DRV" "$DRV"; then
		say "the stock wsfb driver is already installed"
	else
		[ -f "$DRV" ] && cp -p "$DRV" "$SAVE/${DRV##*/}.$NOW" &&
		    say "kept your wsfb driver as $SAVE/${DRV##*/}.$NOW"
		cp -p "$tmp$DRV" "$DRV"
		say "installed the stock wsfb driver of $rel"
	fi
fi

echo
/usr/local/sbin/vf2-kernel status
echo
say "next: vf2-kernel build"
