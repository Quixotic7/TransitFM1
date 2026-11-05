#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_install.py's package check: every package given is clean, and a byte flipped in its flash.bin or
ota.bin is caught. M-VAVE's V15 and V16 store another CRC in their other entries (USR, isd_config.ini): before
0.1.4 the tool called the official V15 "damaged".   install_pkg_test.py PACKAGE.fwsc [...]"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import fm1_install as f

bad = 0
for path in sys.argv[1:]:
    img = bytearray(f.logical_image(Path(path).read_bytes()))
    if f.package_damage(bytes(img)):
        print(f"{path}: called damaged ({f.package_damage(bytes(img))})"); bad += 1
    n = int.from_bytes(f._dec(img[0:0x40])[8:10], "little")
    for i in range(n):
        e = f._dec(img[0x40 + i * 0x50:0x40 + (i + 1) * 0x50])
        kind, off, size = (int.from_bytes(e[0:2], "little"), int.from_bytes(e[8:12], "little"),
                           int.from_bytes(e[12:16], "little"))
        if kind in (0, 100):
            img[off + size // 2] ^= 1
            if not f.package_damage(bytes(img)):
                print(f"{path}: entry {kind} damaged, not caught"); bad += 1
            img[off + size // 2] ^= 1
    print(f"{path}: clean, a flipped flash.bin / ota.bin byte caught" if not bad else "")
print("INSTALL PACKAGE TEST FAILED" if bad else "install package test passed")
sys.exit(bad != 0)
