# Roadmap

## Now: P0, bring-up
- [x] Remove the Steam wrapper (pcrecomp#149)
- [x] Function catalog without IDA (disasm32 + vtable_scan seeds)
- [ ] Lift the OEP closure, build, and boot the CRT into `WinMain`
- [ ] Lift everything (`--all`); boot to the main menu headless with the software renderer
- [ ] Conformance harness: milestone lines (window, first frame, region view, city load)

## Next
- [ ] OpenGL renderer path to the region view
- [ ] Load a city, run the simulation, save; compare against the original
- [ ] `--headless --record out.mp4` (section 10)
- [ ] netlab recipe and a QA pass on testbox

## Later
- [ ] Score the catalog against IDA (`score_recovery.py`) and fix what it finds in disasm32
- [ ] Names from the 237 `cSC4*`/`cIGZ*` class-name strings
- [ ] DirectX 7 renderer path

## Out of scope
- Distributing the game, its data, or anything lifted from it (REPO_RULES section 3)
- Multiplayer/online region sharing (the servers are gone)
