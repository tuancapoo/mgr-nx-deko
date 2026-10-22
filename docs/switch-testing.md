# Testing sa2-nx on the Switch

## SD card layout

```
sdmc:/switch/sa2-nx/sa2.nro
sdmc:/switch/sa2-nx/sa2.toml              your options (app/sa2_switch.toml); everything else is built in
sdmc:/switch/sa2-nx/sa2_shaders.mesp      shader package for the native renderer
sdmc:/switch/sa2-nx/sa2_shaders.mesp.idx
sdmc:/switch/sa2-nx/game_root.nxpack      your game files as the asset cache (one file; tools\build_asset_pack.py)
sdmc:/switch/sa2-nx/game_root.nxidx       its index
sdmc:/switch/sa2-nx/dlc/                  optional: the Battle DLC package (out\pc\dlc)
```

`tools\win\package_switch.ps1 -IncludeGame` builds this whole folder in `out\nx\sdcard\switch\sa2-nx`: copy it to the
card as `switch\sa2-nx`. With a loose `game_root/` folder instead of the pack (`-LooseGame`), the first boot builds
the pack on the card (a few minutes, logged as `[pack] packing N %`); the folder can be deleted afterwards. When a pack
exists it is always used, so an old `game_root/` folder from earlier tests is no longer read and can be deleted.

## This build (2026-10-14, version 1.0.2.1, pre-release)

- **Long sessions, second fix:** 1.0.2 did not cure them. After three to four hours of play textures were black and
  geometry was missing again until the game was closed. The cause found since: the renderer has 4,096 slots for 2D
  textures, the texture cache only released textures by the size of their pixel data, and SA2's textures are so
  small that the slots ran out first; every texture first seen after that was drawn black or not at all. The cache
  now also makes room when the slots run low (`sa2_native_texture_slots_evict`; `docs/enhancements.md` of the
  repository). On the PC the fault was reproduced with fewer slots and is gone with the fix, frame for frame.
- **A pre-release because the fix is not confirmed on the console yet:** the test is a session of four hours or
  more without closing the game. If the glitches still appear, a run with one of the `configs/` files (they turn
  the log on) shows the numbers: the line `[native] C3 slots: 2D N of 4095 in use ...` every ten seconds.
- **Logging and the profiler are off**, as in every release.
- **Only `sa2.nro` changes** against 1.0.1 and 1.0.2.
- **If the game crashes:** please send `switch/sa2-nx/logs/rex/rex_crash.log` from the SD card; it is written even
  with logging off and says what went wrong, which the console's own crash report does not.

## Version 1.0.2 (2026-10-13, pre-release)

- **Long sessions, first fix:** the renderer kept a copy of every render-target texture (mostly shadow maps) for
  the whole session. Copies the game stopped using are now released (`sa2_native_resolved_retire_idle`). It was
  not the cause of the reports: the console's long sessions failed as before.

## Version 1.0.1 (2026-10-13)

- **Faster:** three rounds of CPU optimisations, a new profile recorded on the console with this code, and a better
  link (`docs/cpu-rounds.md` of the repository). In play it was estimated at about 15 % faster than 1.0.0.
- **Chao garden and kindergarten:** the black speckles on the ground are fixed (a shadow map was read from freed
  memory; `sa2_native_partial_resolve_inside`).
- **A self-check at every start:** if an instruction form of this processor gave a wrong result, the game writes
  `sa2_selfcheck_failed.txt` next to the NRO. No file means every check passed.
- **Only `sa2.nro` changes:** `sa2.toml`, the shader package and the game files are the ones of 1.0.0.

## Version 1.0.0 (2026-10-08, PGO-optimised)

- **Optimised from your PGO session** (`sa2_pgo_1.stream`): the compiler now knows which code the game really runs and
  lays it out and optimises it for that. Code your session did not reach is compiled as before.
- **`sa2.toml` is created when it is missing:** the game writes the default one next to the NRO at start, ready to edit.
- **Nothing is written to `logs/` any more:** no `rex_stderr.log` and no `rex_profile.log`. Crash reports are the only
  exception: `logs/rex/rex_crash.log` is still written if the game crashes. The FPS overlay keeps working.
  - To get the logs back for a test, copy any file from `configs/` over `sa2.toml`.

