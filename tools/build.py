#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build TransitFM1 (Dinghy's platform, Felucca's as SLOOP carries it, with ChoralRoot's boot stub): the boot stub,
the update loader, the app with the loader inside it, and an installable .fwsc package.

  tools/build.py [--release X.Y[.Z][-suffix]]

Outputs in build/: stub/stub.bin (the boot stub, firmware/stub: ChoralRoot's, byte for byte), loader/ota.bin (the
update loader), gen/transit_loader.h (ota.bin as the app carries it), transit.bin (app.bin: the stub's 0xEE0 bytes,
then the app), transit.fwsc (the package, identity FM-1_015 as the official V15; with --release X.Y:
transit-X.Y.fwsc and SHA256SUMS). See README.md for the toolchain and the SDK.

The JieLi toolchain is Linux x86-64 only. JIELI_TOOLCHAIN points at it; on
macOS (or with JIELI_DOCKER=1) each tool runs in a linux/amd64 container.
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
OUT = SRC / "build"
GEN = OUT / "gen"
LDR = OUT / "loader"
STB = OUT / "stub"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

STUB_XIP = 0x02000120               # app.bin offset 0 in the XIP map; the SPL jumps here: the boot stub
STUB_LEN = fm1pkg_make.STUB_LEN     # 0xEE0: the stub ends at flash 0x5000
APP_XIP = STUB_XIP + STUB_LEN       # 0x02001000: the app proper (the body), firmware/app.ld
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

# SDK files of AC79NN_SDK_V1.2.1_2023-12-13 (the tested version)
SDK_SHA256 = {
    "uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}

PRODUCT = "FM-1_015"                # the official V15's identity: every installer writes over TransitFM1 as over stock
VERSION = None                      # APP_VER for release builds
UNIT = "transit"                    # firmware/src/UNIT.c; build/UNIT.{bin,elf,dis,fwsc}


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN")
    if not tc or not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("JIELI_TOOLCHAIN must point at the JieLi Linux toolchain "
                         "(the directory with pi32v2/ and common/; see tools/get_toolchain.sh)")
    return Path(tc).resolve()


def use_docker():
    native = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
    return os.environ.get("JIELI_DOCKER", "0" if native else "1") == "1"


def tc(tool, *args):
    """run a toolchain binary (pi32v2/bin/..., common/bin/...) with cwd SRC; paths relative to SRC"""
    rel = [str(Path(a).resolve().relative_to(SRC)) if isinstance(a, Path) else a for a in args]
    if tool == "cc":                # the toolchain's cc wrapper needs python3; call clang directly
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    if use_docker():
        cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/work",
               "-v", f"{toolchain()}:/opt/jieli:ro", "-w", "/work", DOCKER_IMAGE, f"/opt/jieli/{tool}", *rel]
    else:
        cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def tc_all(*cmds):
    with ThreadPoolExecutor(len(cmds)) as ex:
        return list(ex.map(lambda c: tc(*c), cmds))


# ---- update loader

