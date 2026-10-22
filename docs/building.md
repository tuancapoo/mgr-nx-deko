# Building sa2-nx

How to build the Windows PC build (used for development and testing), the Nintendo Switch NRO, the shader package and
the SD card folder, from your own copy of the game.

## What you need

- **Your own XBLA copy of Sonic Adventure 2:** the package
  `Content/0000000000000000/58411202/000D0000/525E7BB1E72A2DB99AB79281BA24938048198FAD58` from an Xbox 360. The
  Battle DLC (`58411202/00000002/SonicAdventure2Battle`) is optional.
- **Windows 10/11** with:
  - VS 2022 Build Tools (the MSVC libraries and the Windows SDK; CMake and Ninja come with it);
  - LLVM clang 20 (a portable copy is fine; by default `..\_tools\llvm`);
  - Python 3 and Git.

  `tools/win/env.ps1` finds them; override the paths with `SA2_LLVM`, `SA2_VSDIR` and `SA2_CMAKE_DIR`.
- **For the Switch:**
  - [devkitPro](https://devkitpro.org) with devkitA64 and libnx;
  - the NVK Vulkan driver built from [mesa-switch](https://github.com/danfromtico/mesa-switch), as a static
    `libvulkan.a`. SA2 links nfsmw-nx's build; set `NVK_SDK` to another one;
  - glslangValidator (`tools/win/build_glslang.ps1` builds it into `out/glslang`).

Run every build command from PowerShell, through `tools\win\run.ps1` or the scripts below. Running `cmake` from a
Bash shell picks up MSYS2's tools: the configure fails and wipes the build folder's cache.

## 1. Extract the game

```powershell
# Your package (it is only read, never written; ~2.4 GB)
python tools\stfs_extract.py "<...>\58411202\000D0000\525E7BB1E72A2DB99AB79281BA24938048198FAD58" assets\game_root
# Optional: the Battle DLC, installed by the game from dlc\ at start
mkdir out\pc\dlc; copy "<...>\58411202\00000002\SonicAdventure2Battle" out\pc\dlc\
```

## 2. Translate the game's code to C++

```powershell
python tools\fetch_thirdparty.py                       # third-party sources of the SDK
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"
powershell -File tools\codegen.ps1                     # default.xex and the stage DLL -> app/generated (git-ignored)
```

`tools/codegen.ps1` also runs the post-codegen steps of the optimised flavour (`tools/post_codegen.py`: the
no-volatile precompiled header, direct calls, the localized native copies).

## 3. The PC build

```powershell
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app"
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out/pc"
powershell -File tools\win\boot_test.ps1               # 45 s, muted and minimized, then the end of the log
```

The PC build runs the code generator again by itself when one of its inputs changed (a toml, the xex, the generator).
That build then stops once with "`sa2_pch.h` has been modified since the precompiled header ... was built" (the
precompiled header was made before the regeneration finished): run the build command again.

`out\pc\sa2.toml` holds the PC's settings. It starts **muted and minimized** (`audio_mute`, `window_start_minimized`)
so test runs stay out of the way; set both to `false` to play. `license_mask = 1` runs the full game rather than the
XBLA trial, and `sa2_renderer_native = true` (or the environment variable `REX_SA2_RENDERER_NATIVE=true`) uses the
native Vulkan renderer instead of the emulated GPU.

To record a video while it runs minimized, give `-VideoAt <seconds after start>` and optionally `-VideoFrames` (60
per second). The MP4 and a contact sheet land in `out\pc\captures\` (needs ffmpeg at `C:\ffmpeg\ffmpeg.exe`, or set
`FFMPEG`):

```powershell
powershell -File tools\win\boot_test.ps1 -VideoAt 8 -VideoFrames 600
```

## 4. The shader package

The native renderer draws with the game's shaders translated ahead of time (XenosRecomp to HLSL, DXC to SPIR-V) into
`sa2_shaders.mesp` + `sa2_shaders.mesp.idx`.

1. Run the PC build once with `sa2_dump_shaders = true`: every container the game creates is written to `shaders/`
   next to `sa2.exe`.
2. Build the package from those dumps:

   ```bash
   tools/build_shader_library.sh out/pc/shaders
   ```

Direct3D's own shaders and the variants Direct3D rewrites at run time are found with the discovery dumps
(`SA2_SHADER_DISCOVERY`, `SA2_VERTEX_VARIANTS`, `SA2_SHADER_MISSING`) and `tools/wrap_discovered.py`.
[porting-guide.md](porting-guide.md), chapter 6, explains the whole loop.

## 5. The Switch NRO

```powershell
powershell -File tools\win\build_switch.ps1            # out\nx\sa2.nro (LTO: allow 15-30 minutes)
python tools\check_switch_build.py                     # start-up code at 0, flavour, driver, no vector FMA, ...
powershell -File tools\win\package_switch.ps1          # the SD card folder: out\nx\sdcard\switch\sa2-nx
powershell -File tools\win\package_switch.ps1 -IncludeGame   # the same with the game as game_root.nxpack
```

`build_switch.ps1` configures `out\nx` the first time (`-Reconfigure` does it again). The NRO's name, author, version
and icon (`app/icon.jpg`) are in `app/CMakeLists.txt`. The settings compiled into the NRO are
`app/sa2_switch_defaults.toml`, and the player's `sa2.toml` is `app/sa2_switch.toml`.

The Switch build's own switches for the CPU round of 2026-10-12, all ON (`-DSA2_SWITCH_...=OFF` at the configure puts
the old form back; `docs/cpu-rounds.md` has what each does): `SA2_SWITCH_OMIT_FRAME_POINTER`,
`SA2_SWITCH_FLUSH_FLOW`, `SA2_SWITCH_GUEST_CAS`, `SA2_SWITCH_VEC_BYTESWAP`, `SA2_SWITCH_NEON_SHUFFLES`,
`SA2_SWITCH_NEON_VMX_HELPERS`. Version 1.0.1 is built with them.

The second round's switches work on every platform (a PC build can take one step out to find where a difference
comes from). ON: `SA2_GEN_RO_CONSTANTS`, `SA2_GEN_FLOAT_LOADS`, `SA2_GEN_LEAF_LOCALS`, `SA2_GEN_LOOP_LOCALS` (third
round), `SA2_GEN_COLD_TRAPS`, `SA2_GEN_PROMOTED_CALLS`, `SA2_GAME_NO_MATH_ERRNO`. OFF, because they are not exact: `SA2_GEN_NV_SAVES_ELIDE` (it
tinted SA2's characters), `SA2_GEN_LR_MOVES_ELIDE`, `SA2_GEN_BACK_CHAIN_ELIDE`, `SA2_GAME_NO_TRAPPING_MATH`.
The post-codegen steps need the generator's image dump `out\default_image.bin`; `tools\codegen.ps1` writes it.

### Profile-guided optimisation

`pgo/` holds the profile the released NRO was built with (one console session of version 1.0.0). To build with it:

```powershell
powershell -File tools\win\build_switch.ps1 -Pgo use
```

`-Pgo none` goes back to a build without PGO. The profile matches the code of this repository: a function you change
loses its part of it, and the rest still applies. [pgo/README.md](../pgo/README.md) explains its file names and what
applies when you build from another folder.

To record a new profile:

1. `build_switch.ps1 -Pgo generate` builds an instrumented NRO. Play it on the console: it writes
   `pgo/sa2_pgo_<N>.stream` next to the NRO (one file per session, saved every 2 minutes and on the HOME button).
2. Move the `.gcda` files of `pgo\` aside, copy the console's `pgo` folder to the PC, and run
   `powershell -File tools\win\pgo_merge.ps1 <folder>`. It merges every session into `pgo\` and fills in each file's
   summary. Merge on the PC that built the instrumented NRO: the streams name its profile folder.
3. `build_switch.ps1 -Pgo use` builds the optimised NRO.

Do not change the game or app code between steps 1 and 3: a changed function loses its profile. What the link
decides can still change, because GCC adds the counters before its interprocedural inliner runs: a build that uses a
profile gives the game code `--param=max-inline-insns-auto=64` and `--param=inline-unit-growth=12`
(`SA2_SWITCH_INLINE_AUTO`, `SA2_SWITCH_INLINE_UNIT_GROWTH`; empty for the compiler's 30 and 40).
`docs/cpu-rounds.md`, "Link-time optimisation of a recompiled program", has the measurements.

## Tests on the PC

| Script | What it does |
|---|---|
| `tools\win\boot_test.ps1` | One muted, minimized run; `-VideoAt` / `-VideoFrames` record it |
| `tools\make_enhancement_configs.py` | The A/B configurations (`out\nx\configs`) |
| `tools\compare_video.py`, `tools\compare_raw.py` | Line up two recordings frame by frame |
| `shaders\tests\test_vertex_fetch_reorder.cpp` | The run-time proof for reordered vertex fetches (`--orders` for every order) |
| `tools\check_switch_build.py` | The checks every Switch build must pass |
| `tools\win\run_sdk_tests.ps1` | Builds and runs the SDK's unit tests (`sdk\tests`): the any-mode float helpers, rotates, the signal protocol, the vector forms |
| `tools\arm_forms_check.py` | What the Switch compiler makes of the AArch64-only forms (lock words, NEON helpers), with the claims checked. No NRO is built |
| `tools\switch_compile_check.py <file> ...` | Compiles chosen files with the Switch compiler and the Switch build's flags into a scratch folder: code only the Switch builds, checked on the PC in a minute |
| `tools\one_file_ab.py <N> name=flags ...` | One recompiled file in several variants with the Switch compiler; instructions, loads and stores counted |
| `tools\pgo_fit.py <N> ...` | How many functions of a recompiled file still match the committed profile |
| `tools\scratch_ab.py copy / count N... / save NAME / compare NAME` | A scratch copy of the generated code (`out\gen_scratch`) to develop a post-codegen step on and measure it with the Switch compiler. `tools\post_codegen.py --gen out\gen_scratch` runs the whole sequence on it |
| `tools\indirect_promotions.py --census <file> --gen app\generated\default` | Makes `app\indirect_promotions.txt` from a PC run with `sa2_diag_indirect_census = "<file>"`: the indirect call sites that go to one target |
| `tools\loop_body_census.py --compare before.o after.o` | Instructions, loads and stores inside loops of two AArch64 objects: for a step that changes loops, where a file's size says nothing |
| `tools\switch_compile_check.py --like <known source> <new source>` | A source the configured Switch tree does not list yet, compiled with a sibling's flags |
| `tools\lto_census.py <elf>` | What the LTO link left in a Switch ELF: the calls between game functions that were not inlined (by the callee's size), functions with one call site left, calls out of the LTO unit, the clones and cold parts the optimiser made |
| `tools\lto_inline_report.py [--save NAME] [--against NAME]` | After a Switch link configured with `-DSA2_SWITCH_LTO_DUMP=ON`: the calls the inliner took and refused (by reason), counted as often as they ran in the profile's session, GCC's profile-weighted time estimate, the size of the hot functions, and against a saved report the copies a setting added |
| `tools\nro_set_version.py <in.nro> <out.nro> <version>` | Sets the version the homebrew menu shows (the NACP's display version) in a copy of an NRO, so a release is byte for byte the build that was tested. 1.0.1 was made this way from the tested build |
| `tools\address_taken_census.py` | An estimate of the game functions nobody takes the address of, and how many of them are called from one place (the code LTO cannot treat as it would in an ordinary program) |
| `tools\crt_signatures.py make / scan` | Signatures of the C runtime functions one port found (`tools\crt_signatures.json`), looked for in any game's image dump |
| `tools\census_summary.py <log>`, `tools\census_sites.py <log>` | A run with `sa2_diag_runtime_census = true`: float-mode switches and runtime calls per Swap, and the functions the switches come from |

Lockstep captures (`REX_VBLANK_PER_SWAP=true`, `REX_SA2_VIDEO_FRAME=6000`) pin every frame to a presented swap, and
`<name>.hash` keeps the XXH3 of each frame at full resolution. Two runs are equal when their hashes are, frame for
frame (align the labels by content first: each run numbers its swaps from its own start).
