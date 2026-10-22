# Notes on the game's code

Addresses are guest addresses in `default.xex` (title `58411202`, version 0.0.0.3, image `0x82000000`+`0x2560000`,
code `0x82120000`-`0x82970600`) unless they say otherwise. Every entry says how it was found, so it can be checked
again after a new code generation.

## Modules

| Module | Image | Code | What it is |
|---|---|---|---|
| `default.xex` | `0x82000000`+`0x2560000` | `0x82120000`-`0x82970600`, ~31,000 functions | the game |
| `gd/DLL/xbox 360/Data_DLL.xex` | `0x88000000`+`0x1DE0000` | `0x88010000`+`0x4E4C`, 160 functions | stage data packed as a guest DLL, loaded at run time; 31 MB, almost all data |
| `gd/*.rel` (76 files) | - | - | GameCube REL overlays left on the disc; never loaded (see [plan.md](plan.md#the-game)) |

The DLL is in the manifest as a `[[modules]]` entry; without it the game stops at its entry point
(`Execute(88010308): function not in function table`).

## What the first boots needed (2026-10-05)

| Boot | Stopped at | Cause | Fix |
|---|---|---|---|
| 1 | call to unregistered `0x82968398` | CRT static initializers reached only through the initializer table `0x82980004`-`0x829801C8` | 2 entries in `overrides.toml` |
| 2 | call to unregistered `0x828842F8` | virtual functions reached only through vtables | `tools/pointer_fixpoint.py`: 363 entries, each with the address of its pointer |
| 3 | `Execute(88010308)` | the stage-data DLL was not recompiled | `[[modules]]` in the manifest |
| 4 | read of guest `0x1A4` in `sub_8282CF10` | libjpeg's error path `longjmp`s to its `setjmp`; recompiled as plain calls, longjmp restored `r1`/`r31` from the `jmp_buf` and returned to its caller (`sub_8282CE30` came back with `r1` 0x260 higher and `r31` = 0) | `setjmp_address = 0x82908F10`, `longjmp_address = 0x82908AE0` |
| 5 | runs: title screen, title music, then the attract movie `gd/event/e0401_m.sfd` | | |

Analysis rules learned on the way:

- `tools/pointer_scan.py` targets preceded by `b` are mostly functions after a this-adjustor thunk
  (`subi r3,r3,N; b func`), not switch labels. A candidate whose final `b` lands inside a function body (the
  `__restgprlr_N` epilogue entries at `0x827EFDxx`) is an unwind funclet or a detached chunk, and is left out.
- The JPEG decoder at `0x8281xxxx`-`0x8282xxxx` is libjpeg (error 53 = `JERR_NO_SOI`); it fails on some input during
  boot and recovers through `longjmp`. Which image it is fed is still to be found.

## Crash audit (2026-10-05)

A static pass over every place the translation can stop the game, both modules, then soak runs of the attract loop.

**Jump tables.** `tools/switch_check.py` (with `tools/ppc_mini.py`, a small PPC decoder shared with
`tools/pointer_audit.py`) now follows every `bctr` of the code range back to its table: 298 table
jumps in `default.xex` (247 absolute `lwzx`, 36 byte `lbzx`, 15 halfword `lhzx`, with or without a scale on the
offset). The bound comes from forward value numbering over the straight-line code before the `bctr` (a `mr` copies a
value, every other write makes a new one; `cmplwi` + `bgt` = K + 1 cases, `bge` = K, `clrlwi`/`andi.` masks), over
every way into that code (fall-through, each branch to its first instruction; a dispatch loop jumps back with
`cmplwi` + `ble`), and from the table end for the two tables placed right after their `bctr` (`sub_82141700` and
`sub_82436E48` switch on an argument with no check). Result: all 297 generated switches match the binary case by case
(with the 4 `[[switch_tables]]` below); the 298th, `0x821F1350`, is a state machine's function-pointer table in data
(`0x83762F10`, 18 entries, all registered functions), rightly an indirect call. `0x82876C88` and `0x828781D8` index
function-pointer tables with a stride (12 and 8 bytes); checked by hand, every entry is a registered function. The
stage DLL has no jump table (its 34 `bctr` are its 33 import thunks and one indirect call).

**Why the generator truncated tables** (fixed in `sdk/src/codegen/function_scanner.cpp`). The switch actually emitted
comes from the late detector in `function_graph.cpp` (old `FunctionScanner::detect_jump_table`), which runs at every
`mtctr; bctr` and wins over the table found during discovery. Both detectors took the bound from the first
`cmplwi`/`clrlwi` naming the index register on the way back from the `bctr`, without noticing that the register had
been reloaded in between: `sub_824D6468` switches on `r11` after `mr r11,r10; cmplwi cr6,r10,67; bgt`, and the walk
went on to a `clrlwi r11,r11,29` of the code before (8 cases of 68). Both now use one forward value-numbering helper
(`straightLineSwitchBound`; a branch forward inside the run is a join, not a bound; after a loop the analysis starts
over). Checked by a scratch codegen without any `[[switch_tables]]`: all 297 switches come out right; with the
manifest as it is, the only change in the generated code is that `sub_82560178` loses a bogus 20th case. The late
detector's offset-table reader also skipped invalid entries, which would have moved every later case down one; it now
keeps the position (a null target).

**Functions reachable only through pointers.** `tools/pointer_fixpoint.py` rejected 19 candidates as "extends beyond
its gap" and 2 as fragments; 21 of them are real entries (added to `overrides.toml`, fixpoint reached afterwards):

- 18 vtable entries (the 6 words around each pointer are registered functions; COM-style methods: null check, return
  `E_POINTER` `0x80004003`, else continue) whose continuation after an early `blr` the generator had made a function
  of its own; the entry itself was never registered. A virtual call to any of them stops the game with "Call to
  invalid or unregistered function". The tool now accepts this shape: the listing past the gap is only registered
  functions the code in the gap branches to. Once the entries are declared, the generator no longer makes their 26
  tails functions of their own (`0x8256F160`-`0x8256F198`, `0x8258E72C`, `0x825A5E2C`, ...); none of those addresses is
  referenced from outside its new function (no data word in either module, no `lis`/`addi`, no branch from elsewhere),
  so nothing can call them directly.
- `0x826C8ED8`, a callback built with `lis`/`addi` at `0x826C8FC8` (same shape).
- `0x82851BD0` and `0x82851BE0`, VMX routines that `sub_82852C88` returns as function pointers (with registered
  siblings `0x82851BC0`, `0x82851C58`). They were "fragments" only because `lvx128 v1,r0,r11` was read as a read of
  `r0` (`r0` in the base slot is the constant 0); `gap_classify.reads_undefined_at_entry` fixed, together with the
  `bdzf 4*cr6+eq` form that hid its CR field.

Left out, correctly: 15 "no code" (XEX import thunks, registered as `__imp__*`, and a zero word), `0x8214381C`
(a catch funclet's continuation), `0x8215F0E0` (an inline jump table's data), 4 unwind funclets (`b __restgprlr_N`).

Also fixed in `pointer_fixpoint.py`: it appended its entries at the end of `overrides.toml`, which since the
`[[switch_tables]]` were added made them fields of the last switch table; the generator ignored them without a word.
They now go at the end of `[functions]`, and the generator warns about unknown keys in a `[[switch_tables]]` entry
(`config.cpp`).

**Other code pointers** (`tools/pointer_audit.py`, both modules and across them): no other pointer in data or built
in code lands on an unregistered address that is called. What is left are C++ EH IP-to-state entries
(`0x820EExxx`-`0x820F1xxx`), SEH scope records (next to the `mr r8,r8` markers around `__try` blocks), catch/unwind
continuations, jump tables and data that looks like code. The stage DLL's data holds no pointer into `default.xex`'s
code (the ~100 hits are stage data that happens to look like addresses) and the DLL's own code is all registered
except an unreferenced copy routine at `0x880143C8` and three SEH handler blocks. Image dumps for this: codegen with
`REXGLUE_DUMP_IMAGE=out/default_image.bin` now also writes `out/default_image.Data_DLL.bin`.

**Other traps in the generated code.** No `REX_FATAL`, no "jump to null value", no unimplemented instruction. 297
`__builtin_trap` = the switch defaults above. 1,219 `ppc_trap` in `default.xex`: 1,207 conditional (`twllei`/`tdllei
rX,0` and `twlgei`/`tdlgei rX,-1`, MSVC's checks for a zero divisor and for INT_MIN / -1 before a division; the
generated division is guarded and gives 0), 11 `twi 31,r0,22` (`__debugbreak`) and 1 `twi 31,r0,20` (debug print),
all in library code; the DLL has 1 conditional and 1 debug print. `ppc_trap` only logs ("tw/td trap hit"), so none of
them stops the game. Only one `setjmp`/`longjmp` pair exists (the two registered). `sub_82801DEC` restores a full
CONTEXT and returns to its `r3`; it is part of the SEH unwinder and only runs when an exception is raised. Every `bctr`
of `default.xex` is accounted for: 297 switches, 739 indirect calls in generated functions, 282 in import thunks, and
one in `0x82964470`, an unregistered 9-instruction wrapper that nothing references (no branch, no data word, no
`lis`/`addi`, nothing in the DLL).

**Known risk: exceptions.** The game throws C++ exceptions in 105 places (`_CxxThrowException` = `sub_827F2E48`;
e.g. `operator new` `sub_827EE720` throws `bad_alloc`) and raises through `sub_82808410` (`RaiseException`). The SDK's
`RtlRaiseException` handles only SetThreadName and otherwise calls `rex::debug::Break()` (`__debugbreak` on Windows):
any throw ends the game. None was seen in the soak runs.

**Kernel stubs the game imports** (`REX_EXPORT_STUB`: logs "<name> STUB", leaves `r3` as it was): `XamGetLanguage`,
`XGetAudioFlags`, `HalGetCurrentAVPack`, `XeKeysGetConsoleID`, `NtWriteFileGather`, `XamShowMarketplaceUI`,
`XamShowAchievementsUI`, `XamShowGamerCardUIForXUID`, `XamUserCreateStatsEnumerator`, `XamContentResolveInternal`,
`XamIsOptionalMediaUpdateInstalled`, `ObReferenceObject`, `ObIsTitleObject`, `RtlUpcaseUnicodeChar`, the
`NetDll_XHttp*` family, the `Vd*` HDCP/closed-caption calls, `XamBackgroundDownload*` and a few more. None was called
in the attract loop; the menus (language, achievements, marketplace for the Battle DLC, saving) are where to watch for
them.

**Soak runs** (attract loop, muted and minimized, logs checked for crashes, unregistered calls, "Switch case out of
range", traps and stubs): 11 min with the DLC before the fixes; after them 12 min with the DLC (the 21 functions),
then with the final generator 12 min without the DLC and 20 min with it. The no-DLC run used `REX_USER_DATA_ROOT`
pointing at an empty folder and `REX_SA2_DLC_DIR` at a missing one, because the DLC is installed in the shared content
root under Documents and renaming `dlc/` does not remove it. Stage files seen in the logs (only slow or failed opens
are logged): 13, 43, 46, 47, 48, 50, 53, 54. All runs clean. The attract loop never opens the menus, so the stubs
above and the throw sites stay untested. The log files fill with the GPU's "Texture fetch constant ... invalid type"
warning (about 1 MB a minute); with `log_max_files` = 20 a run keeps about 100 MB.

## Direct3D (Xbox 360 XDK, statically linked), for the native renderer

The GPU's MMIO registers are at `0x7FC80000` (register index * 4). Functions that load that base
(`lis rX, 0x7FC8`), found by searching the generated code for `2143813632`, and their measured call rates
(`sa2_d3d_trace = true`, title screen, emulated GPU):

| Function | Registers written | Calls | What it is |
|---|---|---|---|
| `sub_8254DC00` | `0x714` = `CP_RB_WPTR` (0x1C5) | ~220/s, all from `0x8254E7FC` | ring kick-off: publishes the PM4 write pointer |
| `sub_8254D340` | `0x714` = `CP_RB_WPTR` | 0 at the title | ring kick-off (second path) |
| `sub_8254D420` | `0x1628`, `0x6544` | ~120/s, from the host | the GPU interrupt callback (`SetInterruptCallback(8254D420, ...)`): vblank and swap interrupts |
| `sub_82557FC0` | `0x6110` = `D1GRPH_PRIMARY_SURFACE_ADDRESS` (0x1844) | 60/s, from `0x8254D4A8` (inside the interrupt callback) | flip: scan-out surface, on vblank |
| `sub_825580F0` | `0x4`, `0x5FA0`, `0x5FA4`, `0x6110` | ~55-60/s, from `0x8254D450` (inside the interrupt callback) | display / flip setup |
| `sub_8255AE60` | `0x112C` | 0 at the title | |
| `sub_82568B98` | `0x112C`, `0xE5C` | 0 at the title | |
| `sub_8255C0F0` | `0x268`, `0x3214`, `0x3408` | 0 at the title | |
| `sub_825666F0` | `0x0`, `0x6034` | 0 at the title | |

So the game presents a steady 60 frames per second and kicks the ring about 3.7 times per frame. The D3D library sits
around `0x8254xxxx`-`0x8256xxxx`.

### Named D3D functions (2026-10-05)

Found by `tools/d3d_match.py` against Banjo-Kazooie: Nuts & Bolts (2008 XDK, functions named in banjo-nx's
`app/config/renut_gpu_funcs.toml`): same instruction/register shape, immediates ignored. Score 1.00 = identical shape.

| Function | SA2 | Score | Confirmed by |
|---|---|---|---|
| `D3DDevice_Swap` | `sub_82558E98` | 0.57 | the only caller of the kernel's `VdSwap` |
| `D3DDevice_DrawVertices` | `sub_82565C60` | 0.91 | its 20 calls line up one to one with Banjo's |
| `D3DDevice_DrawIndexedVertices` | `sub_82566078` | 0.90 | |
| `D3DDevice_DrawVerticesUP` | `sub_82565C18` | 0.89 | |
| draw-time shader load (FlushState part) | `sub_82556A60` | - | 4th call of `DrawVertices` at the same instruction as Banjo's `sub_8222BF10` |
| `D3DDevice_CreateVertexShader` | `sub_8255E750` | 1.00 | |
| `D3DDevice_CreatePixelShader` | `sub_8255E568` | 0.59 | called in pairs with CreateVertexShader from the same 5 engine functions (`sub_82577DA0`, `sub_8275C6B8`, `sub_8275F840`, `sub_827BC888`, `sub_82856B38`), plus 3 wrapper pairs (`sub_82759E90`/`E48`, `sub_82609D50`/`DD0`, `sub_8295DDB8`/`DD30`): SA2 creates its shaders through these calls, so the creation hooks see them all |
| `D3DDevice_CreateVertexDeclaration` | `sub_8255E198` | 0.81 | |
| `D3DDevice_Clear` | `sub_82563AB0` | 0.96 | |
| `D3DDevice_Resolve` | `sub_825645A0` (to confirm) | 0.51 | |
| `D3DDevice_BeginTiling` / `EndTiling` | `sub_825576D0` / `sub_82557A40` (to confirm) | 0.59 / 0.74 | |
| `D3DDevice_CreateTexture` / `CreateSurface` | `sub_82548DF8` / `sub_82548F18` | 1.00 / 0.75 | |
| `D3DDevice_CreateVertexBuffer` / `CreateIndexBuffer` | `sub_82549730` / `sub_82549808` | 1.00 | |
| `D3DDevice_SetGammaRamp` | `sub_825527D8` or `sub_82552708` (identical shape) | 1.00 | |
| `D3DResource_AddRef` / `Release` / `GetType` | `sub_82549650` / `sub_8254A4A8` / `sub_82549358` | 0.94-1.00 | |

Device fields read by the draw-time shader load (`sub_82556A60`, device in r3, then r31): bound **vertex shader at
`+13072` (0x3310)** and **pixel shader at `+13068` (0x330C)**; the 2008 XDK had them at +12696 / +12692 (the device
grew by 376 bytes before them). Both functions then read the vertex shader's +896, +872 and +904 at the same offsets,
so the shader object layout itself did not change.

### How SA2 clears the screen, and the native renderer's ghosting (2026-10-05)

`D3DDevice_Clear` does not clear the 1x 1280x720 back buffer directly: it binds a **4x MSAA alias of the same EDRAM
tiles** (pitch 640, 640x360 pixels of 4 samples, base 0 or 0x2D0 for the two back buffers) and draws a rect list with
Direct3D's own shaders (VS n11, PS n20 = `ps_ba62fb45`, `oC0 = r0`: the clear colour arrives as interpolator 0), then
switches back to the 1x view (frame trace: `banjo_trace_frame_s` + `SA2_TRACE_ALL_DRAWS=1`). Two native renderer
bugs made every boot screen stay behind the next ones as a ghost:

1. The 4x -> 1x colour transfer did not exist (`ConvertAliasEDRAM` handled format conversions only), so it was skipped
   as unsupported and the back buffer was never cleared. RGBA8 <-> RGBA8 between different sample layouts now goes
   through the fragment conversion pass (`ConvertAliasColorFragEDRAM4`, which maps physical samples).
2. `ps_ba62fb45`'s container is rebuilt from a template (`SA2_SHADER_MISSING`) and carries the template's empty
   interpolator list, so XenosRecomp loaded r0 with the pixel position: the clears wrote (x, y, 0, 0) (green). A pixel
   shader that declares no interpolator now reads all 16 by register, as the hardware does.

Result: the darkest boot frame between screens went from 5.42 to 0.00 (`tools/win/cvar_sweep.ps1`), and the 40 s boot
sequence matches the emulated GPU at mean MAD 0.20 per frame (`tools/compare_raw.py`; the rest is movie timing).

### Comparing renderers frame by frame: `vblank_per_swap`

SA2's logic advances per vblank, so two runs drift apart (loading time, host speed) and gameplay frames cannot be
paired. With `vblank_per_swap = true` (SDK, `xboxkrnl_video.cpp`) both vblank timers (the emulated GPU's and the native
renderer's) give one vblank per `VdSwap`, at most 60 a second (`vblank_per_swap_min_us`), with a 100 ms fallback for
loading. The attract demos then play the same frames in both runs (same rings and poses), shifted only by the number
of loading-screen frames: `tools/compare_raw.py --follow` pairs them. The intro movie and the title screen background
stay wall-clock driven. Recorder: `-VideoEvery N` (sa2_video_every) keeps one frame in N.