## Version 1.0.0, PGO collection (2026-10-08)

- **`sa2.toml` holds only your options:** language, MSAA, relaxed vsync and the controls. Everything else is built
  into the NRO (`app/sa2_switch_defaults.toml`). A key copied into `sa2.toml` still overrides the built-in value, for
  testing.
- **Language** (`language`): `auto` follows the Switch's language. You can also set `english`, `japanese`, `french`,
  `german`, `spanish` or `italian`. A language the game lacks gives English. PC test: the menus came up in French and
  English.
- **Player 2 has its own button map:** the `[input_p1]` and `[input_p2]` sections of `sa2.toml`. They replace the
  old `input_map_*` keys. Players 3 and 4 use player 1's map.
- **No logging and no profiling:** no `logs/` folder, no profile and no shader-miss dumps. The `configs/` files turn
  the log back on, for testing.
- **Version 1.0.0, author ChanseyIsTheBest, new icon** in the homebrew menu.
- **Mass Effect names are now SA2:** settings, files, log text and thread names. The pipeline cache is now
  `cache/sa2_native_pipelines.bin`; the first start reads the old `masseffect_native_pipelines.bin` once, so nothing
  is recompiled.
- **This NRO is instrumented for profile-guided optimisation (PGO).** It counts where the game's code goes, so it runs
  slower than a normal build. Its frame rate says nothing; the next build, made from these counts, is the fast one.
  - Play as much of the game as you can: story stages of several characters, 2P, the Chao garden, the menus.
  - The counts are saved to `pgo/sa2_pgo_<N>.stream` next to the NRO, one file per session. They are saved every 2
    minutes and whenever you press HOME. Press HOME before closing the game, or wait 2 minutes.
  - Send the whole `pgo` folder. On the PC, `tools\win\pgo_merge.ps1 <folder>` merges it, and
    `tools\win\build_switch.ps1 -Pgo use` builds the optimised NRO.

## Round 13 + the Chao garden fix (2026-10-07)

- **The Chao garden's stats window is drawn again** (the panel when you pick up a Chao). Its vertex shader is one the
  game's Direct3D rebuilds at run time with its inputs in another order, which the renderer did not recognise, so the
  window's draws were dropped. Fixed in two ways:
  - the shader package carries the two orders found in the garden (your PC test, 2026-10-07);
  - the renderer now recognises any such reordering itself (`sa2_gpu_vs_fetch_reorder`, on). Only 4 of the game's
    vertex shaders can be reordered (74 possible orders, 9 seen so far), and all 74 are covered, so no other area
    should lose draws this way, without anyone having to find them first.
- **What to check:** the stats window in the garden, and anything else you noticed missing before. In the log,
  `[native] sa2_gpu_vs_fetch_reorder: VS n.. identified with its fetches reordered by Direct3D` names an order the
  package did not carry, now drawn anyway: send the log if you see one. `unidentified VS IM_LOAD` lines should no
  longer appear.
  - A/B: `configs/sa2_test_vs_fetch_reorder_off.toml` (the renderer's own recognition off; the package's two garden
    orders still draw).
- Everything from round 13 below.

## Round 13 (2026-10-07)

- **No more falling from 60 straight to 30** (`sa2_vsync_relaxed`, on): a frame that misses 60 is shown as soon as it
  is ready instead of waiting for the next vblank, so a heavy stretch runs at 45-59 fps instead of 30 (the game still
  runs per frame: at 50 fps it runs at 83 % speed). Never above 60. PC test with an 18 ms GPU frame: 30 fps before,
  54.6 fps now. The log says `[native] sa2_vsync_relaxed: the flip wait polls ...` within the first seconds; send it if
  that line is missing.
  - A/B: `configs/sa2_test_vsync_relaxed_off.toml` (the old vsync).
- **The 2P race's vertex work, by hand:** the chunk-model vertex loaders (njCalcPoint inline, skinned characters too)
  and njCalcPoint / njMultiMatrix themselves, which were the game thread's largest single cost in your 2P log.
  - A/B: `configs/sa2_test_round13_off.toml`.
