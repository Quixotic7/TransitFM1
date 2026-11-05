# How TransitFM1 keeps a firmware change safe

TransitFM1 exists for one moment: the write that replaces it. Everything here serves one goal. Wherever the power
or the cable is cut during that write, the FM-1 should come back in update mode, not dark.

## The pieces

| piece | where | from |
| --- | --- | --- |
| the boot stub: flash 0x4120..0x4FFF, where the SPL jumps | `firmware/stub/` | ChoralRoot, byte for byte (version 3, sha256 `baee55ed…`) |
| the commit marker, flash 0x91FE0: magic, the body's length and CRC-32, its own CRC | `tools/fm1pkg_make.py` | ChoralRoot |
| the CRC compensation that keeps sector 0x4000 the same bytes in every build | `tools/fm1pkg_make.py` | ChoralRoot |
| the update loader, staged at 0xE0000, USB "Felucca Update", `ota-FM-1_900` | `firmware/loader/` | Felucca / SLOOP, with ChoralRoot's order and **TransitFM1's** |
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
| the update loader | `ota-FM-1_900`, USB device "Felucca Update" | the update mode the Felucca-family installers and the FM-1 Firmware Hub resume after a cut (they resume only `ota-FM-1_9NN`). Also what an installer that needs M-VAVE's updater recognises and refuses, rightly: Baud Girl's (below) |
| the update record (RAM 0x01C7FD88, flash 0xE4F00) | `ota-FM-1_015` | the same bytes as ChoralRoot's record, which the stub writes for its own hand-over |

**Telling TransitFM1 from V15** (0.1.4, issue 1): the handshake above stays V15's, byte for byte. For apps that
want to know, TransitFM1 says what it is where no installer looks:

- the USB serial number (string 3): `TransitFM1-0.1.4`. The official V15 has none;
- SysEx `F0 7D 54 46 31 01 F7` (`7D`, "TF1", 1) is answered `F0 7D 54 46 31 02 "TransitFM1 0.1.4" F7`. V15 doesn't
  answer it. No installer sends it.

The update loader has neither: in update mode it is `ota-FM-1_900`, as before.

Dinghy's USB rescue (the boot guard's) answered `FM-1_000`, which ChoralRoot's installer refuses as "Sloop's
rescue mode". TransitFM1's answers `FM-1_015` too.

## The numbers (host, `tests/run_tests.sh`)

Power was cut after every erase and every programmed page. The flash as it stood went to the stub's own decision
(the device code), and the install was resumed from there to a bit-exact end:

| hop | cut points | rescued | the end | old firmware whole | unprotected |
| --- | --- | --- | --- | --- | --- |
| TransitFM1 → the official V15 | 2432 | 2415 | 1 | 0 | **16** (the last sector) |
| the official V15 → TransitFM1 | 2432 | 0 (2414 to the ROM's USB boot) | 1 | 0 | 17 (the marker's erase, then the first sector) |
| TransitFM1 → ChoralRoot 0.25.1, and back | 2279 | 2278 | 1 | 0 | **0** |
| TransitFM1 → another TransitFM1 body | 256 | 255 | 1 | 0 | **0** |
| a stub-less layout → TransitFM1 | 273 | 255 | 1 | 1 | 16 |

The stub's own test (16143 checks), the package builder's (265), and step 1's (with M-VAVE's V15 as a foreign
package; the loader check against the stub's on 20000 damaged copies) pass too.

## Limits

- **Baud Girl** (FM-1+VA, closed source, its own installer at baudgirl.com): **go through the official V15**, and
  install Baud Girl from there with its installer. Its package's `flash.bin` reaches past the app area when its app
  outgrows the stock slot (its `fwsc.js` grows it), so only M-VAVE's loader, which writes the whole `flash.bin`,
  can install it. TransitFM1's loader writes the app area only: Baud Girl's installer refuses TransitFM1's updater
  (`ota-FM-1_900`, "another custom firmware"), and 0.1.3 refuses such a package itself, in step 1, in the loader and
  on the page. Into TransitFM1 from Baud Girl: not offered (its identity `FM-1_101` is one no table here knows); go
  through V15 too.
- **Any package whose `flash.bin` reaches past 0x93000** is refused (0.1.3), before anything is written: the same
  reason. Go through V15. From 0.1.4 the screen says why: "TOO LARGE FOR TRANSIT: INSTALL V15 FIRST", "NOTHING WAS
  INSTALLED" and the code (-4).
