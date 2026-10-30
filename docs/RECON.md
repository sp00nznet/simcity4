# SimCity 4 Deluxe (PC) reconnaissance

Reconnaissance of the Steam copy (`steamapps\common\SimCity 4 Deluxe`, app 24780),
done 2026-10-28 with pcrecomp `origin/main` (a85ea97) plus pcrecomp#149.

## The binary

| | |
|---|---|
| File | `Apps\SimCity 4.exe`, 7,876,608 bytes, SHA1 `0a59cdfb0940614c2d596dfed6f2b22ab65a7bce` |
| Unwrapped | `work\sc4.exe`, 7,524,352 bytes, SHA1 `34fa29c356c470a4be1fbd8cf440563a7ed017c9` |
| Build | PE32 i386 GUI, linker 7.10 (VC.NET 2003), timestamp 2010-06-11 22:59:03 UTC (the Steam rebuild) |
| Image | base 0x00400000, SizeOfImage 0x7C8000 after unwrapping, **no relocations** (must load at its base) |
| Code | `.text` 0x00407000-0x00A80000, 6.79 MB, plaintext (entropy 6.43) |
| Data | `.rdata` 552 KB, `.data` 737 KB virtual (106 KB raw), plus `LBMPEG_D`, `.data1`, `STLPORT_` |
| Wrapper | `.bind` 0x00BC8000, 344 KB, held the entry point: SteamStub 2.x |
| OEP | 0x009F15DC: MSVC CRT start (`push 0x60; push SEH; call __SEH_prolog`) |
| Imports | 399 from 13 DLLs: KERNEL32 (162), USER32 (87), OPENGL32 (67), WSOCK32 (22), GDI32 (15), WININET (11), WINMM (9), IMM32 (8), ADVAPI32 (6), SHELL32 (4), VERSION (3), MSVFW32 (3), DSOUND (2, by ordinal) |
| Data path | `Apps\SimCity 4.ini`: `Data=..\`, `PlugIn=..\Plugins\` |

Libraries linked in, by section name and strings: STLport, an MPEG decoder
(`LBMPEG_D`), Intel JPEG (`ijl1*.dll`, loaded at run time), and DbgHelp.

## Renderers

Three, chosen by the `-d:` switch and the `Graphics Rules.sgr`/`Video Cards.sgr`
tables in the install: **DirectX 7** (`DDRAW.DLL` loaded at run time, not imported),
**OpenGL** (imported) and **Software** (`GDriverClass--Software`). The software driver
is the bring-up target: it needs nothing from a GPU, so it runs on a test VM and headless.

## The Steam wrapper (SteamStub 2.x, x86, code not encrypted)

Same 2.x stub layout as KotOR (header XOR-chained, payload, XTEA-encrypted
`steamdrm.dll` of 0x51C00 bytes inside `.bind`), but this payload's flags are
`0x26`: bit 2 says the code is **not encrypted**. That `steamdrm.dll` build reads the
unused code-range, AES-key and first-block fields as `[eax+0]`, which `steamstub.py`
skipped, so it found 5 of 7 fields and refused the file. Fixed in pcrecomp#149: the
zero-displacement reads count, and with the flag set the code section is the one
holding the OEP. The result is a clean PE with `.bind` dropped.

## Names

| Source | What it gives |
|---|---|
| RTTI (`cpp/rtti.py`) | Off. 52 classes, all CRT/STL (`std::*`, exceptions); 206 methods, 188 named |
| Vtables without RTTI (`cpp/vtable_scan.py`) | 1,139 vtables (1,105 in `.rdata`), 18,437 distinct methods; median 30 slots, largest 1,965 |
| Strings | 237 class names (`cSC4*`, `cIGZ*`, `cISC4*`, `cSC424HourClock`, `cSC4AdvisorFinance`, ...), no `.cpp` paths |
| Exports, map, PDB | None |

The game is built on Maxis's GZCOM framework: every system is a class behind a
`cIGZ*`/`cISC4*` interface, reached through vtables. So most methods are named by no
`call`, and seeding the catalog with the vtable slots is required, not optional.

## Function catalog (disasm32, no IDA)

`disasm32.py work\sc4.exe --seed-functions work\vtable_seeds.json`, 7.5 minutes:

| | |
|---|---|
| Seeds, first pass | 38,582 |
| Discovery rounds | 7 (+6,390, +154, +29, ..., +14) |
| Functions | **50,604** (89 thunks, 17,371 leaves); 3,402 aliases, 358 EH funclets |
| Instructions | 6,362,002 |
| Byte coverage | 6,585,200 of 6,787,072 (**97.0%** of `.text`) |

Not yet scored against a reference (ROADMAP).

## The lift

`run_lift.py --all`, 172 s: **50,766 functions lifted, 0 errors**, 136 branch targets
added beyond the catalog, 548 with no clean terminator. 10.2 million lines of C in
127 files (490 MB). The OEP closure alone is only 300 functions: the CRT reaches
the rest through `_initterm`'s constructor table and the GZCOM vtables.

## What this means for the recomp

- **Front end**: complete. A plain MSVC 7.1 PE32 is what disasm32/lift32/native32 were
  built on (KotOR, The Movies, Red Alert 2).
- **Symbols**: weak. No map, no RTTI; names come from vtables and class-name strings.
  This is the recomp-ideas rule's warning case, offset by the size of the modding
  community's reverse-engineering of the GZCOM interfaces.
- **Size**: the largest PC target so far by code (6.8 MB `.text`, vs KotOR's 3.4 MB).
  Builds go to netlab (REPO_RULES section 10).

## Bring-up so far (2026-10-28)

`build\sc4.exe --run --headless --watchdog 60`, full lift built on netlab (clang-cl, 70 min cold):

1. The CRT, its static constructors and the GZCOM framework start in lifted code.
2. **Fixed:** a fault at 0x0098E9A6 (a null view object). `--original` (the shipping code
   under the same host, `oracle.c`) faulted at the same address, so the lift was right and
   the host was wrong: its headless `GetDC(NULL)` returned 0, SC4 read a 0-bit screen depth
   from it (0x0098ADE0) and picked no video mode. Headless now hands out a screen-compatible
   memory DC.
3. DirectDraw's exclusive fullscreen fails headless (`SetDisplayMode` E_NOTIMPL); the game
   says "Could not initialize Direct Draw" and falls back to its software driver on a GDI DIB
   section. It creates `GDriverWindow--Software` at 1024x768.
4. **Fixed:** `cGZFramework` startup (0x0087AF13) failed its phase 4: the component loop
   (0x00879FBC) asks each registered component, by priority, and the app itself (vtable
   0x00A86ABC, init 0x0044C320) refused. `--trace-init` showed which. Its init reads
   `HKLM\SOFTWARE\Maxis\SimCity 4\Install Dir`, and this copy was never launched through
   Steam, which writes those keys on first launch from `24780_install.vdf`. The original
   fails the same way. The host now serves the script's keys from memory (`steamreg.c`):
   nothing is written to the real registry, and a test VM needs no setup.
5. **Fixed:** the game took its data folder from `GetModuleFileNameW`, fetched with
   `GetProcAddress` (0x00912467) and so not shimmed: it looked for `Graphics Rules.sgr`
   beside the host. `GetProcAddress` now hands out a stand-in that names the guest exe.
6. **Fixed:** the game throws `nSCRes::cParseError` "Couldn't open file
   '...\Documents\SimCity 4\Graphics Options.sgr'", an optional file it catches itself.
   SC4 links its C runtime statically, so its `_CxxThrowException` (0x009EF84F) is game code
   that calls `RaiseException`, and Windows never saw the guest's EH frames. `run_lift.py`
   now forwards that function to native32's `eh32` dispatcher (`FORWARD`) and gives the 38
   functions with catch blocks their landing pads (`eh32.resume_points`, kf's way).
   `--original` cannot catch it either: Windows refuses SEH handlers outside a loaded image
   when the host is `/NXCOMPAT`, so the oracle stops at the first guest throw.

7. **Fixed:** the main loop's software renderer calls 17 span and blit callbacks that
   disasm32 missed (named only by `mov reg, offset`); seeded in `run_lift.py`.
8. **Fixed:** the renderer compiles span code at run time into a `PAGE_EXECUTE_READWRITE`
   buffer (0x008EC050) and calls it from hand-written MMX (0x008DD8B0) with its inputs in
   esi/edi/ebp and MMX registers. native32 ran it through the C-ABI bridge, so it read 0.
   native32 now tracks executable memory the guest allocates and enters it with the whole
   register file (pcrecomp, `feat/native32-generated-code`).
9. **Fixed:** with no audio output device, DirectSound fails, the sound manager's init
   never runs and its per-frame update writes through a null buffer object (0x00876C03).
   The original does the same; `-audio:off` avoids it and the host adds it when there is
   no device.
10. **Now:** the main loop runs and presents frames. The splash screen and the main menu
   render, and the original code under the same host (`--original`, with an empty
   `Graphics Options.sgr` in `work\docs\SimCity 4` so it never throws) draws the same
   frames, cloud streaks included. Speed: about 110 frames a second lifted, 340 original,
   on the main menu.
11. **Fixed (2026-10-29): white city terrain on OpenGL** (`-d:OpenGL`), in `--original`
   too, so a modern-driver problem, not the lift. SC4 uploads only level 0 of its terrain
   textures and never sets their min filter (it caches filter state per texture unit, the
   Direct3D way), so they keep GL's default `GL_NEAREST_MIPMAP_LINEAR`: incomplete, and
   sampled as white. The additive splatting passes (`SRC_ALPHA, ONE`) then sum to white.
   The host's `glTexImage2D` / `glCompressedTexImage2DARB` shims now cap
   `GL_TEXTURE_MAX_LEVEL` at the level uploaded. Found with `--gltrace-busy 100@1500`
   (every GL call in the first busy city frame) and `--gltex` (what the driver holds for
   the texture each draw samples).
   Water was pale and the terrain grey-washed: SC4 lights terrain and water on the CPU
   into vertex colours and draws them with `LIGHT0` on, `COLOR_MATERIAL` and a 1.0 global
   ambient, but never calls `glLightfv`, so GL's default white diffuse and specular add
   on top. The host's `glLightModelfv` shim zeroes `LIGHT0`'s diffuse and specular.
   Lots and building sprites drew as black squares: SC4 uploads DXT1 as
   `GL_COMPRESSED_RGB_S3TC_DXT1` (83F0), which turns DXT1's cut-out texels black; Direct3D
   keeps them transparent. The compressed upload shim asks for RGBA DXT1 (83F1) instead.
   Scripted input (`src/runtime/input.c`) gets there: `--click 616,330@10 --click 716,321@22
   --click 775,222@30` = main menu, Timbuktu, Getting Started, Start New City.

12. **Load, simulate, save (2026-10-30).** The lifted game opens "Making Money Tutorial",
   leaves the tutorial for normal play, simulates a month (advisor news arrives, the date
   runs 11/11/08 to 12/22/08) and saves from the menu ("City Saved"). `--original` does the
   same from the same script. Both saves hold the same 150 DBPF entries
   (`tools/dbpf_diff.py`); contents differ, as two runs of the simulation do. The original
   loads the lifted game's save (population 2,070, $49,304) and plays on. The script,
   timed in frames so a busy machine can't throw it off:
   `--click 616,330@150f --click 465,250@700f --click 571,228@1000f --click 905,285@2000f
   --click 755,195@2300f --click 632,486@2900f --click 632,488@3600f --click 215,730@3800f
   --click 310,730@3950f`. A test overwrites the city in `work\docs`;
   `work\Timbuktu_pristine` holds the region as shipped.
   Still open: `CTRL+S` posted as messages doesn't save; the original's frame was at dusk, the lifted
   one's in daylight, at the same game date (Deluxe's day/night cycle).

13. **Fixed: the blue wedge (a bug in the game).** A blue sheet across the city near the
   power plant, `--original` too. It is the water-edge wall the city tile shows where the
   ground is below sea level (x = 0, heights 184..250; drawn by 0x0075B460 under
   0x0075BFD0), drawn with the last car's modelview. The renderer's "load the camera"
   (0x007D2D20) skips the load while its view's flag (+0x138) says the camera is current,
   but several views share the one GL modelview, and another view's object matrices
   (0x007D3400) leave that flag stale. `run_lift.py` `PATCHES` drops the skip, so the camera
   is always loaded. Lifted build only: `--original` still shows the wedge. Found by
   reading back the framebuffer after each draw in one frame to find the draw that
   painted it, then `--gltrace` of `glMatrixMode`/`glLoadMatrixf` (the matrix loaded
   before each draw).

Headless runs (and `--private-docs`) keep the game's Documents folder in `work\docs`, so a
test never writes into the player's `Documents\SimCity 4` (an early run created `sc4.cfg`
there; nothing else was touched).

Host diagnostics: `[host] framework phase N` as startup moves, `[host] C++ throw <type>` for
every guest throw (with a `cParseError`'s message), `--trace-init` for the component loop,
`--native-trace` for every Windows call, `--spy VA` for one function's indirect calls,
`--snap DIR` for frames, `NATIVE32_TRACE_EH=1` for guest exceptions, `--gltrace` (each
distinct texture/state GL call once), `--gltrace-frame N` / `--gltrace-busy DRAWS[@FRAME]`
(every GL call in one frame), `--gltex`, `--gl-hide`/`--gl-only EXT,...` (filter
`GL_EXTENSIONS`; hiding `GL_KTX_buffer_region` drops SC4 to software), `--noshim NAME,...`.