def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
    blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader():
    LDR.mkdir(parents=True, exist_ok=True)
    src = FW / "loader"
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src"]
    tc_all(("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
           ("cc", *flags, "-c", src / "loader.c", "-o", LDR / "loader.o"))
    elf = LDR / "loader.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (LDR / "loader.dis").write_text(dis)
    for ln in hdr.splitlines():     # the loader rewrites the flash: nothing may run from XIP
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(image)
    if len(ota) > 0x4000:           # staged at 0xE0000 it must end below the record's sector (0xE4000), which the
        raise SystemExit(f"loader: ota.bin {len(ota)} B reaches the update record's sector")   # app and loader erase
    if b"FELUCCA-LOADER-1" not in ota[0x48:0x48 + 80]:     # the boot stub and ota_staged look for it there
        raise SystemExit("loader: no FELUCCA-LOADER-1 in the first LZ4 literals (crt0_ldr.S)")
    (LDR / "ota.bin").write_bytes(ota)
    GEN.mkdir(parents=True, exist_ok=True)   # the app stages its own copy (firmware/src/ota.c ota_restage)
    rows = [", ".join(f"0x{b:02x}" for b in ota[i:i + 16]) for i in range(0, len(ota), 16)]
    (GEN / "transit_loader.h").write_text(
        "/* generated by tools/build.py: build/loader/ota.bin, the update loader TransitFM1 stages (ota.c) */\n"
        f"static const uint8_t TRANSIT_LDR[{len(ota)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n")
    bss = [int(ln.split()[0], 16) for ln in syms.splitlines() if ln.rstrip().endswith("_bss_end")]
    print(f"loader: image {len(image)} B at {LOADER_LOAD:#x}" + (f", bss end {bss[0]:#x}" if bss else ""))
    print(f"loader: ota.bin {len(ota)} B")
    return ota


# ---- boot stub

def build_stub():
    """firmware/stub -> app.bin's first STUB_LEN bytes (0xFF padded). ChoralRoot's stub, built from its own sources
    alone, so it is the same bytes in every build: the loader rewrites sector 0x4000 only when it changes, and an
    install between TransitFM1 and ChoralRoot never does. firmware/stub/stub.lock records the version and hash of
    the stub that ships (ChoralRoot's own lock: the same file)."""
    STB.mkdir(parents=True, exist_ok=True)
    src = FW / "stub"
    tc_all(("cc", "-Ifirmware/stub", "-c", src / "crt0_stub.S", "-o", STB / "crt0_stub.o"),
           ("cc", *CFLAGS, "-Ifirmware/stub", "-c", src / "stub.c", "-o", STB / "stub.o"))
    elf = STB / "stub.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "stub.ld",
       STB / "crt0_stub.o", STB / "stub.o", "-o", elf)
    (STB / "stub.bin").unlink(missing_ok=True)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, STB / "stub.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (STB / "stub.dis").write_text(dis)
    img = (STB / "stub.bin").read_bytes()
    for ln in hdr.splitlines():     # no .data, no .bss: nothing but .text may have a size
        p = ln.split()
        if len(p) > 3 and p[0].isdigit() and p[1].startswith(".") and int(p[2], 16) and \
                p[1] not in (".text", ".shstrtab", ".symtab", ".strtab"):
            raise SystemExit(f"stub: section {p[1]} holds {int(p[2], 16)} B; the stub is code and constants only")
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != STUB_XIP:
        raise SystemExit(f"stub: _start is not at {STUB_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        raise SystemExit(f"stub: starts with {img[:4].hex()}, not the entry the SPL expects (goto +4; rts)")
    if not fm1pkg_make.has_stub(img + bytes(STUB_LEN)):
        raise SystemExit("stub: no CRSTUB signature at offset 8")
    if not img or len(img) > STUB_LEN - 256:     # keep a margin: a stub that grows to the brim cannot be fixed
        raise SystemExit(f"stub: {len(img)} B does not fit {STUB_LEN} B with 256 B to spare")
    for ln in dis.splitlines():     # nothing may call or load an address in the chip ROM (as the app's check)
        mm = LINE.match(ln)
        for v in (re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)) if mm else ()):
            val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
            if 0xFFC00000 <= val < 0xFFD00000:
                raise SystemExit(f"stub: reference to ROM address {val:#010x}")
    used = len(img)
    img += b"\xff" * (STUB_LEN - len(img))
    ver, sha = fm1pkg_make.stub_version(img), hashlib.sha256(img).hexdigest()
    print(f"stub: {used} B of {STUB_LEN} ({100 * used / STUB_LEN:.1f}%), version {ver}, sha256 {sha[:16]}")
    lock = src / "stub.lock"
    want = lock.read_text().split() if lock.exists() else []
    if want != [str(ver), sha]:
        msg = (f"the boot stub differs from {lock.relative_to(SRC)} ({' '.join(want)[:20] or 'none'}..): installing "
               "this build rewrites sector 0x4000, the one window an interruption is not rescued in, and an install "
               "between TransitFM1 and ChoralRoot would no longer leave that sector alone. The stub is ChoralRoot's: "
               "change it there, then copy firmware/stub/ and its lock")
        if VERSION:
            raise SystemExit("build: " + msg)
        print("WARNING: " + msg)
    return img