- **The pipeline list keeps every setting's records:** switching MSAA on and off no longer empties it (your two
  round-12 runs compiled every pipeline again).
- **What to check:** the 2P race and heavy 1P stretches at 1020 MHz (your round-12 2P log ran at 31-37 fps there);
  the speed of the game in those stretches; the `[enh] uses` line's `sa2_vsync_relaxed N` (frames shown without
  waiting, per 10 s) and `sa2_cpu_native_vertex_*` counts. Please keep the CPU at 1020 MHz for the comparison runs (the
  round-10 and round-12 logs both changed clocks mid-session).

## Round 12 (2026-10-07)

- **Docked 1080p, handheld 720p, live on docking**, and an optional **MSAA** setting: see "Render resolution and
  MSAA" below. Handheld 720p with MSAA off is exactly the image as before (PC: every frame identical).
- MSAA works on the console (your test, 2026-10-07). `configs/sa2_all_on.toml` is the defaults again, MSAA off.
- Everything from round 11 (the controls below, the tightened Direct3D layer).

## Round 11 (2026-10-07)

- The native Direct3D layer tightened again. The ring of records is readied in the cache ahead of time (DC ZVA),
  and the shader pair is remembered between draws. Same image: PC verify mode 0 differences, every frame
  identical. Compare the `[api] drawing-thread cost` line with round 10.
  - A/B: `sa2_api_ring_zero_ahead = false` turns the ring readying off.
- Controls and two players: see the next section.

## Controls and multiplayer (round 11)

- **Face buttons by position, as on the Xbox 360 pad** (default): the bottom button (B on Switch controllers) is A,
  the right one is B, the left one is X, the top one is Y. `input_xbox_layout = false` in `sa2.toml` goes by letter.
- **Two players:** every controller is its own player, numbered in the order they connect. The handheld (Joy-Cons on
  the console) counts as one: handheld + a Pro Controller = players 1 and 2; docked, two Pro Controllers or two
  Joy-Con pairs = players 1 and 2.
- **One Joy-Con each, held sideways:**
  - press **L + R + ZR** for the system's controller screen and choose to hold the Joy-Cons sideways (it also adds
    controllers and changes the order);
  - each sideways Joy-Con has its four buttons under the thumb as A/B/X/Y by place;
  - SL / SR are the triggers (camera), minus or plus is Start, the stick moves.
- **Remapping, per player:** the `[input_p1]` and `[input_p2]` sections of `sa2.toml` (`a`, `b`, ... `rt`, `start`,
  `up`, ...). Each names the Switch buttons that press that Xbox 360 button, for example `a = "B, ZR"`. Empty = the
  default. Read at start.

## Render resolution and MSAA (2026-10-07)

