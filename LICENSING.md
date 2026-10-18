# Licensing

TransitFM1 is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/` (but for the order in `ldr_core.c`), `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/build.py`, `tools/fm1_install.py`, `tools/lz4blk.py` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The wiring `firmware/src/platform.c`, as [Dinghy](https://github.com/zvenson/dinghy) has it | zvenson |
| The boot stub `firmware/stub/`, `firmware/crt0.S`, the stub's order in `ldr_core.c`, the kept loader in `ota.c`, `tools/fm1pkg_make.py` (the marker, the compensation), `tools/make_stub_test_pkg.py`, `tests/stub_test.c`, `tests/ota_test.c`, `tests/ldr_test.c`, `tests/stub_pkg_test.py` | ChoralRoot FM-1 contributors ([ChoralRoot FM-1](https://github.com/Quixotic7/ChoralRootFM1)) |
| The web installer's protocol and package code `web/fm1ota.js`, `web/fm1pkg.js` | Leo Kuroshita (@kurogedelic), Hügelton Instruments (Felucca), with ChoralRoot FM-1 contributors' changes |
| TransitFM1's parts: the foreign order and the commit after the package CRC in `ldr_core.c`, step 1 in `ota.c` (our own loader, restaged), `transit.c`, the identity, the web installer (`web/index.html`, `web/transit.js`, `web/test_installer.mjs`, `web/releases.json`), `tools/make_site.py`, the workflows, the tests of these, the docs | TransitFM1 contributors |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` and so into every package | SIL OFL 1.1 | `LICENSES/OFL-1.1-Terminus.txt` |
| JieLi AC79 SDK (tag `AC79NN_SDK_V1.2.1_2023-12-13`): `uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`, unmodified, in `tools/sdk/` so the package builds without a network fetch; placed in every package (the head and the configuration). Copyright JieLi Technology; the SDK has no NOTICE file | Apache-2.0 | `LICENSES/Apache-2.0.txt`, `tools/sdk/LICENSE`, <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## What goes with a package

A package (`transit-X.Y.fwsc`) is GPL-3.0-only firmware with the two parts above inside it. Whoever distributes it
gives with it: `LICENSE` (the GPL), `LICENSING.md` (this file), `LICENSES/` (Apache-2.0, OFL-1.1), and the source
(this repository at the release's tag). `./build.sh --release X.Y` puts them all in `build/release-X.Y/`; the
GitHub release carries them (`LICENSES.zip`), and the site serves them beside the package (`firmware/`).

Dinghy's logo (the designburg signet) is not under the GPL and marks Dinghy itself: it is not part of TransitFM1.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. TransitFM1 answers the update protocol with the official firmware's identity (`FM-1_015`) only so that
installers accept it; it is not made or endorsed by M-VAVE, and its screen says what it is.
