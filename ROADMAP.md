# Roadmap

## Now: P0, bring-up
- [x] Remove the Steam wrapper (pcrecomp#149)
- [x] Function catalog without IDA (disasm32 + vtable_scan seeds)
- [x] Lift the OEP closure, build, and boot the CRT into `WinMain`
- [x] Lift everything (`--all`); boot to the main menu headless with the software renderer
- [ ] Conformance harness: milestone lines (framework phase 7, first frame, region view, city load)
- [x] Input scripting (kotor's `input.c`): click into a region from the menu
- [ ] Toolkit: disasm32 missed 17 `mov reg, offset fn` callbacks (seeded here for now)
- [ ] Toolkit: detect a statically linked `_CxxThrowException` instead of `FORWARD`
- [ ] Speed: lifted runs the menu at a third of the original's frame rate

## Next
- [x] OpenGL renderer path to the region view and a city (white terrain: texture max level)
- [x] OpenGL: water renders pale in the city view (LIGHT0 left at GL defaults)
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
