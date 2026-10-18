# How TransitFM1 keeps a firmware change safe

TransitFM1 exists for one moment: the write that replaces it. Everything here serves one goal. Wherever the power
or the cable is cut during that write, the FM-1 should come back in update mode, not dark.

## The pieces

| piece | where | from |
| --- | --- | --- |
| the boot stub: flash 0x4120..0x4FFF, where the SPL jumps | `firmware/stub/` | ChoralRoot, byte for byte (version 3, sha256 `baee55ed…`) |
| the commit marker, flash 0x91FE0: magic, the body's length and CRC-32, its own CRC | `tools/fm1pkg_make.py` | ChoralRoot |
| the CRC compensation that keeps sector 0x4000 the same bytes in every build | `tools/fm1pkg_make.py` | ChoralRoot |
| the update loader, staged at 0xE0000, USB `ota-FM-1_900` | `firmware/loader/` | Felucca / SLOOP, with ChoralRoot's order and **TransitFM1's** |
| step 1: what the running firmware does when an installer asks for an update | `firmware/src/ota.c` | Felucca / SLOOP / ChoralRoot, **TransitFM1's** step 1 |
| the app: a screen, USB-MIDI for the updater, the boot guard | `firmware/src/` | Dinghy, without the sound |

ChoralRoot's documentation of the stub is its `docs/BOOT-SAFETY.md`, section "As built". It covers the layout,
the marker, the rescue key (OCT- alone at power-on), the rescue count, the LED flashes, and what was tested on a
unit. TransitFM1 changes nothing in the stub: the same sources build to the same bytes, and `tools/build.py`
checks them against `firmware/stub/stub.lock`.

### What the stub decides at every power-on

1. Is OCT- alone held? Has the app asked for a rescue?
2. If not, is the marker valid, and does the body's CRC-32 match it? Then **boot**.
3. Otherwise **rescue**. If a rescue attempt already counted, enter **the ROM's USB boot** (`WL82 UBOOT1.00`).
   Otherwise, if a whole Felucca-family loader is staged at 0xE0000, count one attempt, flash three times and
   **start that loader**. If none is staged, flash once, long, and enter the ROM's USB boot.

The rescue starts whatever loader is staged. So TransitFM1 makes sure the staged loader is always its own and
whole:

- **At every boot** (`ota_boot_cleanup`, then `ota_own_staged` / `ota_restage`), a whole loader of ours is kept and
  anything else is erased. Then TransitFM1 stages its own copy (built into the app, 7.5 KB) if what is staged is
  not that copy, byte for byte. This also covers a unit written with the Transporter or MvaveFM1Unbricker, which
  stage nothing.
- **At every install** (step 1, `ota_stage`), TransitFM1 checks the package's header and makes sure its own loader
  is staged. **It never stages the package's loader**, whatever the package carries: the official one, Felucca's,
  Bubba Machine's. A step 1 that fails, is refused or loses the host leaves our loader as it is.

### The loader's order (`firmware/loader/ldr_core.c`)

The loader reads the package in address order, for the package CRC. It holds one **commit sector** back in RAM
and writes it only after every other sector is written and verified **and** the CRC of the whole package holds:

- **A package with the marker** (TransitFM1's, ChoralRoot's): the commit sector is the marker's (0x91000). This is
  ChoralRoot's order: uncommit, then sector 0x4000 if it differs, then the body, then the marker.
- **Any other package**, i.e. a foreign firmware: the commit sector is **0x4000 itself**, the new firmware's
  directory and entry point. The order is: erase the running firmware's marker (uncommit), write 0x5000..0x92FFF,
  then 0x4000 last. Until that last erase, sector 0x4000 still holds TransitFM1's stub. The stub finds no marker
  and hands over to the staged loader, our own. The window is that last sector's erase and its first 15 pages:
  16 flash operations, about 0.1 s of a write that takes a minute or two.

A pass whose CRC does not add up (a byte damaged in transit) writes no commit sector. The next pass rewrites what
differs and then commits. The flash head (0x0000..0x3FFF, the SPL) is never written.

### The identities

| what | answers | why |
| --- | --- | --- |
| the running app (and its USB rescue) | `FM-1_015`, USB device "FM-1" | the official V15's: every installer that guards against other firmwares lets an install over V15 through |
| the update loader | `ota-FM-1_900`, USB device "Felucca Update" | the update mode the Felucca-family installers and the FM-1 Firmware Hub resume after a cut (they resume only `ota-FM-1_9NN`) |
| the update record (RAM 0x01C7FD88, flash 0xE4F00) | `ota-FM-1_015` | the same bytes as ChoralRoot's record, which the stub writes for its own hand-over |