# ---- app

def build_app(stub):
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen", "-Ibuild/gen", f'-DFELUCCA_ID="{PRODUCT}"',
             f'-DAPP_VER="{VERSION or "0.0"}"']
    cmds = [("cc", "-c", FW / "crt0.S", "-o", OUT / "crt0.o"),
            ("cc", "-c", FW / "hal" / "fm1_vec.S", "-o", OUT / "fm1_vec.o"),
            ("cc", "-c", FW / "hal" / "fm1_isr.S", "-o", OUT / "fm1_isr.o"),
            ("cc", *flags, "-c", FW / "src" / f"{UNIT}.c", "-o", OUT / f"{UNIT}.o")]
    tc_all(*cmds)
    elf = OUT / f"{UNIT}.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", FW / "app.ld", OUT / "crt0.o", OUT / "fm1_vec.o",
       OUT / "fm1_isr.o", OUT / f"{UNIT}.o", "-o", elf)
    for sect in ("text.bin", "data.bin", "ramtext.bin"):
        (OUT / sect).unlink(missing_ok=True)
    *_, syms, dis, rt = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, OUT / "text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, OUT / "data.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, OUT / "ramtext.bin"),
                               ("common/bin/objdump", "-t", elf),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-d", "-j", ".ram_text", elf))
    (OUT / f"{UNIT}.dis").write_text(dis)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    img = bytearray((OUT / "text.bin").read_bytes())
    # .ram_text and .data follow .text at their load addresses; crt0 copies them by words
    for sect, lname in (("ramtext.bin", "_rt_load"), ("data.bin", "_data_load")):
        load = symv(lname)
        if load % 4:
            raise SystemExit(f"{lname} {load:#x} is not word aligned")
        blob = (OUT / sect).read_bytes() if (OUT / sect).exists() else b""
        if blob:
            if load - APP_XIP < len(img):
                raise SystemExit(f"{lname} overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img))
            img += blob
    img += b"\xff" * (-len(img) % 4)
    (OUT / f"{UNIT}.bin").write_bytes(stub + bytes(img))   # app.bin as it is packed: the stub, then the body
    return bytes(img), syms, dis, rt


def check(img, syms, dis, rt):
    errors, notes = [], []
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
    if rt_calls:                    # RAM code runs with the flash off: no calls into XIP
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    else:
        notes.append(f".ram_text: {len([ln for ln in rt.splitlines() if LINE.match(ln)])} insns, no calls")
    for ln in dis.splitlines():     # nothing may call or load an address in the chip ROM
        mm = LINE.match(ln)
        if not mm:
            continue
        for v in re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
            val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
            if 0xFFC00000 <= val < 0xFFD00000:
                errors.append(f"reference to ROM address {val:#010x}")
    if len(img) > fm1pkg_make.BODY_MAX:
        errors.append(f"the body {len(img)} B reaches the marker's sector (room: {fm1pkg_make.BODY_MAX} B)")

    def sym(name):
        mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
        return int(mm.group(1), 16) if mm else 0
    ram = sym("_bss_end") - 0x01C08000
    pool = sym("_pool_end") - 0x01C20000
    notes.append(f"body {len(img)} B of {fm1pkg_make.BODY_MAX}; RAM .data+.bss {ram} B of 98304; pool {pool} B of 344064")
    if ram > 96 * 1024:
        errors.append(f"RAM .data+.bss {ram} B > 96 KiB")
    return errors, notes


# Every hardware register access lives in hal/. In src/ and loader/ (comments stripped):
#  - no volatile pointer cast, except of a C object's address (`*(volatile T *)&x`: a read-once
#    of a RAM flag shared with an ISR, e.g. usb.c ota_wire_send);
#  - no literal in a register or reserved window (core SFRs 0x10000-0x13FFF, SFC 0x40000-0x43FFF,
#    GPIO/IOMAP 0x50000-0x51FFF, CPU 0x1EE0000-0x1EEFFFF, RAM top 0x01C7F000- (boot info,
#    mailbox, vectors), XIP 0x02000000-0x020FFFFF);
#  - no inline asm, except the empty compiler barrier RING_PUBLISH() (emits no instruction);
#  - no HAL register macro (a hal/ #define that is, or expands to, a volatile access).
# Linker symbols (_bss_start[], _rt_load[], ...) are plain C objects and pass.
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|"
                      r"1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check():
    def strip(s):
        s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
        return re.sub(r"//[^\n]*", "", s)
    defs = {}
    for h in sorted((FW / "hal").glob("*.h")):
        for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
            defs[m.group(1)] = m.group(2)
    regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
    while True:                                   # macros built on register macros (FM1_WR_LIMIT_H -> FM1_X2)
        more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
        if not more:
            break
        regs |= more
    errors = []
    for f in sorted([*(FW / "src").glob("*.[ch]"), *(FW / "loader").glob("*.c")]):
        for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
            where = f"{f.relative_to(FW)}:{no}"
            for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"),
                             (MMIO_ASM, "inline asm")):
                m = rx.search(ln)
                if m:
                    errors.append(f"{where}: {what} outside hal/ ({m.group(0).strip()}); add a hal/ helper")
            used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
            if used:
                errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside hal/; use a hal/ helper")
    return errors


