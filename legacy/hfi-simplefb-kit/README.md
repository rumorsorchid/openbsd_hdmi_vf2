> **Superseded.** This was the first approach: keep HFI BIOS flashed and patch
> OpenBSD's `simplefb` to use the uncached alias. The current approach (the
> top of this repository) brings HDMI up in blob-free U-Boot instead, so
> OpenBSD needs no source patch at all. Kept for reference and as a fallback
> for boards that stay on HFI.

# OpenBSD workstation on the VisionFive 2 with HFI BIOS

This directory replaces the NATIVEBARS stack (8 display drivers, JH7110 `sfcc`,
a private ioctl, a patched `wsfb_drv.so`) with:

- stock OpenBSD `GENERIC.MP` plus the stock `simplefb` driver (`HFIFB`),
- one 11-line patch to `simplefb.c` (`patches/simplefb-jh7110-uncached.diff`),
- stock Xorg `wsfb` with ShadowFB,
- `hfi-kernel`, which rebuilds that kernel when the release or syspatches change.

## Why it should work

HFI BIOS brings HDMI up and hands OpenBSD its framebuffer through the EFI GOP.
OpenBSD's boot loader turns that into a `simple-framebuffer` device tree node,
and `simplefb` attaches to it. That part already worked in the first experiments.
What failed was caching: the JH7110 display controller does not see what sits
in the CPU caches, and the U74 cores ignore the page-table "uncached" bits.

The JH7110 has an uncached view of all DRAM at DRAM + 16 GiB (`0x4_4000_0000`
for DRAM at `0x4000_0000`, physical address bit 34).
The patch makes `simplefb` map the framebuffer through that view, so every
write, from the console or from X, reaches memory immediately. That removes the
need for cache flushing, the ioctl and the `wsfb` patch.

**This has not been tested on hardware yet.** Follow the test below before
relying on it. Your current `/bsd-hfi79` stays the default until you run
`hfi-kernel activate`.

## Files

| File | Installed as |
|---|---|
| `hfi-kernel` | `/usr/local/sbin/hfi-kernel` |
| `HFIFB` | `/root/hfi/HFIFB` (copied into `/usr/src/sys/arch/riscv64/conf/` by the script) |
| `patches/simplefb-jh7110-uncached.diff` | `/root/hfi/patches/` |
| `mail/` | a draft for tech@openbsd.org |

## 1. Install (over SSH, as root)

From the Mac:

```sh
scp -r vf2-openbsd puff@openbsd.home.arpa:/tmp/
```

On the board:

```sh
su -
mkdir -p /root/hfi/patches
cp /tmp/vf2-openbsd/HFIFB /root/hfi/
cp /tmp/vf2-openbsd/patches/*.diff /root/hfi/patches/
install -m 755 /tmp/vf2-openbsd/hfi-kernel /usr/local/sbin/hfi-kernel
echo "alias hfi-kernel=/usr/local/sbin/hfi-kernel" >> /root/.profile
```

`/usr/local/sbin` and `/root` both survive `sysupgrade`.

The first build talks to anoncvs over SSH, and SSH will ask you to accept the
server's host key. Do that once by hand, and compare the fingerprint with
<https://www.openbsd.org/anoncvs.html>:

```sh
ssh anoncvs@anoncvs.ca.openbsd.org
```

## 2. Build the test kernel

```sh
hfi-kernel build
```

This moves `/usr/src/sys` to the `OPENBSD_7_9` (-stable) branch, applies the
patch, builds `HFIFB` and installs it as `/bsd-hfifb`. It does not touch `/bsd`
or `/etc/boot.conf`. It takes a while on the U74; the log is in
`/var/log/hfi-kernel.log`.

Your NATIVEBARS changes to `sfcc.c` and `files.riscv64` stay in the tree and
are harmless (HFIFB does not configure the `stf*` drivers). Once the new setup
is proven, clean them out with `cvs up -C` so the tree stays pristine; they are
backed up in `/root/hfi79-v2-backup-*`.

## 3. Use the stock wsfb driver for the test

Your patched `wsfb_drv.so` calls the DC8200 ioctl, which `simplefb` does not
have. Keep it, and put the stock module in place:

```sh
cp -p /usr/X11R6/lib/modules/drivers/wsfb_drv.so /root/hfi/wsfb_drv.so.dc8200
cd /tmp
ftp https://cdn.openbsd.org/pub/OpenBSD/7.9/riscv64/SHA256.sig
ftp https://cdn.openbsd.org/pub/OpenBSD/7.9/riscv64/xserv79.tgz
signify -C -p /etc/signify/openbsd-79-base.pub -x SHA256.sig xserv79.tgz
tar -xzpf xserv79.tgz -C / ./usr/X11R6/lib/modules/drivers/wsfb_drv.so
```

