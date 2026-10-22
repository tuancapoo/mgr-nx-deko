# Plan

The order is the one that worked for nfsmw-nx and masseffect-nx (their `docs/porting-another-game.md`): every problem
of the translation is far easier to find on a PC than on the console, and ReXGlue's emulated GPU is the right way to see
the first frames, but far too slow on the Switch.

## The game

| | |
|---|---|
| Package | XBLA LIVE (STFS, read-only), content type `0x000D0000`, title `58411202` |
| `default.xex` | 10,166,272 bytes, XEX2, AES + LZX, version 0.0.0.3 |
| Image | base `0x82000000`, size `0x02560000` (39,190,528 bytes), entry `0x82543108` |
| Data | `gd/` (1822 files: `.prs` PRS-compressed Ninja data, `.rel` overlays, `ADX/` music, `.sfd` movies), `Shader/xbox360/` |
| DLC | "Battle" (content type `0x00000002`): `gd/e0609_m.sfd` and its achievements; mounted later as a content package |

SA2 on the 360 is the Dreamcast/GameCube engine (Ninja + Chao Garden) ported with its own Direct3D 9 layer.

**The `gd/*.rel` files are GameCube leftovers, not 360 code** (checked with `tools/rel_info.py` and the decrypted
image, 2026-10-05). They are GameCube REL modules (76 files, 21 MB of code) whose code uses CodeWarrior's prologue
(`mflr r0`: 51,794 times, `mflr r12`: never) and whose imports from the main module point into the GameCube DOL's
address range (`0x8000532C`-`0x803E0544`). `default.xex` uses the Xenon MSVC prologue (`mflr r12` + `__savegprlr`:
22,934 times) and holds the stage code itself; it names no REL file except `ChaoMotionsD.rel` (to watch at runtime:
if the game ever calls into it, the log shows a call to an unregistered function). So recompiling `default.xex` alone
covers the game.

## Stage 1: recompile and boot on Windows

1. Extract the package (`tools/stfs_extract.py`, verified against the STFS SHA-1 hash tables).
2. Build `rexglue` (host code generator) from `sdk/` with clang 20.
3. `rexglue codegen app/sa2_manifest.toml` -> `app/generated/default/`.
4. Build `app/` for Windows with the SDK's Xenos plugin (D3D12 on Windows, Vulkan optional).
5. Boot loop: fix what the translation misses (functions, jump tables, function chunks) with `tools/find_gaps.py`,
   `tools/gap_fixpoint.py`, `tools/pointer_scan.py` (with `SA2_DUMP_IMAGE`) and `tools/runtime_gap_resolver.py`;
   each entry goes to `app/overrides.toml` with its evidence.
6. Kernel exports the game needs that the SDK lacks (XAM: achievements, avatar awards, marketplace) are stubbed in the
   app, not in the SDK.
7. Goal: title screen, menus, one stage of each type (speed, treasure hunt, shooting), Chao Garden, a 2P battle.

## Stage 2: native renderer on PC

Same design as nfsmw-nx (`app/src/nfsmw_nativo_*`) and masseffect-nx (`app/src/native/`):

- The game keeps its own Direct3D, which writes the PM4 command ring as always.
- The ring thread reads the ring, tracks the GPU registers, records each draw with Vulkan.
- Render targets are Vulkan images; resolves avoid copies where the next operation clears anyway.
- Texture cache with untiling, keyed on a fingerprint of the guest bytes.
- Shaders: Xbox 360 microcode -> XenosRecomp -> HLSL -> DXC -> SPIR-V, looked up by microcode hash.
- Hooks on the shader-creation functions identify each shader; `WAIT_REG_MEM`/`SCRATCH_REG` writes keep the game in step.
- Start with a tracing phase: wrap the game's D3D functions and log every call to confirm each address.

Done on the PC first (Vulkan backend of the SDK), with a runtime switch back to the emulated GPU for comparison.

## Stage 3: Switch

- Toolchain: devkitA64 (GCC 16) through `tools/switch/cmake/switch-devkitA64.cmake`, libnx, the SDK's Horizon layer.
- Driver: mesa-switch NVK with `mesa/` patches, linked into the NRO.
- Platform traps (see the reference ports' `platform-notes.md`): memory-mapping limit (`2001-0103`), one libnx exception
  stack, no `std::thread::detach`, only priority `0x3B` is time-sliced, three cores, never read `cntvct_el0`.
- Codegen speed options (`perf_overrides.toml`: registers as locals, `share_registers` pieces checked with
  `tools/read_before_write.py`), direct calls (`tools/direct_calls.py`), LTO, PGO, function ordering.

## Stage 4: shaders, measuring, speed

Shader library built from the user's own files, measured on the console at stock clocks, one change at a time.
