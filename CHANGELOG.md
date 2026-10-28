# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- Reconnaissance of the Steam copy (docs/RECON.md).
- The pipeline from KotOR's shape: `tools/setup.ps1` (unwrap, vtable scan,
  catalog, lift, build), `run_lift.py`, and a native32 host (`src/runtime/host.c`).
- Needs pcrecomp#149: SimCity 4's SteamStub build leaves the code unencrypted,
  which `steamstub.py` refused before it.
