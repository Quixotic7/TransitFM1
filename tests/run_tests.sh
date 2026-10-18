#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The host tests of TransitFM1's update path (docs/SAFETY.md), on the device build's outputs. Run ./build.sh first.
#   tests/run_tests.sh
#   V15_PKG=path/FM-1.fwsc   also: TransitFM1 -> the official V15 and back (M-VAVE's file; not in this tree)
#   CR_PKG=path/choralroot.fwsc  also: TransitFM1 <-> ChoralRoot (the same sector 0x4000, no unprotected cut)
# Ends "ALL TESTS PASSED" (exit 0), or stops at the first failure.
set -e
cd "$(dirname "$0")/.."
if [ -z "${AC79_SDK:-}" ]; then                   # as build.sh: an SDK checkout, else tools/sdk
    if [ -f "$HOME/fw-AC79_AIoT_SDK/cpu/wl82/tools/uboot.boot" ]; then AC79_SDK="$HOME/fw-AC79_AIoT_SDK"; else AC79_SDK="$PWD/tools/sdk"; fi
fi
export AC79_SDK
H=build/host
mkdir -p "$H"
[ -f build/transit.fwsc ] && [ -f build/transit.bin ] || { echo "run_tests.sh: run ./build.sh first"; exit 1; }
CC="cc -std=c99 -Wall -Wextra -Wno-unused-function -Wno-sign-compare"
SAN="-O1 -fsanitize=address,undefined -fno-sanitize-recover=all"

echo "== the boot stub's decision (firmware/stub/stub_core.h)"
$CC $SAN -o "$H/stub_test" tests/stub_test.c
"$H/stub_test"
echo "== the package builder: the marker, sector 0x4000 the same for every body"
python3 tests/stub_pkg_test.py
echo "== step 1: our own loader staged, kept, restaged (firmware/src/ota.c)"
$CC $SAN -o "$H/ota_test" tests/ota_test.c
"$H/ota_test" build/transit.fwsc ${V15_PKG:+"$V15_PKG"}
echo "== the loader's order: a power cut after every erase and every programmed page (firmware/loader/ldr_core.c)"
for m in other-body old-stub no-stub no-stub-other; do
    python3 tools/make_stub_test_pkg.py "$m" --app build/transit.bin --ota build/loader/ota.bin --product FM-1_015 \
        -o "$H/stub-test-$m.fwsc" >/dev/null
done
$CC -O2 -o "$H/ldr_test" tests/ldr_test.c
"$H/ldr_test" "$H/stub-test-no-stub.fwsc" build/transit.fwsc "$H/stub-test-other-body.fwsc" \
    "$H/stub-test-no-stub-other.fwsc" "$H/stub-test-old-stub.fwsc"
if [ -n "$V15_PKG" ]; then
    echo "== TransitFM1 -> the official V15 (a foreign package: sector 0x4000 last)"
    "$H/ldr_test" build/transit.fwsc "$V15_PKG"
    echo "== the official V15 -> TransitFM1 (TransitFM1's loader, staged by V15's updater)"
    "$H/ldr_test" "$V15_PKG" build/transit.fwsc
else
    echo "(V15_PKG not set: TransitFM1 <-> the official V15 skipped)"
fi
if [ -n "$CR_PKG" ]; then
    echo "== TransitFM1 <-> ChoralRoot (the same stub: sector 0x4000 never written)"
    "$H/ldr_test" build/transit.fwsc "$CR_PKG"
    "$H/ldr_test" "$CR_PKG" build/transit.fwsc
else
    echo "(CR_PKG not set: TransitFM1 <-> ChoralRoot skipped)"
fi
echo "ALL TESTS PASSED"