Dinghy's USB rescue (the boot guard's) answered `FM-1_000`, which ChoralRoot's installer refuses as "Sloop's
rescue mode". TransitFM1's answers `FM-1_015` too.

## The numbers (host, `tests/run_tests.sh`)

Power was cut after every erase and every programmed page. The flash as it stood went to the stub's own decision
(the device code), and the install was resumed from there to a bit-exact end:

| hop | cut points | rescued | the end | old firmware whole | unprotected |
| --- | --- | --- | --- | --- | --- |
| TransitFM1 → the official V15 | 2432 | 2415 | 1 | 0 | **16** (the last sector) |
| the official V15 → TransitFM1 | 2432 | 2414 | 1 | 0 | 17 (the marker's erase, then the first sector) |
| TransitFM1 → ChoralRoot 0.25.1, and back | 2279 | 2278 | 1 | 0 | **0** |
| TransitFM1 → another TransitFM1 body | 256 | 255 | 1 | 0 | **0** |
| a stub-less layout → TransitFM1 | 273 | 255 | 1 | 1 | 16 |

The stub's own test (16143 checks), the package builder's (265), and step 1's (with M-VAVE's V15 as a foreign
package; the loader check against the stub's on 20000 damaged copies) pass too.

## Limits

- **From Bubba Machine there is no way in.** Its updater accepts only a package with M-VAVE's own update loader and
  the unit's boot head (its `hal/fm1/fm1_update.c`). Go to V15 with Bubba's installer first.
- **Arriving is as safe as the firmware you leave.** The write into TransitFM1 is done by the loader your current
  firmware stages: ours, if that firmware stages the package's loader, as V15, Felucca, SLOOP and ChoralRoot do.
  Until sector 0x4000 holds the stub, no rescue code of ours is on the unit: the first 16 or 17 flash operations,
  once per unit. From ChoralRoot there is no such window.
- **Leaving has its window at the end:** the new firmware's sector 0x4000. A torn sector there may leave the unit
  dark (the Transporter).
- **A rescue that fails twice** (no normal boot between two arrivals) ends in the ROM's USB boot. That's
  recoverable with MvaveFM1Unbricker on Windows or Linux, not on macOS.
- **Data.** TransitFM1 reads and writes nothing but its staging area (0xE0000..0xE4FFF). What the previous firmware
  stored stays where it was. The next firmware finds it, as it would after any install.

## On the unit (0.1, 2026-10-17)

The owner, with the release package (`transit-0.1.fwsc`, sha256 `9bcee26a…`):

- **Installed from the web installer** (<https://quixotic7.github.io/TransitFM1/>): no problems.
- **TransitFM1 to other firmwares**: works.
- **The USB cable pulled during an install**: the FM-1 came back in update mode, and the install finished from
  there. This is the foreign order and the kept loader working on the unit, not only on the host.
- **Bubba Machine and back**: Bubba Machine installed; over it the web installer refused (Bubba Machine accepts only
  M-VAVE's own package) and said to go to the official V15 first; from V15, TransitFM1 again: works.

Not reported separately: OCT- at power-on (the stub's rescue key, tested on ChoralRoot's units), TransitFM1 to and
from ChoralRoot, another firmware's own installer over TransitFM1. The one window left, a cut inside the new
firmware's sector 0x4000 (about 0.1 s), cannot be hit by hand.

## The device test plan

The steps for the next version, with the Transporter at hand, a full battery, and M-VAVE's `FM-1.fwsc`:

1. From V15 (or ChoralRoot), install `build/transit.fwsc` with `tools/fm1_install.py`.
   - The screen should read TRANSIT, "AS V15 (FM-1_015)", and no red line.
   - `python3 tools/fm1_install.py --info` should give `FM-1_015 [running]` on the port "FM-1".
2. Hold **OCT-** at power-on. Expect three short flashes, a black screen, and `ota-FM-1_900` ("Felucca Update").
   Install `transit.fwsc` again to leave (nothing is written).
3. **Leave to V15** with `fm1_install.py FM-1.fwsc`. The FM-1 should come back as `FM-1_015`, running stock V15.
   Then return to TransitFM1.
4. **The cable pull:** start an install of V15 (or ChoralRoot), pull the cable at about 30 %, and switch the FM-1
   off and on. Expect three flashes and `ota-FM-1`. Run the same install again: it should finish.
5. **TransitFM1 → ChoralRoot → TransitFM1.** ChoralRoot's `--debug` should report "boot stub v3 ... marker ok".
6. **Another firmware's own installer** (the FM-1 Firmware Hub, a Felucca release): it should see an FM-1 on V15
   and install over it.
