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
4. **Now:** `cGZFramework` startup (0x0087AF13) passes phases 1 and 3, then the app init at
   phase 4 (vtable 0x00AC3090 slot +0xA8 = 0x00879FBC) fails: it walks the registered
   components by priority and one refuses (exit at 0x0087A03A). `WinMain` returns -1.
   Next: name that component (hook 0x0087A03A, print `edi` and its vtable).

The host prints each framework phase as it changes (`[host] framework phase N`).