- **M-VAVE's V16** (`FM-1_016`, 2026-10-21) is refused the same way, after its header (2 requests). Its `flash.bin`
  is 0xA9000 B (V15's: 0x93000). V16 also brings a new boot head: `uboot.boot` 0x3898 B (V15's: 0x3830), a new
  `isd_config.ini`, and a new region table (VM 0xBA000, SEQ64 0xDA000). Felucca-family loaders, TransitFM1's
  included, never write the head 0x0000..0x3FFF, so even a longer app area would not install V16 whole: only
  M-VAVE's own updater does. **Go to V15, then V16 from V15 with M-VAVE's updater.** V16 carries the same update
  loader as V15 (hcrc EBAA, dcrc 5881). The page names V16 when it refuses it. (Back from V16 to V15, V15 may play
  one note at a time: V16 changes V15's settings record at 0xFBD80. That's M-VAVE's, m-vave/fm1_open#16.)
- **The flash chip.** The app, its USB rescue and the loader write only to the FM-1's 1 MiB part: Puya's 0x856014
  or, from 0.1.4, 0xB36014 (FM-1s with the logo on the front, late 2026; `firmware/hal/fm1_flash_id.h`). On another
  id the screen says "UNKNOWN FLASH CHIP" and every install is refused. **0.1.3 and earlier on a 0xB36014 unit**
  show exactly that: no installer gets past step 1, and OCT- at power-on doesn't help, because the loader refuses
  the chip too. The way out is the ROM's USB boot: TransitFM1 still takes the key `F0 22 24 35 7D F7` (MvaveFM1Unbricker
  sends it). From there, MvaveFM1Unbricker or the Transporter restores V15. Install 0.1.4 from V15.
- **From Bubba Machine there is no way in.** Its updater accepts only a package with M-VAVE's own update loader and
  the unit's boot head (its `hal/fm1/fm1_update.c`). Go to V15 with Bubba's installer first.
