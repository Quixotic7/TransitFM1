# TransitFM1

**A tiny firmware for the M-VAVE FM-1 to stop at between two others.**

Moving an FM-1 from one custom firmware to another is where units get bricked. The usual advice is to go back to
the official V15 first. But V15 has no protection against an interrupted write, so the step **out of** V15 into a
custom firmware is the riskiest one. If the cable comes out or the battery dies partway, the FM-1 can stay dark.

TransitFM1 is a stop that is safe to leave. It makes no sound and stores nothing. It shows what it is, then waits
for you to install the next firmware. Three things make it a good stopover:

- **Every installer treats it as the official V15.** It answers the update handshake as `FM-1_015`, and its USB
  device is named "FM-1". An installer that refuses other custom firmwares writes over TransitFM1 as it would over
  stock.
- **It brings ChoralRoot's flash safeguards.** That's ChoralRoot's boot stub, byte for byte, in front of the app
  (docs/SAFETY.md). It checks the firmware before starting it. If the write was cut off, it starts update mode
  instead: three short flashes, then `ota-FM-1` over USB. Holding **OCT-** at power-on does the same.
- **It writes the next firmware itself.** When you install another firmware from TransitFM1, its own update loader
  does the writing, whatever loader the package carries. It saves the new firmware's first sector for last. A cut
  at any point before that last sector leaves an FM-1 that comes back in update mode: switch it on, run the
  installer again.

> **Status: 0.1.1 replaces 0.1.** 0.1 faulted on its first boot after the official V15, while it staged its rescue
> loader (the unit ended in the USB rescue, then the ROM's USB boot; seen on a unit, recovered with the
> Transporter), and stopped taking the installer's messages while a DAW sent MIDI to the FM-1. 0.1.1 fixes both
> (docs/SAFETY.md "0.1's two bugs"); the fixes are host-tested. From ChoralRoot, 0.1 worked on the unit: to other
> firmwares, and a cable pulled mid-install came back in update mode. Keep the [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) or
> [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) at hand.

## How much of each write is protected

These are measured on the host. Power was cut after every flash erase and every programmed page, the real stub
code judged the flash as it stood, and the install was resumed (`tests/ldr_test.c`). "Unprotected" cuts leave
neither a working stub nor a whole firmware.

| hop | cut points | rescued | unprotected |
| --- | --- | --- | --- |
| TransitFM1 → the official V15 (any firmware without ChoralRoot's stub) | 2432 | 2415 | **16**, the last sector only (about 0.1 s) |
| TransitFM1 ↔ ChoralRoot 0.25.1 | 2279 | 2278 | **0**: sector 0x4000 is the same bytes in both |
| TransitFM1 → a later TransitFM1 | 256 | 255 | **0** |
| the official V15 → TransitFM1 | 2432 | none to update mode: 2414 end in the ROM's USB boot (V15 stages the loader at 0xE6000, where the stub does not look), recoverable with MvaveFM1Unbricker | 17, the first sector (once per unit) |

Compare the plain address-order write that V15, Felucca and most custom firmwares use. Once the first sector is
written, no rescue code is left on the unit, so almost the whole one-to-two-minute write is unprotected.

## Using it

1. **Getting there.** The web installer: **<https://quixotic7.github.io/TransitFM1/>** (Chrome or Edge, the FM-1
   connected by USB): **Install TransitFM1**. It checks the package against its published SHA-256, says what the
   FM-1 runs, refuses the firmwares an install has bricked from (Sloop, the Felucca betas, unless you tick the risk
   box) and Bubba Machine, and finishes an install that was cut off. Or `python3 tools/fm1_install.py
   transit-X.Y.fwsc`, ChoralRoot's `tools/fm1_install.py PACKAGE`, or any installer that takes a Felucca-family
   package. From the official V15 it's the same: TransitFM1 installs like any custom firmware.
   - **Not from Bubba Machine.** Its updater only accepts M-VAVE's own package. Go back to V15 with Bubba's
     installer first.
2. **The screen** says `TRANSIT`, its version, "READY FOR THE NEXT FIRMWARE" and "AS V15 (FM-1_015)".
   - A red **LOADER NOT STAGED** line means TransitFM1 could not set up its rescue. It then refuses updates.
     Don't continue: report it.
3. **Leaving.** Open the installer of the firmware you want and install it. To that installer the FM-1 is an FM-1
   on V15. To go back to stock, install M-VAVE's `FM-1.fwsc` (the web installer's "Install a package file from
   here" takes it, or any other `.fwsc`).
4. **If the install is cut off,** switch the FM-1 off and on. You'll see three short flashes of the key lights, a
   black screen, and `ota-FM-1` ("Felucca Update") over USB. Open the installer again and run the same install:
   it finishes the write. An installer that knows Felucca's update mode resumes it by itself. That covers ChoralRoot,
   Felucca, Dinghy and the FM-1 Firmware Hub. Don't switch it off and on twice in a row while it's in update mode.
   A second arrival with no normal boot in between drops into the chip's ROM USB boot (`WL82 UBOOT1.00`), where
   MvaveFM1Unbricker takes over.