- **Docked 1080p, handheld 720p.** On the TV the game is drawn at `sa2_render_height_docked` (default 1080:
  1920x1080); in handheld at `sa2_render_height_handheld` (default 720: the game's own 1280x720, exactly as before).
  Docking or undocking switches at the next frame, with one short hitch while the render targets are made again; the
  log says `[res] render scale 4/4 -> 6/4 ... in N ms`. Heights are multiples of 180 from 360 to 2160; above 1080 is
  for the PC (GPU time and memory).
- **MSAA:** `sa2_msaa = 0` (default, as before), `2` or `4`: multisample anti-aliasing of the game's render targets,
  read at start. Works on the console (2026-10-07).
- **What to check:**
  - docked: a sharper picture (HUD, text, edges) and otherwise the same scenes as handheld; the FPS at 1020 MHz (the
    GPU draws 2.25 times the pixels: compare the `GPU per Swap` lines with handheld's);
  - dock and undock during play, in a menu and on a loading screen: one short hitch, then the right picture, nothing
    corrupt;
  - the main menu, the pause screen and the text boxes docked (the PC tests could not reach them);
  - `sa2_msaa = 2` and `4`, handheld and docked: smoother edges, the `GPU per Swap` lines and the FPS;
  - send `logs/`: the `[res]` lines give the mode, each switch's time and the memory of the scaled images.
- **A/B:** `configs/sa2_test_render_scale_off.toml` (1280x720 docked too: what 1080p costs the GPU),
  `configs/sa2_test_msaa2.toml`, `configs/sa2_test_msaa4.toml`.
- **Pipelines:** with the default settings no pipeline changes. A new MSAA setting, or a new docked height, compiles
  the pipelines it needs on first use (short stalls) until `cache/sa2_native_pipelines.bin` has them. Records
  of other settings (another MSAA setting, or keys from before GPU round 2) are not compiled at start but stay in the
  list (`[native] C6 prewarm: N records of the list are for other settings`, key `sa2_gpu_prewarm_current_keys`), so
  switching MSAA on and off no longer empties it (round 12's first version dropped them).

## This build (2026-10-07, round 10)

The native Direct3D layer reworked for the Switch's CPU (round 9's console profile: it took about 18 % of the drawing
thread). Same image, same game: PC verify mode 0 differences, every frame identical. The log's `[api] drawing-thread
cost` line, every 10 s, says what the layer costs now (µs per call and % of one core). Compare it with round 9.

## Round 9 (2026-10-07, after round 8)

Everything is on by default in `sa2.toml` (the same as `configs/sa2_all_on.toml`):

- the native Direct3D layer: draws and Clear without PM4 (docs/api-renderer.md);
- GPU round 2: six shader and constant-traffic optimisations. They need the `sa2_shaders.mesp` and `.idx` of this
  build; copy both. The first run compiles the new pipeline variants once (short stalls) until the prewarm list has
  them;
- `non_argument_as_local`: the last register class kept in memory now lives in C++ locals (smaller, faster code);
- the log writer thread: the log no longer stalls the ring thread on SD writes (round 8's stutters).

## Launching

Start it with a title takeover (hold R while starting a game, then open the Homebrew Menu), or with a forwarder that
has full memory. From the Album applet there is not enough memory.

## What to send back after a run

- `sdmc:/switch/sa2-nx/logs/` (the whole folder): the native renderer writes a report every 10 s (swaps, draws,
  rejected draws, unidentified shaders, GPU time per category).
- `sdmc:/switch/sa2-nx/shader_misses/` if it exists: every shader the package lacked (the next package adds them).
- How far it got (boot screens, title, attract demos), and anything that looks wrong, with a photo if possible.
- Loading: the `[io] 15 s:` lines in the log time every file open and read.

## A/B of the GPU optimisations

Every switch in `sa2.toml` under "GPU optimisations" produces the same image. Change one at a time, run the same
section of the attract loop, and compare the `[native] ... Swaps` lines and the GPU report lines in the two logs.
For a per-category GPU breakdown set `sa2_gpu_marks_categories = true` (it adds GPU flushes, so compare runs
with the same setting).

## Enhancements (CPU and non-CPU)

Every enhancement has its own key in `sa2.toml` and two master keys (`sa2_cpu_enhancements`,
`sa2_noncpu_enhancements`); see `docs/enhancements.md`. The log shows:

- `[enh] sa2-nx build ...`: which build this is and which enhancements are on;
- `[enh] uses in the last 10 s`: that they ran (the GPU wait also says how long the game thread slept);
- `[hot] ...`: the native replacements switching on. A `[hot] DIFFERENCE` line means one was switched off because it
  did not match the original; please send that log.

The `configs/` folder next to the NRO has premade variants. To use one, copy it over `sa2.toml`.

| Config | What it is for |
|---|---|
| `sa2_cpu_on.toml` / `sa2_cpu_off.toml` | the CPU A/B pair, profiler on in both |
| `sa2_test_deferred_off.toml` | deferred Vulkan recording off (on by default since round 5: the ring thread's vkCmd* calls recorded by a worker thread); profiler on, compare with `sa2_cpu_on.toml` |
| `sa2_test_direct_state_off.toml` | the direct renderer's state records off (round 5): the ring parses every state packet as PM4; profiler on, compare with `sa2_cpu_on.toml` |
| `sa2_test_worker_core2.toml` | the deferred-recording worker on core 2 only (default: the process's default core only, where it preempts the game threads it shares the core with); profiler on, compare with `sa2_cpu_on.toml` |
| `sa2_test_worker_core_any.toml` | the deferred-recording worker on any of cores 0-2; profiler on, compare with `sa2_cpu_on.toml` |
| `sa2_test_main_priority_on.toml` | the drawing game thread one priority step above the other game threads; profiler on, compare with `sa2_cpu_on.toml` |
| `sa2_test_sync_epochs_off.toml` | round 7's dedupe sync epochs off (the old address-only reuse); profiler on, compare with `sa2_cpu_on.toml` for what the correctness fix costs |
| `sa2_test_direct_flush_off.toml` | the direct renderer's step 2 off (round 8): Direct3D's constant, fetch and register-group flushes write PM4 again and the draw hook decodes them; profiler on, compare with `sa2_cpu_on.toml` (game thread) |
| `sa2_test_gpu_round_off.toml` | the GPU round off (round 8): resolves as copies on NVK's copy engine, no blend skip, every duplicate resolve copied; profiler on, compare the "GPU per Swap" lines with `sa2_cpu_on.toml` |
| `sa2_all_on.toml` | every CPU and GPU enhancement on, both master keys said explicitly (the same as `sa2.toml`, MSAA off); profiler on |
| `sa2_test_api_off.toml` | the native Direct3D layer off (2026-10-07): Direct3D's draws and Clear write PM4 again; profiler on, compare with `sa2_cpu_on.toml` (game thread) |
| `sa2_test_gpu_round2_off.toml` | GPU round 2 off (2026-10-07): push constants per draw, the old shader control flow, the per-draw vertex index base and the three per-vertex/pixel tests; profiler on, compare the "C6" and "GPU per Swap" lines with `sa2_cpu_on.toml` |
| `sa2_test_log_writer_off.toml` | the log writer thread off (2026-10-07): the old synchronous file log; profiler on, compare the stutters (frames over 45 ms in `rex_profile.log`) with `sa2_cpu_on.toml` |
| `sa2_test_copies_off.toml` | the 53 unguarded native copies off (they are on by default since round 4); profiler on, compare with `sa2_cpu_on.toml` |
| `sa2_test_upload_uncached.toml` | the upload buffer back in uncached memory (round 3 A/B); profiler on |
| `sa2_test_render_scale_off.toml` | the render resolution off (2026-10-07): 1280x720 docked too, as before; profiler on, compare docked runs with `sa2_cpu_on.toml` (what 1080p costs the GPU) |
| `sa2_test_msaa2.toml` / `sa2_test_msaa4.toml` | 2x / 4x MSAA of the game's render targets (2026-10-07); profiler on, compare with `sa2_cpu_on.toml` (MSAA off) |
| `sa2_test_vsync_relaxed_off.toml` | the old fixed vsync (round 13): a frame that misses 60 waits for the next vblank; profiler on, compare the FPS of heavy stretches with `sa2_cpu_on.toml` |
| `sa2_test_round13_off.toml` | round 13's hand-written vertex loaders, njCalcPoint and njMultiMatrix off; profiler on, compare the game thread with `sa2_cpu_on.toml` |
| `sa2_test_vs_fetch_reorder_off.toml` | the renderer's recognition of reordered vertex shaders off (draws of orders the package lacks are dropped again); profiler on |
| `sa2_all_off.toml` | everything off |
| `sa2_cpu_verify.toml` | every native call checked against the original: slower, and its `[verify]` lines must say `differences: 0` (no profiler) |

Profiler: on in `sa2.toml` and in every config except `sa2_cpu_verify.toml`. That is `switch_profile_stacks = true`,
which writes `logs/rex/rex_profile.log` with where each thread spends its time, and
`sa2_gpu_marks_categories = true`, the GPU time per pass category in the "GPU per Swap" log lines. Every run
pays the same small profiling cost, so A/B runs compare fairly.

For A/B runs send back `logs/` from each run, and play the same stretch at stock clocks (1020 MHz): anything under 60
FPS there is CPU-bound (the game slows down below 60).

## One NRO: optimised codegen

Only `sa2.nro` is delivered, built with the optimised codegen: guest registers kept as C++ locals, guest memory
accessed without `volatile`, and direct calls between functions (docs/enhancements.md, "Build-time"). The first
`[enh]` line of the log names the build. Features are compared with the configs above, not with a second NRO.