def main():
    global PRODUCT, VERSION
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", metavar="X.Y", help="release build: the version string X.Y (the identity stays FM-1_015)")
    ap.add_argument("--sdk", type=Path, help="JieLi AC79 SDK checkout (default: $AC79_SDK)")
    a = ap.parse_args()
    name = f"{UNIT}.fwsc"
    if a.release:
        if not re.fullmatch(r"\d+\.\d+(\.\d+)?(-[A-Za-z0-9.]+)?", a.release):
            raise SystemExit(f"--release {a.release}: use X.Y, X.Y.Z or X.Y-suffix")
        VERSION = a.release
        name = f"{UNIT}-{a.release}.fwsc"
    fm1pkg_make.SDK = a.sdk
    for rel, sha in SDK_SHA256.items():          # fail early without the SDK
        if hashlib.sha256(fm1pkg_make.sdk_file(rel)).hexdigest() != sha:
            print(f"warning: SDK {rel} differs from AC79NN_SDK_V1.2.1; the package will not match the reference")
    OUT.mkdir(parents=True, exist_ok=True)
    stub = build_stub()
    ota = build_loader()
    img, syms, dis, rt = build_app(stub)
    errors, notes = check(img, syms, dis, rt)
    hal_err = mmio_check()
    errors += hal_err
    if not hal_err:
        notes.append("register access: hal/ only (src/, loader/ clean)")
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    flash = fm1pkg_make.flash_image(stub + img, fm1pkg_make.KEY)
    pkg = fm1pkg_make.ufw(flash, ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"sector 0x4000 (the directory and the stub) sha256 {hashlib.sha256(flash[0x4000:0x5000]).hexdigest()[:16]}; "
          f"marker at 0x91fe0, body {len(img)} B")
    print(f"app      {OUT / f'{UNIT}.bin'}  {len(stub) + len(img)} B")
    print(f"loader   {LDR / 'ota.bin'}  {len(ota)} B")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    if a.release:                   # what a recipient of the package must get with it (LICENSING.md)
        rel = OUT / f"release-{a.release}"
        if rel.exists():
            shutil.rmtree(rel)
        (rel / "LICENSES").mkdir(parents=True)
        shutil.copy(OUT / name, rel / name)
        for f in ("LICENSE", "LICENSING.md"):
            shutil.copy(SRC / f, rel / f)
        for f in sorted((SRC / "LICENSES").glob("*.txt")):
            shutil.copy(f, rel / "LICENSES" / f.name)
        sums = f"{hashlib.sha256(pkg).hexdigest()}  {name}\n"
        (rel / "SHA256SUMS").write_text(sums)
        print(f"release  {rel}/: {name}, SHA256SUMS, LICENSE, LICENSING.md, LICENSES/ "
              f"({', '.join(f.name for f in sorted((rel / 'LICENSES').iterdir()))})")
        print("         " + sums.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