Nothing of yours is kept on TransitFM1. What the previous firmware stored stays in its flash area untouched, unless
the next firmware's installer or the firmware itself deals with it. Back on V15, presets may read garbled after
Felucca-family firmwares: System > Reset Patches.

## Building

You need Linux on x86-64 (or macOS / Windows with Docker), Python 3 and the JieLi toolchain. The three files of
the JieLi AC79 SDK the package needs are in `tools/sdk/` (Apache-2.0; a full SDK checkout at `~/fw-AC79_AIoT_SDK`
or `$AC79_SDK` is used instead when there is one):

```sh
pip3 install mido python-rtmidi             # (the installer only)
tools/get_toolchain.sh                      # JieLi's clang for the chip, to ~/.jieli/toolchain
./build.sh                                  # -> build/transit.fwsc (identity FM-1_015)
./build.sh --release 0.1                    # -> build/release-0.1/: the package, SHA256SUMS, the licences
tests/run_tests.sh                          # the host tests of the update path
V15_PKG=FM-1.fwsc CR_PKG=choralroot.fwsc tests/run_tests.sh   # also the hops to V15 and to ChoralRoot
node web/test_installer.mjs build/release-0.1/transit-0.1.fwsc  # the web installer against a fake FM-1
python3 tools/make_site.py build/release-0.1 build/site       # the site, as Pages publishes it
```

On macOS the build runs the toolchain in a `linux/amd64` Docker container (have Docker running). The build prints
the stub's hash and the hash of sector 0x4000. For the hops to and from ChoralRoot to stay unprotected-free, both
must be ChoralRoot's: stub `baee55ed…` (version 3) and sector `0f33c838…`. The build warns if the stub differs
from `firmware/stub/stub.lock`, and a release build stops.

The size: the app body is about 56 KB (ChoralRoot's is 543 KB), including a 7.5 KB copy of its own update loader.

## Releasing

1. `./build.sh --release X.Y`, twice (the first `build/release-X.Y` moved aside): the same `SHA256SUMS`.
2. `web/releases.json`: X.Y first, its date, tag `vX.Y`, the package's sha256 and 3 to 6 notes.
   `tests/run_tests.sh`, `node web/test_installer.mjs build/release-X.Y/transit-X.Y.fwsc`, `python3
   tools/make_site.py build/release-X.Y build/site`. Commit, push.
3. Push the tag `vX.Y`. `.github/workflows/release.yml` builds X.Y from source on GitHub (the toolchain natively,
   `tools/sdk`), runs the tests, stops if its package differs from the sha256 in `web/releases.json`, makes the
   release with the package, `SHA256SUMS`, `LICENSE`, `LICENSING.md` and `LICENSES.zip`, and starts
   `.github/workflows/pages.yml`, which publishes the site with that package.

Once, in the repository's Settings > Pages: Source "GitHub Actions". To publish the site again without a release,
run pages.yml by hand with the current version.

## What is where

```
firmware/stub/        ChoralRoot's boot stub, unchanged (stub.lock: version 3, the same hash as ChoralRoot's)
firmware/loader/      the update loader: Felucca's, with ChoralRoot's order and TransitFM1's (ldr_core.c)
firmware/src/ota.c    step 1: our own loader staged, kept, restaged; never the package's
firmware/src/platform.c, transit.c   Dinghy's platform without the sound; the screen
tools/build.py        the stub, the loader, the app with the loader inside, the package (fm1pkg_make.py)
tests/                the stub's decision, step 1, the package builder, the loader's interruption model
web/                  the web installer: index.html, transit.js (the page's logic, tested by test_installer.mjs),
                      fm1ota.js / fm1pkg.js (the update protocol and the package, ChoralRoot's), releases.json
tools/make_site.py    the site: the page, the package checked against releases.json, the licences beside it
tools/sdk/            three JieLi AC79 SDK files (Apache-2.0) the package is built with
LICENSES/             the licences of what is not GPL in a package: Apache-2.0 (the SDK files), OFL-1.1 (Terminus)
docs/SAFETY.md        how it works, the numbers, what is not tested, the device test plan
```

## License

GPL-3.0-only, as Felucca, SLOOP, Dinghy and ChoralRoot. You may change and share it, and whoever gets your firmware
gets its source. Credits and third-party parts are in [LICENSING.md](LICENSING.md). A package (`.fwsc`) also holds
three JieLi AC79 SDK files (Apache-2.0) and the Terminus font (OFL-1.1): whoever passes a package on passes
`LICENSE`, `LICENSING.md` and `LICENSES/` with it, as the releases and the site do.

TransitFM1 is based on [Dinghy](https://github.com/zvenson/dinghy) by zvenson. Dinghy is
[Felucca](https://github.com/hugelton/Felucca)'s platform (Leo Kuroshita, Hügelton Instruments) as
[SLOOP](https://github.com/isod89/sloop-fm1) (isod89) carries it. The boot stub, the stub's write order and the
package marker are [ChoralRoot FM-1](https://github.com/Quixotic7/ChoralRootFM1)'s. "M-VAVE" and "FM-1" belong to
their owners: TransitFM1 is not made or endorsed by them, and installing firmware is at your own risk.

Idea proposed by [Melodee](https://github.com/keremimo/melodee) developer [Kerem Kilic](https://github.com/keremimo)