(If `syspatch -l` lists an Xorg server fix, take the stock module from
`/root/hfi79-wsfb-backups` instead, so you don't downgrade it.)

To go back to the old stack at any time:

```sh
cp -p /root/hfi/wsfb_drv.so.dc8200 /usr/X11R6/lib/modules/drivers/wsfb_drv.so
```

For the first test, keep X out of the way:

```sh
rcctl disable xenodm
```

## 4. Boot the test kernel over UART

On the Mac:

```sh
ls -1 /dev/cu.usb*
sudo picocom -b 115200 /dev/cu.usbserial-140
```

Then on the board: `shutdown -r now`.

1. HFI shows its POST. **Press nothing** (Ctrl-G there opens HFI's System
   Console, DEL opens Set-Up).
2. OpenBSD's boot loader prints something like:

   ```
   >> OpenBSD/riscv64 BOOTRISCV64 1.x
   boot>
   ```

   A few seconds later it would boot `/bsd-hfi79` by itself. As soon as
   `boot>` appears, **press the space bar once** (not Enter: Enter boots the
   default immediately). The countdown stops.
3. Press Backspace to remove the space, then type exactly:

   ```
   boot /bsd-hfifb
   ```

   and press Enter.

If you miss the window, let it boot, then `shutdown -r now` and try again.

To get the old kernel back later: do nothing at `boot>` (boot.conf still
selects `/bsd-hfi79`). The stock kernel is `boot /bsd`.

## 5. What to check

Over UART or SSH:

```sh
sysctl kern.version | head -1          # ... (HFIFB) #...
dmesg | grep -E 'simplefb|wsdisplay|stfdc'
```

Expected:

```
simplefb0 at mainbus0: 1920x1080, 32bpp
wsdisplay0 at simplefb0 mux 1
```

and no `stfdc` lines.

On the HDMI screen you should get a login prompt. With the USB keyboard:

- log in and type: every character must appear at once, with no holes;
- `ls -lR /usr/share | head -3000` scrolls a lot: the text must stay clean.
  Note how fast it scrolls.

Then X:

```sh
rcctl start xenodm
```

Log in, open xterms, drag windows, move the mouse, scroll. Look for errors:

```sh
grep -E '\(EE\)|\(WW\)' /var/log/Xorg.0.log
```

### If something is wrong

| Symptom | Meaning / action |
|---|---|
| no `simplefb0` line | HFI did not hand over a GOP framebuffer: HFI Set-Up, Advanced Settings, enable EFI GOP support |
| stripes, garbage or a black screen | the uncached view is not where the U-Boot docs say; send me the `dmesg` |
| hangs at `simplefb0` | same; power-cycle and boot normally (old kernel) |
| clean but very slow | uncached writes are too slow for your taste; tell me how slow |

Any failure: reboot, press nothing, and you are back on `/bsd-hfi79`. Restore
the patched `wsfb_drv.so` (step 3) and `rcctl enable xenodm`.

### If everything works

```sh
hfi-kernel activate      # only works while running /bsd-hfifb
rcctl enable xenodm
```

## Day to day

```sh
hfi-kernel status        # what is running, installed, selected; is a rebuild due
```

After `syspatch`:

```sh
hfi-kernel check         # offline; says whether a kernel fix came in
hfi-kernel build         # rebuilds only if the -stable sources changed
```

Then either test it once at `boot>` (`boot /bsd-hfifb`) and run
`hfi-kernel activate`, or, once you trust the routine, `hfi-kernel activate -f`.
Your previous kernel stays as `/bsd-hfifb.prev`; `hfi-kernel rollback` or
typing `boot /bsd-hfifb.prev` at `boot>` brings it back.

Weekly reminder by mail to root (`crontab -e` as root):

```
0 9 * * 1 /usr/local/sbin/hfi-kernel check >/dev/null || /usr/local/sbin/hfi-kernel check
```

## Twice a year: new release

```sh
hfi-update backup
hfi-kernel pre-upgrade   # stock /bsd becomes the default
sysupgrade
```

The machine comes back on stock `/bsd`: no HDMI (stock riscv64 GENERIC has no
simplefb), so use SSH or UART. Then:

```sh
pkg_add -u
hfi-kernel build         # switches /usr/src/sys to the new -stable branch
```

Test at `boot>` with `boot /bsd-hfifb`, then `hfi-kernel activate`.

If `hfi-kernel build` says the patch no longer applies, upstream changed
`simplefb.c`. Check first whether upstream now handles the JH7110 itself; if
so, drop the patch. Otherwise the patch needs rebasing by hand.

## When upstream takes it

If both diffs in `mail/` are committed, the stock `/bsd` has everything:
delete `/bsd-hfifb*`, run `hfi-kernel pre-upgrade` once to point boot.conf at
`/bsd`, and from then on plain `syspatch` and `sysupgrade` are the whole
update routine.