- **Arriving is as safe as the firmware you leave.** The write into TransitFM1 is done by the loader your current
  firmware stages: ours, if that firmware stages the package's loader, as V15, Felucca, SLOOP and ChoralRoot do.
  Until sector 0x4000 holds the stub, no rescue code of ours is on the unit: the first 16 or 17 flash operations,
  once per unit. From ChoralRoot there is no such window. **From the official V15** the stub cannot hand over to
  update mode at all during that write: V15 stages the loader at 0xE6000, and the stub looks at 0xE0000 only (seen
  in a Transporter dump, 2026-10-17). A cut after the first sector ends in the ROM's USB boot (`LDR_STAGED_ELSEWHERE=1
  ldr_test`: 2414 of 2432 cut points), recoverable with MvaveFM1Unbricker on Windows or Linux or the
  Transporter; not dark, but not update mode either. TransitFM1's own first boot then stages its loader at 0xE0000,
  and from there every write is guarded as above.
- **Leaving has its window at the end:** the new firmware's sector 0x4000. A torn sector there may leave the unit
  dark (the Transporter).
- **A rescue that fails twice** (no normal boot between two arrivals) ends in the ROM's USB boot. That's
  recoverable with MvaveFM1Unbricker on Windows or Linux, not on macOS.
- **Data.** TransitFM1 reads and writes nothing but its staging area (0xE0000..0xE4FFF). What the previous firmware
  stored stays where it was. The next firmware finds it, as it would after any install.

## 0.1's two bugs (found on a unit, 2026-10-17; fixed in 0.1.1)

Found by the RemoteFM1 session, a clone of TransitFM1, on unit 5BD75BE3 (its handoff:
`../RemoteFM1/handoff/TRANSITFM1-FIXES.md`).

1. **The loader restage faulted.** `ota_restage` handed `ota_prog` the app's const copy of the loader, which lives in
   XIP flash. The flash driver reads a page's bytes while the flash is out of XIP (`hal/fm1_flash.h fl_prog_ram`),
   so the first page faulted. It ran at every boot whose staging area was not our loader, byte for byte, and that
   is **the first boot after the official V15 installs TransitFM1** (V15 stages at 0xE6000; 0xE0000 is empty). It
   ran before `lcd_init`, so no crash screen showed. After two faults came the boot guard's "TRANSIT USB RESCUE".
   There an install's step 1 restaged again, faulted again, and dropped to the ROM's USB boot. The Transporter dump
   showed the app area exactly transit-0.1, 0xE0000..0xE4FFF erased, and TransitFM1's loader at 0xE6000. From
   ChoralRoot, which stages at 0xE0000, the staged loader was already ours, and nothing was restaged: that is why
   the earlier tests passed.
   **0.1.1:** `ota_restage` copies each piece to RAM before programming it. The app's `ota_prog` refuses any source
   in the XIP window (`hal/fm1_xip.h fm1_in_xip`); the audit found no other such call (the record and the head are
   RAM; the loader programs from RAM only). The boot stages after `lcd_init` ("STAGING THE LOADER"), and
   `fm1_fault` brings the panel up if a fault comes first. Tests: `tests/ota_test.c`'s flash model stops on a
   program from the loader copy (0.1's `ota.c` stops there at once). The boot (`ota_boot_stage`) is tested over an
   empty area, over V15's layout (our loader at 0xE6000, 0xE0000 empty), over M-VAVE's loader and a record at
   0xE0000, and over a damaged copy of ours: each ends with ours staged.
2. **MIDI in was never drained.** The USB stack queues every channel and real-time message and NAKs EP1 OUT while its
   ring has fewer than 24 of 64 slots free. Dinghy's audio drained it, and went with the sound. After about 40
   events from any app (a DAW's clock), the installer's SysEx could no longer arrive: `--info` answered, then step 1
   stalled. **0.1.1:** the app and its USB rescue drop non-SysEx MIDI every pass of the main loop. The loader is
   unchanged, byte for byte (`ota.bin` sha256 `68599862…`): in update mode, close any app that sends MIDI to the
   FM-1 (as with every installer).

## On the unit

**0.1** (2026-10-17, the owner, `transit-0.1.fwsc` sha256 `9bcee26a…`). Installed from the web installer over
ChoralRoot: no problems. TransitFM1 to other firmwares: works. The USB cable pulled during an install from
TransitFM1: the FM-1 came back in update mode, and the install finished from there. This is the foreign order
and the kept loader working on a unit. Bubba Machine installed; over it the web installer refused (Bubba Machine
accepts only M-VAVE's own package) and said to go to V15 first. **The hop from V15 back to TransitFM1 met bug 1**
(above): TRANSIT USB RESCUE, then the ROM's USB boot, recovered with the Transporter.

**0.1.2 bricked a unit (2026-10-17, withdrawn the same day).** 0.1.2's loader answered `ota-FM-1_015` when
writing a package with M-VAVE's loader, so that Baud Girl's installer would install from TransitFM1. That took
Baud Girl's installer past its own guard: it refuses a Felucca-family updater because such a loader writes the app
area only, and Baud Girl's image is longer (its `ota.js`: "it writes only the start of our image ... the FM-1 would
not start"). It was right. On the owner's unit: TransitFM1 0.1.2 from the site, then Baud Girl with Baud Girl's
installer: TransitFM1's loader wrote 0x4000..0x92FFF, the package CRC held, sector 0x4000 was committed, and the
unit was left with a truncated Baud Girl and no stub: dark, hard bricked (the Transporter). The site went back to
0.1.1 within the hour.

**0.1.3**: the loader answers `ota-FM-1_900` again (0.1.1's step 1 and loader), and a package whose `flash.bin`
reaches past the app area is refused in step 1 (-4), in the loader (`LDR_ETOOLONG`, before any erase) and on the
page. Host tests: `OTA_LONG_PKG` (ota_test), `LDR_EXPECT_TOO_LONG` (ldr_test), `LONG_PKG` (test_installer), with a
package of a 0x9D000 `flash.bin`.

**0.1.3 on the unit** (2026-10-18, the owner, unit 5BD75BE3 after its Transporter recovery to V15;
`transit-0.1.3.fwsc`, sha256 `d1b027d5…`), one firmware after another, every hop a success but the one refused:

1. V15 → Baud Girl (Baud Girl's installer).
2. Baud Girl → TransitFM1 (TransitFM1's live installer).
3. TransitFM1 → Baud Girl: **refused gracefully** by Baud Girl's installer (TransitFM1's updater, `ota-FM-1_900`, is
   "another custom firmware" to it), nothing written, TransitFM1 still running. The behaviour 0.1.2 broke.
4. TransitFM1 → Sloop.
5. Sloop → TransitFM1.
6. TransitFM1 → ChoralRoot.
7. ChoralRoot → Bubba Machine (the FM-1 Firmware Hub).

Not covered by this run: a cut during a write from TransitFM1 on 0.1.3 (0.1's cable pull, from TransitFM1, was
rescued; the write order is unchanged since), OCT- at power-on, and Bubba Machine → TransitFM1 (refused by Bubba's
updater, which takes only M-VAVE's own package: go through V15).

**0.1.1** (released 2026-10-17, `transit-0.1.1.fwsc`, sha256 `e074eabd…`): host-tested; on a unit still to do, first of all **V15 →
TransitFM1**: the first boot shows "STAGING THE LOADER", then the TRANSIT screen with no red line; then OCT- at
power-on gives three flashes and update mode; an install with a DAW sending clock to the FM-1 goes through.

## 0.1.4 on the unit (2026-11-04)

A unit running RemoteFM1 0.3.3 (`FM-1_52000303`; its flash chip was not read), with `tools/fm1_install.py`:

1. **RemoteFM1 → TransitFM1 0.1.4** (sha256 `3ba85410…`): 19 reads in step 1, the write, `FM-1_015`.
2. **The identity:** `--info` gives `FM-1_015`. The USB serial number reads `TransitFM1-0.1.4` (ioreg). `F0 7D 54 46 31 01 F7`
   is answered `7D 54 46 31 02 "TransitFM1 0.1.4"`. ChoralRoot's installer still says "the official M-VAVE firmware".
3. **V16** (M-VAVE's `FM-1_016`, sent past the tool's own package check): "stopped answering after 2 requests;
   nothing was written". The FM-1 then still ran TransitFM1. The screen's text was not read in this run.
4. **V15 right after**, no power cycle (ChoralRoot's `fm1_install.py`): done, `FM-1_015`, no serial number: stock.
5. **Back to RemoteFM1 0.3.3 from that V15.** RemoteFM1's installer got "0 requests" twice, 15 s apart. Then
   ChoralRoot's tool installed V15 over that V15 (47 reads, done), and TransitFM1's tool installed RemoteFM1 0.3.3
   at the first try (25 reads, `FM-1_52000303`). The two tools send the same messages, so the likely cause is the
   V15 that TransitFM1's loader had written: its first update request was ignored until M-VAVE's own loader wrote V15
   again. **Not reproduced** in the second run (below).

**The second run** (the same unit and day, the FM-1 two USB hubs deep):

1. RemoteFM1 → TransitFM1 → V15 (ChoralRoot's tool) → RemoteFM1 with RemoteFM1's own installer: all done at the
   first try (25 reads). The "0 requests" above did not come back.
2. Again with the V16 refusal before V15: TransitFM1, V16 refused after 2 requests, then V15. That write **lost the
   connection after 708 requests**. RemoteFM1's installer found `ota-FM-1_900` ("Felucca Update") and continued
   with RemoteFM1's package. It lost the connection too, after 225 requests. The unit stayed in update mode and kept
   answering. One more install of RemoteFM1 0.3.3 with `tools/fm1_install.py` wrote the whole package: done,
   `FM-1_52000303`. The cut-off write was rescued, as designed.
   - The cause of the two drops is not known: USB (two hubs), the host, or the loader's watchdog (~8 s, fed by
     ldr_poll). The loader's only 0.1.4 change is the chip id check, done once before anything is written.
   - The screen was black (update mode) when it was looked at, so the V16 reason text is still unread.

The run also found that `tools/fm1_install.py` called M-VAVE's own V15 "damaged (USR: CRC)". Its package check
compared every entry's CRC, and only flash.bin and ota.bin carry one (fixed; `tests/install_pkg_test.py`).

## The device test plan

The steps for the next version, with the Transporter at hand, a full battery, and M-VAVE's `FM-1.fwsc`:

1. **From the official V15** (and from ChoralRoot), install the package with `tools/fm1_install.py` or the web
   installer's "Install a package file".
   - The first boot after V15 shows "STAGING THE LOADER" for a moment; then the screen reads TRANSIT,
     "AS V15 (FM-1_015)", and no red line.
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
7. **A DAW sending MIDI** (clock, notes) to the FM-1 for a minute, then an install: step 1 must go through.
8. **0.1.4's reasons:** install M-VAVE's V16 `FM-1.fwsc` with fm1_install.py (the page refuses it before asking
   the FM-1). The screen should read "UPDATE", "FAILED", "TOO LARGE FOR TRANSIT:", "INSTALL V15 FIRST", "NOTHING
   WAS INSTALLED" and "CODE -4", with nothing written. A V15 install right after it should go through without a
   power cycle.
9. **0.1.4's identity:** the USB serial number reads `TransitFM1-0.1.4` (macOS: `ioreg -p IOUSB -l -n FM-1 | grep
   'USB Serial'`). SysEx `F0 7D 54 46 31 01 F7` is answered "TransitFM1 0.1.4". `fm1_install.py --info` still
   gives `FM-1_015`. ChoralRoot's and the Firmware Hub's installers still see V15.
