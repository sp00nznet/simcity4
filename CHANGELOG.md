# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- Reconnaissance of the Steam copy (docs/RECON.md).
- The pipeline from KotOR's shape: `tools/setup.ps1` (unwrap, vtable scan,
  catalog, lift, build), `run_lift.py`, and a native32 host (`src/runtime/host.c`).
- The whole game lifts: 50,766 functions, 0 errors, 10.2M lines of C; builds on netlab.
- `--original` runs the shipping code under the same host (kotor's `oracle.c`).
- The host prints each `cGZFramework` startup phase as it changes, and every guest C++
  throw with its type (`--trace-init` traces the component init loop).
- `--snap DIR` saves the software renderer's frames as BMPs (headless screenshots);
  `--spy VA` logs one lifted function's indirect calls.
- The game boots headless to its main menu, matching the original's frames.
- Headless runs and `--private-docs` keep the game's Documents folder in `work\docs`.
- Scripted input: `--click`/`--rclick X,Y@S`, `--key NAME@S` (`CTRL+NAME` too); `@Nf`
  waits for frame N instead of a time.
- The lifted game loads a city, simulates and saves; the original loads that save.
  `tools/dbpf_diff.py` compares saves.

### Fixed
- OpenGL city view: white terrain (textures capped at the mip level uploaded), pale water
  (`LIGHT0` zeroed; SC4 lights on the CPU), black squares on lots and buildings (DXT1
  uploaded with its alpha).
- A blue wedge across the city in OpenGL, a bug in the game itself: the water-edge walls
  were drawn with a car's matrix. `run_lift.py` gained `PATCHES` (fixes applied to the
  lifted C); this is the first.
- The registry Steam's install script writes on first launch (`Install Dir`, language,
  registration) is served from `24780_install.vdf` in memory (`steamreg.c`): a copy never
  launched through Steam, or a test VM, failed the app's init without it. Nothing is
  written to the real registry.
- `GetModuleFileNameW`, fetched through `GetProcAddress`, names the guest exe: the game
  looked for its data beside the host.
- Guest C++ exceptions: the statically linked `_CxxThrowException` (0x009EF84F) forwards
  to native32's eh32, and the 38 functions with catch blocks get landing pads. The game's
  own "Couldn't open file" for the optional `Graphics Options.sgr` is caught again.
- 17 callbacks the catalog missed (the software renderer's span routines) are seeded.
- No audio device: `-audio:off` is added, or the sound manager's update writes through
  null (the original too).
- The software renderer's generated span code runs natively with the guest's registers
  (needs pcrecomp#153).
- Headless `GetDC(NULL)` returns a screen-compatible memory DC instead of 0: SC4 read
  a 0-bit display from it and crashed on a null view (0x0098E9A6).

### Notes
- Needs pcrecomp#149: SimCity 4's SteamStub build leaves the code unencrypted,
  which `steamstub.py` refused before it.
