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
- The host prints each `cGZFramework` startup phase as it changes.

### Fixed
- Headless `GetDC(NULL)` returns a screen-compatible memory DC instead of 0: SC4 read
  a 0-bit display from it and crashed on a null view (0x0098E9A6).

### Notes
- Needs pcrecomp#149: SimCity 4's SteamStub build leaves the code unencrypted,
  which `steamstub.py` refused before it.
