# Porting an Xbox 360 game to the Nintendo Switch, from scratch

This guide follows the road Sonic Adventure 2 took: from a `default.xex` to a native Switch build that runs at 60 FPS
at the console's stock clocks. It is written for someone starting another game with this toolchain. It covers the
order of work, the tools of this repository, the traps that cost days, and every optimisation the port uses, in the
order they pay off.

The method comes from [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) (the Switch layer, the NVK driver, PGO),
[masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) (the native renderer), and the sibling ports
(Banjo-Kazooie: Nuts & Bolts, Sonic & All-Stars Racing Transformed, Mushihimesama Futari, Peggle 2). Each of them
found some of what is below.

## Contents

0. [The big picture](#0-the-big-picture)
1. [Tools](#1-tools)
2. [The game's files](#2-the-games-files)
3. [Translating the code](#3-translating-the-code)
4. [The first boot, on the PC](#4-the-first-boot-on-the-pc)
5. [The native renderer](#5-the-native-renderer)
6. [Shaders](#6-shaders)
7. [Moving to the Switch](#7-moving-to-the-switch)
8. [Measuring on the console](#8-measuring-on-the-console)
9. [Making the CPU fast](#9-making-the-cpu-fast)
10. [Replacing Direct3D](#10-replacing-direct3d)
11. [Making the GPU fast](#11-making-the-gpu-fast)
12. [Proving every change](#12-proving-every-change)
13. [Shipping](#13-shipping)
14. [Traps](#14-traps)
15. [Glossary](#15-glossary)

---

## 0. The big picture

An Xbox 360 game is a PowerPC program (`default.xex`, sometimes with extra modules) that talks to three things: the
Xbox 360 kernel and system libraries (files, threads, memory, controllers, audio, saves), its own copy of Microsoft's
Direct3D (compiled into the game), and through Direct3D the Xenos GPU (commands written into a ring buffer in memory).

A native port replaces each of them:

```text
default.xex (PowerPC)  --rexglue codegen-->  C++ (app/generated)  --clang / GCC-->  sa2.exe / sa2.nro
kernel, XAM            -->  ReXGlue SDK: the system as C++ (sdk/), with a Horizon layer for the Switch
Direct3D + Xenos GPU   -->  the native renderer: the game's GPU work drawn with Vulkan (app/src/me)
GPU programs (shaders) -->  translated ahead of time: XenosRecomp -> HLSL -> DXC -> SPIR-V (the shader package)
Vulkan on the Switch   -->  NVK (Mesa), linked statically into the NRO
```

Nothing is interpreted or emulated at run time except what you have not replaced yet. The CPU code is translated
once, ahead of time; the GPU is the part that starts emulated and is replaced step by step.

**Work on the PC first.** A Windows build of the same code has a debugger, fast rebuilds, and, at the start, the
SDK's emulated GPU as a reference to compare against. Everything is first made to work and checked on the PC, then
measured and tuned on the console.

**Rules that keep a port correct** (each one was learned the hard way):

- **No game data in the repository.** Game files, generated code and anything made from them (the shader package)
  stay out. The player makes them from their own copy.
- **One change at a time, each behind its own setting.** Every optimisation has a key in the configuration, so it
  can be switched off when something looks wrong, and A/B-tested on the console.
- **Every change proves itself.** Native code is checked against the original while the game runs. Renderer changes
  are tested pixel by pixel against the build before them (section 12).
- **The console decides.** The PC is only the test bench: its timings say little about a 1020 MHz Cortex-A57 with 2 MB
  of L2. An optimisation is kept when the console's profile shows it.
- **ARM first.** Write for the A57: count instructions, cache lines and system calls, not PC milliseconds.

## 1. Tools

- **Windows 10/11**, VS 2022 Build Tools (MSVC libraries and Windows SDK; CMake and Ninja come with them), **LLVM clang
  20** (ReXGlue's code generator and the PC build need clang), Python 3, Git.
- **[devkitPro](https://devkitpro.org)**: devkitA64 (GCC for the Switch) and libnx.
- **NVK** built from [mesa-switch](https://github.com/danfromtico/mesa-switch) as a static `libvulkan.a`, with a patch
  for draw costs, ZCULL and shader compiler fixes. nfsmw-nx's build works for any port.
- **This repository's SDK fork** (`sdk/`): ReXGlue 0.10 with the Horizon (Switch) layer, the code generator changes and
  the fixes of every port. Start a new port from it rather than from upstream ReXGlue.
- **ffmpeg** (videos of test runs), **DXC** (the shader compiler; any recent build with SPIR-V output).
- **An Xbox 360** to copy your own game, its title update and DLC from.

`tools/win/env.ps1` sets up the PC build environment. `tools/win/run.ps1` runs one command inside it and keeps the
whole output in a log.

## 2. The game's files

- **Disc games:** `tools/extract_iso.py <iso>` extracts the disc.
- **Xbox Live Arcade games and DLC:** these are STFS packages (`LIVE`/`PIRS`) from the console's
  `Content/0000000000000000/<title id>/` folder. Extract them with `tools/stfs_extract.py <package> assets/game_root`;
  it only reads the package, never writes to it. (The installer page reads the same packages in the browser without
  extracting them: `lib/stfs.js` of sa2-nx-installer.)
- **Look for more code.** Besides `default.xex`, games load extra modules: guest DLLs (`.xex` files in the data,
  loaded with `XexLoadImage`) go into the code generator's manifest as `[[modules]]` (SA2 has one, its stage data).
  Files left over from older consoles (GameCube `.rel` overlays...) may be dead: check their prologue style (`mflr r12`
  is the Xbox 360 compiler; `mflr r0` is CodeWarrior) before assuming they run.
- **Find the Direct3D version.** Every game links its own copy of the XDK's Direct3D. Its version decides the device
  structure's layout (section 10), so note it early (the XEX's import libraries list the XDK build).

## 3. Translating the code

### 3.1 The first translation

1. Build the code generator: `cmake -S sdk -B out/host` then `cmake --build out/host --target rexglue`.
2. Write the manifest (`app/sa2_manifest.toml` is SA2's): the XEX, the modules, and per-function fixes.
3. `rexglue codegen <manifest>` (`tools/codegen.ps1` runs it with this repository's post-steps). It writes C++ to
   `app/generated`: one function per guest function, the guest's registers in a context struct, memory through the
   guest's 4 GB address space.

### 3.2 Code the generator misses

A translator finds functions by following calls. What it cannot see, it does not translate, and the game crashes
there later. Sweep these classes before the first boot, and again after every translation:

| Class | Symptom | Tool |
|---|---|---|
| Functions reached only through pointers (CRT initializer tables, vtables, callbacks built with `lis`/`addi`) | "unregistered call" / call to an unknown address | `tools/pointer_fixpoint.py`, `tools/pointer_audit.py` |
| Truncated jump tables (`switch` statements) | Illegal instruction, "switch case out of range" | `tools/switch_check.py` |
| Gaps: code between functions that nothing reaches statically | A crash in code that exists in the XEX but not in the C++ | `tools/find_gaps.py`, `tools/gap_fixpoint.py`, `tools/runtime_gap_resolver.py` |
| `setjmp` / `longjmp` | r1 or r31 corrupted after a call returns | Find them by shape (stores of r1, r31 and the non-volatile registers into r3's buffer, and the matching loads); set `setjmp_address` / `longjmp_address` |
| Functions that never return (`abort`, the C++ throw) | Bogus fall-through, a catch funclet merged into its parent | `tools/read_before_write.py --noreturn=<address>` (the audit then treats the call as a path end) |

Every fix in the manifest carries its evidence (the address, what calls it, why). Without it, the next person cannot
tell a real fix from a guess.

### 3.3 The code generator's options

The optimisations that change the generated code are in section 9.1. Start without them: first make the game boot
and run correctly, then turn them on one at a time.

## 4. The first boot, on the PC

1. Build the PC version (`cmake --preset win-pc -S app`, then `cmake --build out/pc`) and run it with the SDK's
   emulated GPU.
2. **Test harness:** `tools/win/boot_test.ps1` runs the game muted and minimized for a given time and shows the end of
   the log. `-VideoAt <s> -VideoFrames <n>` records the frames from the presenter (it works minimized).
   `tools/make_video.sh` makes the MP4 and a contact sheet, and `tools/compare_video.py` lines a recording up against a
   reference video of the real console.
3. **Crash triage:**
   - "Unregistered call": a pointer-reached or gap function (3.2).
   - Guest access violation: symbolize the host address (`llvm-symbolizer` on `sa2.exe+0xRVA`) to find the guest
     function. `app/src/sa2_crash_log.cpp` also logs illegal instructions.
   - Temporary hooks that log the callers of a crashing function (`app/src/sa2_debug_hooks.cpp`) find the rest.
4. **What games expect from the system:**
   - XBLA games ask for their license: `license_mask = 1` runs the full game instead of the trial.
   - DLC: install the packages into the content store (`app/src/sa2_dlc.cpp` does it from a `dlc/` folder).
   - Saves, profiles and achievements come from the SDK's XAM; check that saving and loading work early.
5. **Stop when** the game plays from boot to its menus, gameplay and cutscenes and matches console footage.

## 5. The native renderer

### 5.1 Why not emulate the GPU

The SDK's emulated GPU interprets the Xenos command stream, emulates EDRAM (the GPU's 10 MB on-chip framebuffer
memory) and re-translates shaders at run time. On the PC it runs; on the Switch it is several times too slow. The
native renderer keeps the idea and drops the emulation's generality:

- **A ring thread** (`app/src/me/me_native_system.cpp`) reads the game's GPU commands (PM4 packets) from the ring and
  keeps the Xenos register file the commands describe.
- **Draws:** each draw becomes a Vulkan draw with the game's own shaders, already translated (section 6). Its
  pipeline is chosen from the render state in the registers, and its vertex layout from the vertex shader's fetch
  instructions.
- **EDRAM** becomes Vulkan render targets: resolves (EDRAM to texture copies) and clears act on images.
- **Textures** are untiled and converted from guest memory when their contents change.
- **The present** shows the game's front buffer.

This repository's renderer (`app/src/me`) came from masseffect-nx through banjo-nx. A new port reuses it and only
adds the game-specific glue: hooks on its Direct3D functions (5.2) and its shader package.

### 5.2 The game-specific part

1. **Find the game's Direct3D functions.** They have no names in a retail XEX. Match them against a game whose XDK
   functions are known (`tools/d3d_match.py`), and confirm by the call layout (`tools/listing_diff.py`).
2. **Hooks the renderer needs** (`app/src/me/sa2_me_hooks.cpp`):
   - the draw-time shader load;
   - the device's bound vertex and pixel shader offsets (they move between XDK versions:
     `SetDeviceShaderOffsets`);
   - DrawVertices, DrawIndexedVertices and DrawVerticesUP: they tell the ring which shader objects a draw used
     (`NoteDrawCall`);
   - CreateVertexShader and CreatePixelShader, to dump the game's shader containers;
   - the flip, for frame pacing and captures.
3. **Compare with the emulated GPU.** Record the same scene with both renderers and compare frame by frame. The
   lockstep mode (`vblank_per_swap`: one vblank per presented frame) makes two runs produce the same frames
   regardless of speed (section 12). Every difference gets traced with a single-frame dump until there are none left:
   boot screens, menus, gameplay, cutscenes.

### 5.3 What the renderer already does well

These come with the renderer and are worth knowing before you tune it:

- **Pipeline prewarm:** every pipeline the game creates is recorded in a list (`cache/sa2_native_pipelines.bin`,
  together with the Vulkan pipeline cache), and the next start compiles them before they are needed. Without it, every
  new state stutters the first time.
- **Deferred recording:** the ring thread queues its Vulkan calls and a worker thread records them (masseffect-nx's
  console: the ring thread went from 62 % to 40 % of a core).
- **Vertex and index dedupe:** identical uploads in a frame are found by fingerprint and uploaded once.
- **Upload memory:** vertices and indices go through a CPU-cached buffer published before each submit (SA2's console
  wrote 5,251 MB/s there, against 2,506 in uncached memory).

## 6. Shaders

### 6.1 The pipeline

The Xenos runs shaders as microcode inside "containers" (the compiled shader objects Direct3D creates). Each one is
translated ahead of time:

```text
container (vs_/ps_<hash>.bin)  --XenosRecomp-->  HLSL  --DXC-->  SPIR-V  --me_pack_shaders-->  sa2_shaders.mesp + .idx
```

`tools/build_shader_library.sh <dump folders>` runs all of it. At run time the renderer identifies each shader the
game loads by its microcode and uses the package's SPIR-V. The `.idx` lets the console read the index only, and each
SPIR-V when it is first used.

### 6.2 Collecting every shader

1. **The game's own containers:** run the PC build with the shader dump on (`sa2_dump_shaders = true`): every
   container Direct3D's CreateVertexShader / CreatePixelShader receives is written to `shaders/`. They usually also
   sit in the game's files (SA2: `Shader/xbox360/obj/*.wpu` and `*.wvu`), which is how an installer can find them on
   the player's computer.
2. **Direct3D's own shaders** (clears, copies, the present) live inside Direct3D as raw microcode, in
   `default.xex`. The discovery dump (`SA2_SHADER_DISCOVERY`) writes every shader the package does not know;
   `tools/wrap_discovered.py` wraps those that no real container covers into register-linked containers. Note where
   each one's microcode sits in the decompressed executable and which game container was its template: an installer
   can then rebuild the same containers, byte for byte, from the player's own `default.xex` (SA2's installer has 16
   wrapped shaders and one written over a game container of the same size).
3. **Vertex shaders Direct3D rewrites.** When a draw binds a vertex declaration, Direct3D patches the vertex shader's
   fetch instructions for it (buffer, offset, format, destination swizzle), so the microcode the GPU gets differs from
   the container. The renderer copes with three kinds of rewrite:
   - **Changed fetch fields** (constant, offset, format): ignored by the identity check, applied per draw from the
     loaded microcode.
   - **Changed destination swizzles:** a component order (`D3DCOLOR`) or 0/1 for an element the declaration lacks.
     The draw path remaps them per input (a select chain in the shader: exact).
   - **Reordered fetches:** for some declarations Direct3D moves the fetches to each other's slots. SA2's Chao
     garden stats window was missing because of this. `app/src/me/me_vertex_fetch_reorder.h` proves at run time that
     a load is a library shader with its fetches permuted: every other word identical, a unique bijection of the
     fetches, moves only inside fetch-only runs. The draw then uses that shader. The offline repair tool
     (`shaders/tools/repair_vertex_variant_declarations.cpp`) does the same proof to build a container for one order.
   Run the game with `SA2_VERTEX_VARIANTS` to dump the loads no entry matches, and check the log for
   `unidentified VS IM_LOAD` until there are none.
4. **Repeat** across the whole game (menus, every stage type, cutscenes, multiplayer) until the log says 0
   unidentified loads and 0 draws without shaders.

### 6.3 Translator fixes worth knowing

XenosRecomp was written for another game. This port's copy (`shaders/XenosRecomp`) adds, among others:

- fetches whose swizzle is all constants;
- interpolators linked by hardware register;
- structured control flow (nested if/else) instead of `skipTo` chains;
- fewer per-vertex tests through specialization constants.

When a shader looks wrong, compare its HLSL with the microcode's disassembly before suspecting the renderer.

## 7. Moving to the Switch

### 7.1 The build

- devkitA64's GCC compiles the same C++ (`tools/win/build_switch.ps1`; the toolchain file is
  `tools/switch/cmake/switch-devkitA64.cmake`). The generated code is shared with the PC build. A cross build cannot
  run the code generator, so translate on the PC first.
- The NRO links NVK statically. Check that the right driver went in: a generic Mesa from devkitPro's portlibs once
  got linked instead (`tools/check_switch_build.py` checks the Mesa version string).
- `tools/check_switch_build.py` runs on every build. It checks that the start-up code is at address 0 (a section
  ordering file once moved it and the NRO would not boot), that the optimised flavour is in, that no vector fused
  multiply-add leaked past `-ffp-contract=off`, and that branch-to-self loops are only known guest halts (a polling
  loop GCC turned into `b .` means a missing barrier).

### 7.2 The Horizon layer (in `sdk/`, shared by every port)

- **Memory:** the guest's 4 GB address space is reserved and mapped in views. Page faults are handled for the
  regions the game watches.
- **Threads:** the Xbox 360 had six hardware threads on three cores; the Switch gives a game three cores. Priorities
  matter: only priority 0x3B is time-sliced, in 10 ms slices.
- **Crashes:** the exception handler writes `logs/rex/rex_crash.log` with the registers, the guest function and the
  stack.
- **Files:** thousands of small files on the SD card are slow to open. The asset pack (`game_root.nxpack` + `.nxidx`,
  `tools/build_asset_pack.py`) puts the whole game in one file. The console builds it on the first start if only a
  `game_root/` folder is there.
- **Audio:** the SDK's XMA decoder and mixer, out through `audout`.
- **Input:** HID pads to XInput, per player, with sideways Joy-Cons
  (`sdk/src/input/switch/switch_input_driver.cpp`).
- **Display:** docked or handheld, SaltyNX/ReverseNX support, and the FPS overlay (`sdk/src/ui/switch_saltynx.cpp`).

### 7.3 Launching

The game needs more memory than a homebrew gets in applet mode (from the album). Launch it with a title override
(hold **R** while starting a game) or a forwarder set to a **39-bit address space**.

### 7.4 Bugs that only the console shows

These cost days on the ports before this one:

- **Thread handles leak** until Horizon's limit and the game cannot create threads (the SDK's `switch_reaper` closes
  them).
- **Short reads from the asset pack** on large requests (`pack_storage_switch.cpp`, `ReadAt` loops until done).
- **SD writes stall the thread that logs.** A long log line was an SD write of its own, made by whichever thread
  logged it; the log writer thread (`log_file_writer`) writes once a second instead.
- **`thread_local` is a function call** (`__aarch64_read_tp`) on the Switch: keep it out of loops.

## 8. Measuring on the console

You cannot optimise what you have not measured, and the PC's measurements mislead (section 0). The SDK's profiler
(`sdk/src/ui/switch_perf.cpp`) writes `logs/rex/rex_profile.log` every 10 seconds with:

- the game's frame rate, each thread's exact CPU time (from the kernel), its priority and core;
- the clocks (CPU, GPU, memory) and temperatures;
- per-second counts of the libnx calls that cost IPC (fence queries, submissions, mappings, cache cleans);
- **stack samples** (`switch_profile_stacks = true`): every millisecond one busy thread is paused, its pc, lr and
  frame chain read, and resumed. Addresses come out as `image+0x...`, translated with addr2line on the build's ELF.
  Archive the ELF of every NRO you test (`out/nx/elf/`).

`tools/switch_profile_report.py` and `tools/cpu_profile_report.py` turn the samples into per-function shares. Things
that misled us:

- **The session changes clocks.** A console log can run at 1020, then 1785 MHz in the same session. Compare intervals
  at the same clock (`clocks:` in each interval).
- **Stack-based "self" time charges a frameless leaf to its caller.** Use pc-based self time for leaves (njCalcPoint
  was 8.5 % of SA2's game thread by pc, invisible by stack).
- **GPU time:** the renderer's GPU timestamps (`sa2_gpu_marks_categories`) split each frame's GPU time by pass
  category and show the idle gap.
- **A/B tests:** the same scene with one setting changed, from the configurations `tools/make_enhancement_configs.py`
  writes. The frame rate is capped, so compare the threads' CPU time, not only FPS.

## 9. Making the CPU fast

On SA2 the CPU limits everything: the game thread (the game's logic and its Direct3D calls) and the renderer's ring
thread each needed under 16.7 ms a frame. Below, in the order that paid off most. Each has its own setting
(`app/src/cpu/sa2_enhancements.h` is the framework: a key per optimisation, two master keys, a startup line saying
which ran, and a use counter every 10 seconds).

### 9.1 The code generator's output

The translated code is the game's code, so its shape matters on every function at once.

- **Registers as locals** (`non_volatile_as_local`, with `non_argument_as_local`): the guest's non-volatile registers
  (r14-r31, f14-f31, v14-v31/v64-v127) and the registers that do not pass arguments become C++ locals instead of
  fields of the context struct. GCC then keeps them in host registers instead of loading and storing memory around
  every instruction.
- **No link register stores** (`elide_lr_stores`): `ctx.lr = <return address>` before every call is dropped (nothing
  reads it in translated code). Run `tools/read_before_write.py` first: it finds code that reads a register its
  caller left, which these options would break.
- **Float moves without mode switches** (`fp_moves_modeless`): the generator switched the FPU's flush mode around
  scalar float instructions in vector code. FPCR writes are expensive on the A57; in SA2's console profile 20-26 % of
  the game thread's samples sat right after an `msr fpcr`. Pure moves now switch nothing.
- **Direct calls** (`tools/direct_calls.py`): calls between functions that are not hooked become plain C++ calls
  instead of going through the guest's function table, so GCC can inline them (across files, with LTO).
- **No volatile in the precompiled header** (`tools/pch_no_volatile.py`): the generated code's memory accesses do not
  need `volatile`; without it GCC can keep values in registers. Loops that poll memory written by another thread
  still need a barrier (`tools/loop_barriers.py`, `tools/find_self_loops.py`).

These are build-time options (the "optimised codegen flavour", named in the first log line). Check each with the
verify mode and the pixel tests (section 12) before keeping it.

### 9.2 The compiler

- **`-O3`**, **`-ffp-contract=off`** (the game's float results must not change: no fused multiply-add the guest did
  not have) and **`-fno-strict-aliasing`** (the translated code reads the same memory as different types).
- **LTO**: the translated game and the app are compiled to GIMPLE and optimised as one program at the link
  (`-flto=N -flto-partition=balanced`), so small game functions inline across files. Repeat
  `-ffp-contract=off -fno-strict-aliasing` at the link: the LTRANS stage must not fuse what the compile stage kept
  apart. One trap: xxHash's default memory access method broke under LTO (garbage hash keys once inlined); build it
  with `XXH_FORCE_MEMORY_ACCESS=0`.
- **`-fipa-pta`**: interprocedural points-to analysis at the LTO link.
- **Hot functions first** (`app/hot_functions.ld` from `tools/hot_order.py` over console samples): the NRO has about
  55 MB of code and the A57 a 48 KB instruction cache and a small TLB. Grouping the hot set at the start of `.text`
  saves instruction-cache and TLB misses on far calls. Keep `KEEP(*(.crt0))` first: the start-up code must stay at
  address 0.
- **PGO** (profile-guided optimisation): the compiler lays out and inlines code by how often it really runs.
  1. Build an instrumented NRO (`build_switch.ps1 -Pgo generate`) with `-fprofile-generate
     -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info`.
     - libgcov's own way to save the counts (a `.gcda` file per object at exit, at the build machine's paths) does
       not fit the Switch, where games are often closed from the HOME menu. `-fprofile-info-section` registers each
       object's counts in a section instead, and `app/src/sa2_pgo.cpp` streams them all into one file
       (`__gcov_info_to_gcda`): every 2 minutes, on the HOME button and at exit.
     - Value profiling is off: its shared lists are not safe without atomic counter updates across the game's
       threads.
  2. Play it. Then on the PC, `tools/win/pgo_merge.ps1 <pgo folder>` runs `gcov-tool merge-stream`, which writes the
     `.gcda` files next to the objects and adds the sessions together. `tools/pgo_summary.py` then fills in each
     file's summary (the stream API writes runs = 0, and GCC derives its hot and cold thresholds from it).
  3. Build with `-fprofile-use -fprofile-correction -fprofile-partial-training`. `-fprofile-correction` is required:
     counts taken without atomics are not "flow-consistent" and GCC stops otherwise. `-fprofile-partial-training`
     compiles code the session did not reach as without a profile, instead of optimising it for size.

  Change no game or app code between the instrumented build and the use build: a changed function loses its profile.
  On SA2 one console session made the game about 15 % faster: single player stopped dipping below 60 FPS. Ship the
  profile with the source (SA2's is in `pgo/`): built with `-fprofile-use=<folder>
  -fprofile-prefix-path=<build folder>`, GCC names each `.gcda` by the object's path inside the build folder, so
  anyone building the same code gets the released NRO's optimisation.

### 9.3 The game's hottest functions as native code

The profile shows which game functions take the time (SA2's: the Ninja engine's matrix and vertex code, memcpy,
Direct3D's draw path). There are three ways to make one of them native, from cheapest to fastest:

1. **Localized copies** (`tools/localize_native.py`): the function's translated code, regenerated with its registers
   as C++ locals for the whole body, and each call syncing only the registers the callee can reach ("smart sync").
   Exact by construction: it is the same code. Before smart sync, copies that made real calls were slower (every
   register written back and reloaded around each call); with it, SA2 runs about 50 copies.
2. **Hand-written natives** (`app/src/cpu/hot/`): the function rewritten in C++ from its listing, for example
   njCalcPoint and njMultiMatrix (`n_ninja_matrix.h`) or the chunk-model vertex loaders with njCalcPoint inlined
   (`n_vertex_chunks.h`).
3. **CRT functions** (memcpy, memset, memmove): host versions that do the same thing (NEON on the Switch).

A native must be **exact**, and it proves it while the game runs (`app/src/cpu/sa2_hot_guest.cpp`):

- **The guard:** the first 256 calls, then 1 in 4,096, run both the original and the native on copies of the state,
  and compare the registers the callers can read, the FPSCR and every byte the function writes. Verify mode
  (`sa2_cpu_verify`) checks every call. A difference logs both sides and switches that native off for the session.
- **Write down what it writes:** each native lists the memory it may write (its frame, its outputs), so the guard
  compares all of it.
- **The callers' view of registers:** the Xbox 360's compiler optimised across functions. A caller may keep a value
  in a volatile register across a call when it knows the callee leaves it alone. A native must therefore never write
  a register the original leaves untouched. `tools/call_site_liveness.py` lists, for every call site, the registers
  read after the call before being written, and the comparison mode follows from it (SA2's `kReturnInt`: integer
  results, no float return register).
- **Exact floats:** single-precision operations are double operations rounded to float after each step, as the
  PowerPC does them. `fmadds` is `std::fma` then the rounding; `fctiwz` truncates and saturates (NaN gives
  0x80000000); vector multiply-adds are a separate multiply and add in translated code. NaN payloads may differ
  between two compilations of the same C++, so floating-point comparisons treat any NaN as equal to any NaN.
- **The guest code must not have changed:** each native records the FNV-1a hash of the guest code it was written
  from (`tools/guest_code_hash.py`) and stays off if the game's code differs (another edition, a title update).
- **Cases it does not handle run the original:** declining is always allowed. For example, a vertex loader declines a
  chunk list that overlaps its own output.

SA2's results for one round: njCalcPoint inline in the vertex loaders, and njMultiMatrix writing only its result, took
the 2-player race's largest single cost (8.5 % of the game thread) off the table. Verify mode made 198.7 million checks
with 0 differences.

### 9.4 The renderer's CPU cost

The ring thread was SA2's other limit: 19.5 µs a draw and about 1,200 draws a frame, 22 ms (round 4). What brought it
down, besides replacing Direct3D's command stream (section 10):

- **Shader identity:** a draw's shaders are identified once per load and the result remembered by content; a
  (library shader, loaded program) pair that failed once is never checked again.
- **Pairing a draw with its shader objects** directly from the draw's record instead of guessing from addresses.
- **Fingerprints with the A57's CRC32 instructions** instead of XXH3's NEON loop (`sa2_native_fingerprint_mode = 2`).
- **Fast paths that check themselves** against the old path for their first uses: register runs, constant uploads
  compared with what was already uploaded, render target state reused when nothing changed (the EDRAM memo: about 9 %
  of the ring).
- **Deferred Vulkan recording** on a worker thread (5.3).

Each of these is in `docs/enhancements.md` with its measurement and its key.

### 9.5 Waiting less

- **The game's GPU wait sleeps** instead of spinning (`app/src/cpu/sa2_gpu_wait.cpp`): SA2's PC game thread went from
  1.01 to 0.55 cores. The ring's wake-up is lock-free.
- **Relaxed vsync** (`sa2_vsync_relaxed`): with a fixed 60 Hz vblank, a frame that took 17 ms waited for the next
  vblank and the game fell from 60 straight to 30 FPS (and, since it runs per frame, to half speed). The renderer now
  gives the game its vblank when the flip waits for it, never sooner than 1/60 s after the previous one, like a
  variable-refresh display. The flip is the WAIT_REG_MEM after each swap that the game's vblank callback releases;
  the renderer learns which one it is at start.
  - PC test with a simulated slow GPU, 18 ms of ring work per frame: 30 FPS with the fixed vsync, 54.6 FPS relaxed.
- **Priorities:** a drawing thread one step above the other game threads avoids 10 ms time slices behind them. SA2
  keeps it off until a console A/B shows it helps.

## 10. Replacing Direct3D

The renderer of section 5 still reads PM4: the game's Direct3D encodes every draw's state into command packets, and
the ring thread decodes them back. That is double work, and the emulation's guessing (which shaders a draw used,
which state changed) costs too. The way out is to replace Direct3D itself, one entry point at a time, each step
pixel-identical and checked. `docs/direct-renderer.md` and `docs/api-renderer.md` hold the full record. The steps:

### 10.1 Direct state records (step 1)

The Draw* hooks wrap the original call. Right after it, the packets the call wrote before its draw packet are decoded
into a host record: register runs in host byte order, plus the few type-3 packets the ring handles. The region becomes
one NOP packet carrying the record's number (`'SA2D'`). The ring applies the record where the packets were, with the
same register writes. The draw packet and every address Direct3D keeps stay in place. The one hard part is
concurrency: a submission that races with a record being marked must either see the whole marker or none of it.

### 10.2 Direct3D's flush writes the record (step 2)

Direct3D's draw-time state flush (shader constants, register groups, fetch constants) writes its register runs
straight into the record. A 3-word placeholder stands in the command buffer, so nothing is encoded to PM4 and decoded
back. The hooks follow the original's fast path on paper: the space checks and the fillers it would write. Whenever
the original would take its slow path, they run it instead.

### 10.3 The native Direct3D layer

Then the Direct3D functions become native code altogether (`app/src/me/sa2_api_*.cpp`): DrawVertices with its state
flush and shader load, DrawVerticesUP, the window-scissor writer, Clear, and (in progress) the resolve. The method:

1. **Census:** count which Direct3D functions the game really calls and which of them write PM4 at run time
   (`sa2_diag_d3d_census`, `app/src/me/sa2_d3d_api.cpp`). SA2 called 109 functions, and 14 of them wrote PM4 per
   frame. Beware: the "Direct3D range" of addresses may contain other libraries (SA2's started with XAPI's heap
   functions).
2. **Device layout:** the offsets of the dirty masks, shadows, command buffer pointers and bound shaders in the device
   structure, per XDK build (`docs/api-renderer.md` lists SA2's 2012 layout and Futari's differences).
3. **Transliteration:** each native function follows Direct3D's instructions line by line, with helpers that mirror
   PowerPC rotate-and-mask, so every line can be held against the listing. Branches the game never takes (predicated
   tiling, debug assertions) are checked before anything changes, and the original runs instead. Pure helpers (the
   vertex fetch patcher, the interpolator relink) are called as they are.
4. **Records instead of PM4:** the layer writes operations for the renderer: register runs, the effectful type-3
   packets (draws, shader loads, events), and the draw's shader objects (no pairing heuristics). The device is left
   exactly as Direct3D leaves it, because the game's own code reads and writes it inline.
5. **Verify mode, shadow runs:** with `sa2_cpu_verify`, the original first runs on a copy of the device whose command
   buffer is a scratch area (it never submits). Then the native code runs for real, and the two are compared:
   - its ops against the original's packets, both reduced to a canonical form;
   - the device bytes after each, except the transport's fields and those other threads write;
   - the guest memory each wrote outside the device.

   SA2 compared 2,150,579 native draw records with 0 logic differences before turning the layer on.
6. **ARM first:** the first console run of the layer was correct but cost 18 % of the drawing thread, where the path
   it replaced cost 12 %. None of the causes showed on the PC:
   - an op-writing counter that aliased the buffer pointer, so GCC reloaded it around every word: write through a
     pointer cursor instead;
   - byte swaps one word at a time: use NEON `rev32`;
   - `thread_local` inside a loop: a function call each time on the Switch;
   - copies of the whole 2.7 KB guest context for each helper call;
   - cache misses at the record ring's publish: prefetch the next lines, and allocate them with `DC ZVA` (zeroed
     cache lines with no DRAM read).

   The next round brought the layer to 8-9 % of the thread.

The end state (still in progress in SA2): no PM4 at all, records through a host queue, and EDRAM replaced by images
per Direct3D surface.

## 11. Making the GPU fast

The Tegra X1's GPU (Maxwell, 256 cores, 460-768 MHz) is the limit only in some scenes, and NVK has its own costs. What
`docs/gpu-optimisation-candidates.md` found and SA2 did, each pixel-identical:

- **Fewer render pass breaks and barriers:** clears inside the render pass, multi-rectangle clears, narrowed end
  dependencies.
- **Resolves:** resolves of the color the pass already holds skipped or aliased, never-read scratch resolves dropped,
  a duplicate full-screen resolve shared.
- **Per-draw CPU and GPU work in the shaders:**
  - push constants once per recorded state instead of per draw;
  - structured control flow in the translated shaders (`sa2_gpu_structured_cf`);
  - specialization constants for facts that never change within a pipeline (texture signs, unswapped texcoords,
    identity input remaps), so the shader does not test them per vertex or pixel;
  - blending skipped when the blend is the identity.
- **The driver** (nfsmw-nx's NVK patch): descriptor set 4 updated by differences, cheaper draw emission, ZCULL planes
  for the depth buffers that can have one.
- **Resolution and MSAA:** render scale (720p handheld, 1080p docked, switched live when docking) and optional 2x/4x
  MSAA (`app/src/me/sa2_render_mode.cpp`). Handheld 720p without MSAA is exactly the game's own image.

## 12. Proving every change

This is what lets one person change a renderer and a CPU path every day without breaking the game:

- **A key per change, and two masters.** `sa2_cpu_enhancements` and `sa2_noncpu_enhancements` switch whole groups
  off; every change has its own key. The first log line lists which are on, and the `[enh] uses` line every 10 s
  shows they really ran.
- **Self-checks:** every fast path compares itself with the old path for its first uses (and every use in verify
  mode), counting checks and differences in the `[verify]` line. The difference count must stay 0.
- **Pixel tests in lockstep:** two runs of the attract demo with `vblank_per_swap` (one vblank per presented frame),
  capturing 3,000 frames from the same frame number. The capture keeps the XXH3 of every frame at full resolution.
  The runs number their frames from different starting points, so align them by content first. Equal means equal
  for every frame:
  - SA2's rounds were each checked this way (for example 2,999 of 2,999 frames identical);
  - the test must also fail when the code under test is off. The reordered-fetch fix was tested with the repaired
    shaders removed: 2,596 frames differed with the fix off, none with it on.
- **Look at the frames before calling it a difference.** Animations that run on wall-clock time (a blinking "PRESS
  START", a 24-frame loading loop) shift between runs. Check whether the differing image exists at another frame.
- **Console A/B configurations** (`tools/make_enhancement_configs.py`): the shipped settings with one group or key
  changed, and the profiler on, for the same scene.
- **Tests for the tricky code:** host-built unit tests, such as `shaders/tests/test_vertex_fetch_reorder.cpp` (the
  run-time proof against the offline tool, on every possible fetch order).

## 13. Shipping

- **Settings:** compile the fixed settings into the NRO (`app/sa2_switch_defaults.toml`, applied before the
  player's file), and give the player a short `sa2.toml` with only what they may want to change. The game writes it
  when it is missing.
- **Logging off:** no `logs/` folder in a release (`log_level = "off"`, `log_to_file = false`, no profiler report).
  Crash reports stay. The A/B configurations turn everything back on for testing.
- **The homebrew menu entry:** name, author, version and a 256x256 JPEG icon (`rexglue_switch_add_nro` in
  `app/CMakeLists.txt`).
- **PGO** from a real session (9.2).
- **No game data in the release.** The shader package is made from the game's shaders, so the player makes it from
  their own copy. An installer page does it in the browser, as nfsmw-nx's
  [installer](https://github.com/StevensND/nfsmw-nx-installer) first did. SA2's
  ([sa2-nx-installer](https://github.com/ChanseyIsTheBest/sa2-nx-installer)) reads the XBLA package in place, identifies
  `default.xex` by its SHA-256, and builds the shader library with the translator, DXC and the packer compiled to
  WebAssembly. It checks the library against the SHA-256 of the one the NRO was tested with, writes the game files as
  the asset pack, and streams the zip to disk through a service worker.
  - The library the page makes must be the one you tested: DXC versions differ in their SPIR-V. SA2's page uses DXC
    v2025.1, which wrote different SPIR-V than the PC's 1.8 for 10 of 35 shaders: the same arithmetic, inputs read
    directly instead of through a local copy. That library was then checked in lockstep on the PC (2,999 of 2,999
    frames identical) before it was released.
- **First-start stutter:** say in the README that the first session compiles pipelines, and that `cache/` keeps them.

## 14. Traps

Each of these cost a day or more somewhere:

- **Run CMake from the right shell.** A configure from a shell with MSYS2's tools on the PATH saw a "compiler change",
  wiped the build folder's cache and objects, and wrote the wrong ninja and linker into it.
- **Never move the start-up code** from address 0 (a section ordering file must keep `.crt0` first).
- **Vector fused multiply-adds must not appear:** the guest's vector multiply-add is not fused, so the results would
  differ. Keep `-ffp-contract=off` at compile and link.
- **The "Direct3D range" is not all Direct3D** (10.3).
- **A guard that compares a scratch register** reports false differences when a nested call is itself checked. Find
  the callers' live registers first (9.3).
- **Statics initialised on first call** make a shadow run take another path when another thread's first call lands
  in between: hold a lock from the shadow run through the comparison.
- **Lockstep differences from wall-clock animations** (12).
- **Applet mode** has too little memory; launch with a title override or a 39-bit forwarder.
- **An identity check that holds fetches at their slots** silently drops the draws of reordered vertex shaders (6.2).
- **PGO counts from threads are inconsistent:** use `-fprofile-correction` (9.2).
- **The PC's timings:** a change that halves a PC profile entry can do nothing on the A57, and the reverse. Measure
  on the console.

## 15. Glossary

| Word | Meaning |
|---|---|
| XEX | The Xbox 360's executable format (`default.xex`): PowerPC code, compressed and encrypted |
| XDK | Microsoft's Xbox 360 development kit; games link its Direct3D statically |
| STFS | The package format of Xbox Live Arcade games, DLC and title updates |
| Xenos | The Xbox 360's GPU |
| PM4 | The command packet format the Xenos reads from its ring buffer |
| EDRAM | The Xenos's 10 MB of on-chip framebuffer memory; render targets live there and are copied ("resolved") to textures |
| Microcode / container | A compiled Xenos shader; the container wraps the microcode with its constant table and vertex declaration |
| Vertex declaration | The layout of a vertex buffer; Direct3D patches the vertex shader's fetch instructions for it |
| ReXGlue | The static recompiler and runtime: translates PowerPC to C++ and provides the Xbox 360 system |
| Code generator flavour | The set of options that shape the translated C++ (registers as locals, direct calls...) |
| Localized copy | A guest function's translated code regenerated with its registers as locals |
| Native | A function replaced by hand-written C++ that does the same thing |
| Guard / verify mode | Running a native and the original on copies of the state and comparing them |
| Ring thread | The renderer's thread that reads the game's GPU work and records Vulkan |
| NVK | Mesa's open-source Vulkan driver for NVIDIA GPUs, ported to the Switch |
| NRO | A Switch homebrew executable |
| LTO / PGO | Link-time optimisation / profile-guided optimisation |
| Lockstep capture | A recording where every presented frame is captured in order, so two runs can be compared frame by frame |
