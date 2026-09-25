#!/usr/bin/env python3
"""
hfifw-extract.py - split an HFI BIOS firmware package (.hfifw) into the two
images the VisionFive 2 boots from SPI flash, so that HFI can be put back
with "vf2uart.py flash ... --force".

  hfifw-extract.py HFI-StarFive-VisionFive2-SysBIOS-1.4.hfifw OUTDIR

writes OUTDIR/hfi-spl.bin (SPI offset 0x0) and OUTDIR/hfi-u-boot.itb
(SPI offset 0x100000) after checking each against the sha256 in the package.
Standard library only.
"""

import hashlib
import os
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse_fdt(blob):
    """Return {node_path: {prop: bytes}} for a flattened device tree."""
    (magic, _total, off_struct, off_strings, _rsv, _ver, _lc, _cpu,
     size_strings, size_struct) = struct.unpack(">10I", blob[:40])
    if magic != FDT_MAGIC:
        raise ValueError("not a device tree / FIT image")
    strings = blob[off_strings:off_strings + size_strings]
    s = blob[off_struct:off_struct + size_struct]
    nodes, path, i = {}, [], 0
    while i < len(s):
        tok = struct.unpack(">I", s[i:i + 4])[0]
        i += 4
        if tok == FDT_BEGIN_NODE:
            end = s.index(b"\0", i)
            path.append(s[i:end].decode())
            nodes.setdefault("/".join(path) or "/", {})
            i = (end + 4) & ~3
        elif tok == FDT_END_NODE:
            path.pop()
        elif tok == FDT_PROP:
            length, nameoff = struct.unpack(">II", s[i:i + 8])
            i += 8
            name = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            nodes["/".join(path) or "/"][name] = s[i:i + length]
            i = (i + length + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise ValueError("bad FDT token %d" % tok)
    return nodes


def image(nodes, name):
    node = nodes.get("/images/" + name)
    if not node or "data" not in node:
        raise ValueError("package has no /images/%s" % name)
    data = node["data"]
    want = nodes.get("/images/%s/hash-1" % name, {}).get("value")
    if want is None:
        raise ValueError("/images/%s carries no hash" % name)
    if hashlib.sha256(data).digest() != want:
        raise ValueError("/images/%s does not match its sha256" % name)
    return data


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 64
    with open(argv[1], "rb") as f:
        nodes = parse_fdt(f.read())
    root = nodes.get("/", {})
    board = root.get("qsoe,board", b"").rstrip(b"\0").decode()
    version = root.get("qsoe,version", b"").rstrip(b"\0").decode()
    if board != "starfive,visionfive-2-v1.3b":
        raise ValueError("package is for %r, not the VisionFive 2 v1.3B"
                         % board)
    spl, itb = image(nodes, "spl"), image(nodes, "uboot")
    os.makedirs(argv[2], exist_ok=True)
    for name, data in (("hfi-spl.bin", spl), ("hfi-u-boot.itb", itb)):
        with open(os.path.join(argv[2], name), "wb") as f:
            f.write(data)
        print("%s  %s  (%d bytes)" % (hashlib.sha256(data).hexdigest(),
                                      name, len(data)))
    print("HFI BIOS %s for %s; flash with:" % (version, board))
    print("  python3 tools/vf2uart.py flash PORT %s %s --force"
          % (os.path.join(argv[2], "hfi-spl.bin"),
             os.path.join(argv[2], "hfi-u-boot.itb")))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except (OSError, ValueError) as e:
        sys.stderr.write("error: %s\n" % e)
        sys.exit(1)
