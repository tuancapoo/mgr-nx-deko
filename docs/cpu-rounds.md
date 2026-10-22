# The CPU rounds of October 2026 (version 1.0.1)

What version 1.0.1 changes on the CPU side against 1.0.0: three rounds of changes to the recompiled game code and
the runtime (2026-10-12), a new profile recorded on the console, and one setting of the link (2026-10-13). The
result comes first; after it, each round as it was written when it was finished, with every measurement.

The other change of 1.0.1, the fix for the black speckles in the Chao garden and the kindergarten, is in
[enhancements.md](enhancements.md) under `sa2_native_partial_resolve_inside`.

## Result

**On the console** (the user, 2026-10-13, version 1.0.1 against 1.0.0): performance is better, "I'd approximate by
15%", and the Chao garden's shadow glitch can no longer be triggered. No frame times were recorded: the released
configuration has logging and the profiler off.

Before that test, the estimates were about 5 % of the game thread for the three rounds (from instruction counts and
call frequencies) and 1.5 % for the link setting (GCC's own, weighted by the profile). The new profile also fits
every function again; the released one had lost 30 % of them to the rounds' changes. One build carries all of it,
so nothing separates the contributions.

**The released program, 1.0.0 against 1.0.1** (the Switch ELFs, `tools/arm_cost_census.py` and
`tools/lto_census.py`):

| | 1.0.0 | 1.0.1 |
|---|---|---|
| Game functions that set up a frame pointer | 24,659 | 24 |
| Float-mode writes (`msr fpcr`) in the game code | 32,932 | 24,819 |
| Memory barriers (`dmb`) in the game code | 1,530, one after every guest compare-and-swap | 1,677, all behind the run-time key that puts them back |
| Call sites from game code into other code | 46,768 | 32,289 |
| Executed calls between functions that are still calls, same profile | 4.54 billion with the release's link setting | 2.64 billion |
| Instructions of the game code | 6.12 million | 6.11 million |
| Code (`.text`) | 38.97 MB | 38.93 MB |
| NRO | 55.1 MB | 55.0 MB |

The game code of 1.0.1 was 4.7 % smaller than 1.0.0's before the link inlined more of it (5.83 million
instructions with the release's link setting); the last rows show where that room went.

## What the rounds are

| Round | What it does | Where it is measured below |
|---|---|---|
| 1, ported from sasrt-nx | Fewer host float-mode switches (comparisons and conversions that need no mode, a lookahead before each switch, checks the control flow proves), guest compare-and-swap without the trailing barrier, vector byte reversal and shuffles as NEON instructions, no frame pointer in the game code, fewer wake-up system calls | "The CPU round of 2026-10-12 (sasrt-nx's)" |
| 2 | Loads from the image's read-only data as constants (16,469 sites), the indirect call sites that go to one target as guarded direct calls (81 % of the indirect calls that run), single-precision loads straight into a float register, registers of leaf functions as locals, the trap helper out of line, `sqrt` without the errno check, conversions to integer as the instruction, critical sections that take their owner from the processor block | "The second CPU round" |
| 3 | The registers of call-free loops as locals for the length of the loop (5,767 loops), two and three targets per promoted call site (243 sites, 85 % of the indirect calls that run), the divisions as the instruction (1,978 sites), vector constants, a check of every ARM-only instruction form at each start | "The third CPU round" |
| The link | A new console profile; the inliner's size limit for game code raised from 30 to 64 with the unit's growth capped at 12 %; the localized copies' file compiled like the game code | "Link-time optimisation of a recompiled program" |

Every item has a switch that puts the old form back: a build option (`SA2_GEN_...`, `SA2_SWITCH_...`,
`SA2_GAME_...` in `app/CMakeLists.txt`), a generator option (`app/perf_overrides.toml`) or a run-time key.
Four things that were built stay off: removing the dead prologue saves tinted the game's characters blue (the game
reads a stack word it never wrote), its two sibling elisions are not exact either, and `-fno-trapping-math` could
move a float operation across a mode switch.

## How it was verified

- **Frames.** Lockstep captures of the PC build against a reference, frame for frame by hash: every round's final
  build is identical in every frame except the 53 loading-screen frames whose animation depends on timing. The
  same in verify mode and with the run-time keys off.
- **Self-checks in the PC build.** Each new form runs next to the old one and compares: in one run 81.6 million
  folded constants, 74.3 million promoted calls and 7.8 million critical-section owners, none wrong; 208.8 million
  checks of the native replacements in verify mode, no difference.
- **The ARM forms.** Unit tests on the PC through a model of the NEON instructions (`sdk/tests`,
  `tools/win/run_sdk_tests.ps1`), the Switch compiler's output checked against the claims
  (`tools/arm_forms_check.py`), and at every start of the program 248 results against plain C++ references: on the
  console a failure writes `sa2_selfcheck_failed.txt` next to the program. None was reported from the console.
- **The profile.** All 443 functions of two game files match it, with and without the link's parameters.
- **Every Switch build** passes `tools/check_switch_build.py`.

The sections below were written as each round was finished, before the console test. Where they say that nothing
has run on a console yet, that was true then.

## The CPU round of 2026-10-12 (sasrt-nx's): measured first, ported, checked on the PC

sasrt-nx found 1.4 to 2.1 million host float-mode switches a second in a race (each an `msr fpcr` on the A57) and
built thirteen changes around that and around the lock words, most of them in the generator, the post-codegen tools
and the SDK. The user asked for the same in SA2: measure first, port what sasrt-nx had built and verified, try to
verify what it had not, stop before any NRO. Everything below is on the branch `cpu-round-2026-10-12`; **no NRO was
built from it and nothing here ran on the console.**

### Step 0: the census, on SA2

`sa2_diag_runtime_census = true` (PC builds only; the SDK's `rex/sys_counters.h` counts nothing of it on the Switch)
logs, with every `[enh] uses` report, the real flush-mode writes, the code addresses they come from, and the calls of
the runtime's lock and wait paths. `tools/census_summary.py <log> --capture` turns the lines into per-Swap figures
over the span of a lockstep capture (the same game frames in every run); `tools/census_sites.py <log>` names the
functions.

The released code (the run `sa2_292`, lockstep, 3,000 frames from game frame 6000: title, loading, the City Escape
2-player demo), against what sasrt-nx measured in a race:

| per second, PC | SA2 | sasrt-nx |
|---|---|---|
| flush-mode switches | 80,400 (1,368 per Swap) | 1,400,000-2,100,000 (60,800 per Swap) |
| critical sections entered | 25,700 (437 per Swap) | 85,000 |
| events set / semaphores released | 1,850 / 750 | 12,400 / 6,650 |
| waits | 1,800 | 8,900 |
| handle lookups under the global lock | 7,400 | 31,000 |

So SA2 switches the mode about 45 times less often per frame, and about 60 % of what it does switch comes from
five localized copies: a vector matrix multiply (`n_82140A90`) calling scalar matrix copies (`n_82146E48`,
`n_82146DD8`), and `n_82833568` / `n_82832B78`. SA2's float code is scalar almost everywhere: the generated code
has 40,146 "switch off" checks against 136 "switch on" ones.

### What was ported, and how each was checked

| Change (sasrt-nx's status) | In SA2 | Switch | Checked by |
|---|---|---|---|
| `fp_compares_modeless` (built) | on | `app/perf_overrides.toml` | unit test, lockstep, verify mode |
| `fp_mode_lookahead` (built; its "mode-free" refinement PC only) | on, **without** the bet past a branch (below) | `fp_mode_lookahead`, `fp_mode_lookahead_past_branch = false` | unit test, lockstep, verify mode, the any-mode census |
| `rotate_word_narrow` (PC only) | ported, **left off** | `app/perf_overrides.toml` | exact (unit test, 2.57 million comparisons; lockstep and verify mode passed with it on), but no gain: with the Switch compiler three recompiled files came out 0.07-0.12 % larger (GCC folds the 64-bit form of an `srwi` into one `lsr`, the narrow one into `ror` + `and`) |
| `tools/guest_cas.py`: `stwcx.` without the trailing `dmb` (built) | 592 sites | `SA2_SWITCH_GUEST_CAS`; run time: `sa2_cpu_guest_cas_no_barrier` | PC runs the rewritten expression; Switch compiler: `ldaxr` / `stlxr`, the `dmb` only behind the key |
| The SDK's lock words inline, `__atomic` (atomic_posix.cpp built; inline in `atomic.h` not compiled for ARM) | on | the same run-time key | Switch compiler (`tools/arm_forms_check.py`); every use read (critical sections, spin locks, lists, bit map) |
| `tools/flush_mode_flow.py` (built) | 15,477 of 40,168 checks proven | `SA2_SWITCH_FLUSH_FLOW` | every PC run tests each proof it executes |
| `thread_signal_skip_idle` (built) | on | the SDK key (`thread_signal_skip_idle`) | protocol test, no lost wake-up; Switch compiler |
| Hook call counters in the thread's context (built) | on, for the copies' use counters too | `sa2_cpu_hook_counter_ctx` | lockstep, verify mode, a run with the key off |
| `-fomit-frame-pointer` on the game code (built) | on | `SA2_SWITCH_OMIT_FRAME_POINTER` | Switch compiler (a flag: nothing to run on the PC) |
| `tools/vec_byteswap.py` (PC only) | 6,357 reversals, 1,293 table shuffles | `SA2_SWITCH_VEC_BYTESWAP` | unit test (4 million vectors); Switch compiler |
| `tools/pch_neon_shuffles.py` (PC only, no test) | on | `SA2_SWITCH_NEON_SHUFFLES` | **new** unit test, 10.2 million comparisons over all 256 immediates; Switch compiler |
| NEON forms of vperm, vctsxs, vctuxs (not compiled for ARM) | on | `SA2_SWITCH_NEON_VMX_HELPERS` | unit tests (19 million comparisons; all 2^32 inputs for the small helpers); Switch compiler: one `tbl`, `fcvtzs`, `fcvtzu` |
| `sasrt_cpu_index_no_fingerprint`, `sasrt_cpu_ring_draw_trims` | **not ported** | - | SA2 made no indexed draw in 5.9 million draws of the census run: neither has anything to act on |
| `sasrt_api_thread_stats` | **not ported** | - | SA2 has one drawing thread: the shared words it would move are written by that thread alone |

The "Switch" options are CMake options of the Switch build (`app/CMakeLists.txt`), all on; each OFF defines the
macro that puts the old form back. `configs/sa2_test_cpu_round12_off.toml` (`tools/make_enhancement_configs.py`)
turns the three run-time keys off.

### What SA2 does differently: no any-mode conversions on a bet

sasrt-nx's `fp_mode_lookahead` gives an `lfs` / `stfs` / `frsp` / `fcmpu` its any-mode form (a denormal test, about
four instructions, instead of a mode check per function) in three cases: a vector instruction comes next; VMX mode
is known to be current; or nothing that needs a mode follows before the next branch, in any function with vector
float code and in small "mode-free" functions. The third is a bet on a switch that may come later.

Counted on the PC (`REX_DIET_CENSUS_ON`: plain counters in the any-mode helpers of `rex/ppc/diet_ops.h`, compiled
into PC builds of the game code only), over the capture span:

| generator rule | any-mode sites | executed per Swap | flush-mode switches per Swap |
|---|---|---|---|
| the released code (`fp_moves_modeless` only) | 104 | not counted | 1,368 |
| sasrt-nx's rule as shipped | 8,949 | 93,656 conversions, 404 compares | 1,359 |
| without the bet (`fp_mode_lookahead_past_branch = false`) | 205 | 29,813 conversions | 1,346 |

With the bet SA2 ran 94,000 tests a frame, each about four instructions, to save nine switches a frame in play
(about 180 a frame over the whole run, in the menus): a loss by any plausible cost of an `msr fpcr`. Without it the
any-mode forms stay where they replace a switch for certain. `fp_mode_lookahead_past_branch` is a new generator
option (default true: sasrt-nx's behaviour; `sdk/include/rex/codegen/config.h`). Over the first 150 s of the run
(menus included) the switches per Swap are 928 in the released code, 744 with sasrt-nx's rule and 921 without
the bet.

What is left in SA2 is real alternation: the matrix multiply in vector code calls matrix copies that load and store
sixteen floats through the float registers. A copy that needs no mode (one test per load / store pair, or none) is
candidate 3 of the next round's list.

### Why each is exact

sasrt-nx's arguments, read again against SA2's code:

- **Any-mode helpers:** each returns, in either mode, the bits the plain expression gives with the mode off
  (`sdk/tests/fp_any_mode_test.cpp`). The generator only chooses between two exact forms, so the lists it decides by
  cannot produce a wrong result (they were still compared with SA2's builders: they match).
- **Compare-and-swap without the barrier:** `lwarx` / `stwcx.` order nothing on PowerPC; the game's own `lwsync` /
  `sync` / `eieio` are translated separately. The SDK's lock words (critical sections, spin locks, interlocked
  lists, the bit map) are each read and written only through these atomics, and acquire/release exclusives are what
  every C++ mutex on AArch64 is built from.
- **Flush-mode proofs:** a forward must-dataflow per function with callee summaries; anything that hands `ctx` to
  other code resets it. SA2's addition: `--redo`. A proof depends on which callees are called directly, and
  `tools/direct_calls.py --redo` turns the calls of a newly hooked function back into hooked calls; proofs from
  before would be stale. `tools/post_codegen.py` therefore undoes and proves again after `direct_calls.py`, writing
  only files that change.
- **Skipped notifies:** a waiter is counted under the condition's mutex before the wait releases it, and the
  signaller reads the count together with its state change. The interrupt path, mutants, timers and thread handles
  notify as before.
- **Byte reversal, shuffles, NEON helpers (and the narrow rotates):** identities, each with a test that compares
  the two forms.

### Checks

Unit tests (`tools\win\run_sdk_tests.ps1`, PC, clang; `sdk/tests/`):

| test | result |
|---|---|
| `fp_any_mode_test` | 600,081,920 comparisons, 0 differences; negative control 448,806 differences |
| `rotate_word_test` | 2,568,192 comparisons, 0 differences (and the wrapping masks do need the old form) |
| `signal_skip_idle_test` | 19.7 million signals notified, 3.4 million skipped, no lost wake-up |
| `vec_byteswap_test` | 4,004,081 vectors, 0 differences |
| `vmx_helpers_neon_test` | 19,006,688 comparisons, 0 differences |
| `vmx_small_helpers_test` | all 2^32 inputs of vcfux, 6 million pairs of the others, 0 differences |
| `neon_shuffles_test` (new) | 10,240,512 comparisons, 0 differences |

PC runs of the final build (lockstep, 3,000 frames from game frame 6000):

- **Frames:** all 2,997 frames in common with the baseline run (`r2f_crcensus0`: the released code with the counters) have a pixel-identical frame there: 2,944 at the same position, and the 53 frames of the loading screen one animation step apart (its ring and its Omochao run on the loading thread's timing). Against the previous build of this branch: 3,000 of 3,000 identical.
- **Verify mode** (`sa2_301`): 208,122,035 native replacement checks, 0 differences; the native Direct3D layer compared 3,519,467 records with the original, 0 differences; no `DIFFERENCE` or `MISMATCH` line.
- **Flush-mode proofs:** every PC run tests each proof it executes (`REX_FLUSH_KNOWN_*` keeps its test off the
  Switch and reports to `RexFlushFlowMismatch`); the `[verify] flush-mode flow` line counts them. 508,240,316 in the census run and 1,077,244,266 in the verify run, none wrong.
- **Run-time keys off** (`sa2_cpu_hook_counter_ctx`, `sa2_cpu_guest_cas_no_barrier`): 2,999 of 2,999 frames identical to the default run of the same build (the build before the last regeneration); the log shows the old counters in use.

With the Switch compiler, without building the NRO (devkitA64, the Switch build's own flags from `out/nx`):

- `tools/arm_forms_check.py`: the `__atomic` lock words are `ldaxr` / `stlxr` with their only `dmb` behind the
  key's byte (the legacy forms end in an unconditional `dmb`); vperm is one two-register `tbl` (8 instructions
  against 12), vctsxs `fcvtzs` (2 against 14), vctuxs `fcvtzu` (2 against 29).
- `tools/switch_compile_check.py`: every changed file the PC does not build compiles (the POSIX threading, the
  atomics, the profiler, the kernel's lock paths), and so do the app's hook file with all the regenerated copies,
  the stage module and game files with `-fomit-frame-pointer`.
- `tools/one_file_ab.py` (one recompiled file, each switch alone, instructions / loads / stores):

| `sa2_recomp.16.cpp` | instructions | loads | stores | `msr fpcr` sites |
|---|---|---|---|---|
| every switch off | 42,138 | 7,617 | 7,738 | 211 |
| flush-mode proofs | 41,713 | 7,584 | 7,714 | 173 |
| no frame pointer | 41,654 | 7,483 | 7,574 | 211 |
| NEON VMX helpers | 41,990 | 7,624 | 7,757 | 211 |
| vector byte reversal | 42,120 | 7,614 | 7,740 | 211 |
| compare-and-swap (adds the key's test) | 42,160 | 7,626 | 7,738 | 211 |
| all on | 41,026 (-2.6 %) | 7,438 (-2.4 %) | 7,549 (-2.4 %) | 173 |

`sa2_recomp.64.cpp`, every switch off against all on: 40,050 -> 38,764 instructions (-3.2 %), loads -4.1 %, stores -3.0 %, `msr fpcr` sites 258 -> 196, `tbl` 81 -> 25.

Static, in the game code: flush-mode checks compiled on the Switch 40,282 -> 24,691 (15,477 proven), sites that
always switch 57 -> 39, 592 compare-and-swap sites, 7,650 vector reversals and table shuffles in the new form.

### The profile

The committed profile (`pgo/`) was recorded with the released code. `tools/pgo_fit.py` compiles regenerated files
against it with the mismatch warnings on: of 2,075 functions in nine files, 267 (12.9 %) no longer match (their control flow changed) and 6 are new. GCC still uses the counts of a function whose lines only
moved; a function whose control flow changed is compiled without its profile. The flush-mode proofs and the
generator options account for most of it; `-fomit-frame-pointer` and the SDK changes cost nothing. The PGO build
gave 15 % on the console, so a build from this branch should have a new instrumented session before it replaces the
release.

### Not measurable on the PC

The `dmb` removal (both forms are `lock cmpxchg` on x86), the skipped wake syscalls (the PC does not build that
file), the frame pointer and the NEON forms (Switch code), and what a mode write costs on the A57. The static
counts above and the per-frame counts of the census are what the PC can give: 437 critical sections, 44 signals and
1,368 mode switches a frame are small numbers next to sasrt-nx's, so the expected gain in SA2 is small too: a
few per cent of the game code's instructions (frame pointer, proven checks), not a new frame rate.

### A fix found on the way: the SDK read the port's release tag as its own

`sdk/cmake/rex_version.cmake` takes the SDK's version from `git describe` and stops when the tag it finds is ahead
of the SDK's floor. With the repository's `v1.0.0` tag every configure of a clone failed ("floor version (0.10) is
behind tag version (1.0)"). The tags are now asked for only when the SDK is the root of its repository; inside a
port the version is the floor, the string the released build has. Committed on `main` on its own.

### Tools of this round

`tools/census_summary.py`, `tools/census_sites.py`, `tools/arm_forms_check.py`, `tools/switch_compile_check.py`,
`tools/one_file_ab.py`, `tools/pgo_fit.py`, `tools/arm_cost_census.py`, `tools/arm_got_census.py`,
`tools/generated_code_census.py`, `tools/make_sdk_patch.py`, `tools\win\run_sdk_tests.ps1`, and the four
post-codegen steps (`guest_cas.py`, `flush_mode_flow.py`, `vec_byteswap.py`, `pch_neon_shuffles.py`).

## The second CPU round of 2026-10-12: the fifteen candidates built, more found, everything audited

The user's requests after the round above: build the fifteen candidates listed under "Candidates" below, find and
build more that fit any ReXGlue port, then audit all of it for what could backfire on the A57. Everything here was
verified on the PC build only. Nothing has run on the console, no NRO was built, and the committed profile no longer
fits this code (see "PGO" at the end).

**Result in one paragraph.** With the Switch compiler, three recompiled files come out 2.0 to 3.1 % smaller than after
the first round (2.7 to 3.3 % with the errno flag), with 2.5 to 7.7 % fewer loads. About 21,000 indirect calls a frame
were counted; 78 % of them now go straight to their target behind a compare. Two steps that looked free were not:
one changed the picture (found by the frame comparison, not by any self-check) and one cost more than it saved
(found by the census). Both are switched off.

### The fifteen candidates

| # | Candidate | State | What was measured |
|---|---|---|---|
| 1 | `-fno-math-errno` on the game code | **on** (`SA2_GAME_NO_MATH_ERRNO`) | 0.2 to 0.6 % fewer instructions on top of the rest. `-fno-trapping-math` was split off and is **off** (audit, below) |
| 2 | Loop barriers only where a guest load could be hoisted | **on** (`tools/loop_barriers.py`) | 6,281 of 20,249 loop headers lose their barrier: the loop reads no guest memory and calls nothing, or every way round it passes an indirect call |
| 3 | Registers as locals in leaf functions with loops | **on** (`tools/leaf_locals.py`) | 1,005 functions, 8,526 registers (1,333 only read, not written back). One file: 6.5 % fewer loads |
| 4 | A float copied through a register without a mode | generator option `fp_copy_pairs`, **on where certain**; `fp_copy_only_functions` **off** | Exact for all 2^32 singles (unit test). Where vector mode is current: 71 statements, 1.6 million executions a second, the any-mode conversions drop from 1.32 million to 126,000 a second. In every copy-only function: a loss, see the audit |
| 5 | The cost of one `msr fpcr` on the A57 | built into the profiler's report header ("FPCR on this CPU") | Compiles with the Switch compiler; runs only on the console |
| 6 | Indirect calls with one usual target called directly | **on** (`tools/indirect_calls.py`, `app/indirect_promotions.txt`) | 20,624 sites tagged. PC census of the attract demo: 1.2 to 1.36 million indirect calls a second (about 21,000 a frame), 82 % from sites with one target. 222 sites promoted; calls through the table fall to 283,000 a second. 72.5 million promoted calls checked in one run, 0 wrong |
| 7 | Flush-mode proofs into a function from its direct callers | **on** (`tools/flush_mode_flow.py --image`) | 1,185 functions are entered in a known mode; proven checks 15,477 -> 16,139 of 40,168 |
| 8 | NEON forms for the small VMX helpers | **on** (`rex/ppc/intrinsics.h`) | Ten helpers: 2 to 5 instructions instead of 4 to 41, one of them was 2 already (`tools/arm_forms_check.py`); 0 differences against the x86 forms |
| 9 | `fctidz` as `fcvtzs` and a NaN select | **on** (generator option `fcti_inline`) | 843 sites |
| 10 | `fctid` / `fctiw` round halves to even on ARM | **on** (same option) | 10 sites. A correction of the Switch build: simde's portable conversion rounded halves away from zero. 240 million results against an integer model in the four rounding modes, 0 differences |
| 11 | No address loads from the GOT in the libraries | **not built** | Measured: a forced include with `#pragma GCC visibility push(hidden)` takes one kernel file from 893 to 371 GOT loads. It changes how every library symbol links, which only a linked and run NRO can validate |
| 12 | Critical sections without the thread lookup | **on** (SDK key `kernel_critsec_pcr_owner`) | 28,000 enters a second in SA2. The owner comes from the guest context (r13 -> KPCR -> current thread); 8.3 million calls compared with the lookup in one run, 0 wrong. In the Switch object the enter path has no call left |
| 13 | One signature table of the C runtime for all ports | tool built (`tools/crt_signatures.py`, `tools/crt_signatures.json`) | SA2's `memcpy`, `memmove` and `memset` are found at one address each in another game's image (Banjo-Kazooie) |
| 14 | LTO and PGO for the SDK's libraries | **not built** | Confirmed: the SDK is compiled without `-flto` (177 of the Switch build's 1,253 compile commands have it: the game code and the app). A link-time change: needs an NRO build |
| 15 | Hot words of the shared constant block as push constants | **not built** | It changes the translated shaders, so the shader library the installer builds; a decision for the user, not a CPU step |

### More that was found while building them

| Step | State | What was measured |
|---|---|---|
| Loads from the image's read-only data as constants (`tools/ro_constants.py`) | **on** | 14,297 float, 792 double and 1,380 integer loads folded (the address is built from `lis` / `addi` constants and lies in `.rdata`). The PC build still loads and compares: 59.7 million in one run, 0 wrong |
| A single-precision load straight into a float register (`tools/float_loads.py`) | **on** | 72,124 loads: `ldr s; rev32; fcvt` instead of `ldr w; rev; fmov; fcvt` (the general-to-float move is the slow step on the A57) |
| The trap helper out of line and cold (`tools/pch_cold_traps.py`) | **on** | 1,219 trap sites (mostly the guards in front of divisions) no longer carry logging code inline. A layout change: -0.4 / -0.8 / +0.25 % instructions |
| `dcbt` as the host's prefetch (generator option `dcbt_prefetch`) | built, **off** | 217 sites, no measurement on the console |
| No link-register moves in prologues (`tools/frame_traffic.py`) | marked, **off** | 9,880 `mflr` with their saves, 10,682 `mtlr`. Passed the frame comparison, but see the audit |
| No prologue saves of non-volatile locals (same tool) | marked, **off: NOT exact** | 14,979 saves. Changed the picture, see the audit |
| No back-chain stores (same tool) | marked, **off** | 22,597 stores |
| The stack pointer as a C++ local | measured and **dropped** | 1.3 to 1.6 % MORE instructions: a local that lives across a call needs a callee-saved host register |
| Pointer translation without a heap lookup (SDK, off Windows) | **on** | `Memory::TranslateVirtual` called `LookupHeap` for an offset that is 0 on every platform but Windows and macOS on ARM. 318 call sites; 5,700 a second in SA2 |
| Object reference counts inline (SDK) | **on** | `XObject::Retain` / `Release`: 577 call sites; 14,700 a second in SA2 |

Checked and nothing to gain: the Switch compiler already expands atomics inline (no `-moutline-atomics` helpers), and
no indirect call of SA2 leaves the main image (0 % go through the cross-module resolver).

### The audit: what could backfire, and what was changed because of it

1. **Removing the dead saves of non-volatile registers changes the picture. Off, and opt-in in the tool.** With
   `non_volatile_as_local` these saves write zeros (the callee's fresh locals) into the frame. The game reads a stack
   word it never initialised: zero is harmless, stale data is not. In the two-player demo Sonic's and Shadow's red
   parts came out purple and white road lines blue (`out/pc/captures/broken_r2/`, the user spotted the tint).
   Every self-check passed; only the lockstep frame comparison showed it. The released build depends on those zeros
   too. The reader has not been found yet.
2. **The link-register half is off as well.** It passed the same comparison, but it changes stale frame contents in
   9,880 functions in the same way, and one demo scene is not enough after item 1.
3. **`-fno-trapping-math` is off.** It lets the compiler run a float operation earlier than the code does (out of a
   branch or a loop). The recompiled code changes the host's flush mode in between, which the compiler does not know
   a float operation depends on. It bought at most 0.5 % of one file. sasrt-nx and masseffect-nx ship it.
4. **The float copy forms in every copy-only function are off.** They removed 24 % of the mode switches (92,300 ->
   70,000 a second) but ran 5.25 million times a second to do it: 235 extra tests per switch saved. SA2's matrix
   copies run about 1,500 times a frame and only about one call in six comes from vector code. The same bet lost in the
   first round (`fp_mode_lookahead_past_branch`). The number that would decide it is candidate 5's.
5. **Promoted calls and loop barriers.** A loop needs no barrier when every way round it passes an indirect call. A
   promoted call is no longer one: the compiler can inline its target. So `tools/indirect_calls.py` now runs before
   `tools/loop_barriers.py`, and 23 loops got their barrier back.
6. **Promoted calls otherwise.** The guard compares the target, so a site that goes elsewhere is still correct and
   pays one compare. The list comes from one scene (the attract demo); a census of more scenes would make it better,
   not safer. `SA2_GEN_PROMOTED_CALLS=OFF` makes every guard false.
7. **Read-only constants** assume nothing writes the image's `.rdata`. No source of the app does (it only reads
   constants there), and every PC run compares each folded load with memory. A future patch that writes there would
   show up as a `DIFFERENCE` line on the PC, not on the console.
8. **Leaf locals** load every register at entry and store the changed ones at each return. That pays when the loop
   runs at least once; a leaf that usually skips its loop does a little more than before. Only leaves that still
   have a barrier are taken.
9. **The NEON `vcfux`** rounds in the current mode like the x86 form, but the two differ in rare cases when the
   guest has set a mode other than round-to-nearest. The 360 always rounds vector conversions to nearest, so neither
   was exact there.
10. **Entry modes, critical section owners, pointer translation**: each has its check. The first two are compared
    on every execution in the PC build; the third aborts at start-up if a heap's offset disagrees with the constant.
11. **The post-codegen sequence is idempotent**: a second run on a processed tree changes no file (compared by
    hash). It was not at first: the barrier step decided differently on text the later steps had rewritten.

### Verification (PC, final build)

- **Unit tests** (`tools\win\run_sdk_tests.ps1`): 9 of 9. New: `sdk/tests/fcti_copy_test.cpp` (17.4 billion checks,
  0 differences; its negative control finds 16,777,214) and `sdk/tests/float_loads_test.cpp` (the vector form of a
  float load against the integer form, all 2^32 patterns).
- **Lockstep capture** against the first round's reference (`r2f_crcensus0`): 2,942 of 2,995 frames identical in
  place; the other 53 are the known loading-screen frames. The same with the run-time keys off.
- **Verify mode**: 208 million native checks, 0 differences; 1,938 of 1,991 frames identical plus the same 53.
- **Self-checks of one run**: 59.7 million folded constants, 72.5 million promoted calls, 8.3 million critical
  section owners, 514 million flush-mode proofs; 0 wrong.
- **Switch compiler**: every changed SDK and app source and four generated files compile
  (`tools/switch_compile_check.py`); `tools/arm_forms_check.py`: all claims hold.

### Switches

| Kind | Name | Default |
|---|---|---|
| Generator (`app/perf_overrides.toml`) | `fcti_inline`, `fp_copy_pairs` | set |
| Generator | `fp_copy_only_functions`, `dcbt_prefetch` | not set |
| Build (`SA2_GEN_*`, every platform) | `RO_CONSTANTS`, `FLOAT_LOADS`, `LEAF_LOCALS`, `COLD_TRAPS`, `PROMOTED_CALLS` | ON |
| Build | `SA2_GEN_LR_MOVES_ELIDE`, `SA2_GEN_NV_SAVES_ELIDE`, `SA2_GEN_BACK_CHAIN_ELIDE` | OFF (not exact) |
| Build | `SA2_GAME_NO_MATH_ERRNO` / `SA2_GAME_NO_TRAPPING_MATH` | ON / OFF |
| Run time (SDK) | `kernel_critsec_pcr_owner` | true |
| Run time (PC diagnostic) | `sa2_diag_indirect_census = "<file>"` | empty |

The promotion list is made with `python tools/indirect_promotions.py --census <file> --gen app/generated/default`.

### PGO

`tools/pgo_fit.py` on three files: 29.7 % of the functions lose the committed profile (12.9 % after the first round).
A release build from this code needs a new instrumented session first; without one it may be slower than v1.0.0.

## Fifteen more candidates, mostly in the SDK (2026-10-12; found, two built)

The user's next request: look at the SDK itself. "Sites" are call sites in the 138 Switch objects of the SDK's
kernel, system and core sources (`objdump -r`). "A second" is the PC census of SA2's attract demo at 58 frames a
second (`sa2_diag_runtime_census`, new line "runtime overhead"). In SA2 all of these are small: its game thread makes
a few hundred kernel calls a frame. They weigh more in a port with more kernel traffic (sasrt-nx enters 85,000
critical sections a second, three times SA2).

| # | Candidate | Sites | A second in SA2 | State |
|---|---|---|---|---|
| 1 | Every log statement calls `rex::GetLoggerRaw` out of line before it tests its level, written or not. An inline level gate in front | 3,012 (plus 245 category accessors) | 1,360 | not built |
| 2 | `Memory::TranslateVirtual` looked a heap up for an offset that is 0 off Windows | 318 | 5,700 | **built** |
| 3 | `XObject::Retain` / `Release` out of line | 577 | 14,700 | **built** |
| 4 | `rex::system::kernel_state()`: an out-of-line call that returns one pointer | 378 | 12,000 | not built (a DLL data boundary on Windows) |
| 5 | A setting read (`REXCVAR_GET`) is a call that returns a reference to a function-local static | 319 over 63 settings | not counted | built for the critical sections only |
| 6 | Handle lookups take the global recursive mutex and retain the object | 72 | 7,400 (16 to 28 find the lock taken) | not built (a concurrency change) |
| 7 | Waits and events look their host object up from the guest dispatcher header | - | 3,200 | not built |
| 8 | The guest clock behind a mutex | - | 2,700 | not built |
| 9 | Thread-local reads are calls on the Switch (`__aarch64_read_tp`); kernel exports ask for the current thread and thread state that way. Hand them the guest context instead, as the critical sections now get | 51 + 48 + 23 | not counted | built for the critical sections only |
| 10 | Every kernel export goes through a `noinline` shim and, when it returns a value, a compiler barrier that spills everything | 1,363 exports | about 1,100 a frame | not built |
| 11 | The profiler's counters are out-of-line calls on the Switch even with the report off (one per handle lookup, per clock read) | 18 | not counted | not built |
| 12 | Sites with two or three usual targets: a chain of guards. 18 % of the calls came from sites with several targets (the largest, `sub_8214B520`, the render-state table, has more than four and stays indirect) | - | 283,000 indirect calls left | not built |
| 13 | Signed division is written `(d && !(n == INT32_MIN && d == -1)) ? n / d : 0`; AArch64's `sdiv` already answers 0 for a zero divisor, so one of the two tests can go | 3,643 in the game code | not counted | not built |
| 14 | A host call into guest code zero-fills a whole `PPCContext` first (its 128 vector registers alone are 2 KB) | - | 117 | not built |
| 15 | Each log statement expands to a formatted call inside a try / catch in the function that logs | 2,271 handler sites | - | not built (code layout) |

## The third CPU round of 2026-10-12: searched, built and audited; six items kept, three dropped

The user's request: search, build and audit another fifteen improvements that fit any ReXGlue port. Fifteen that hold
up were not there to find in one pass. What follows is everything that was tried: six items kept, three measuring
tools, three experiments dropped with their numbers, and the audit. PC-verified only, like the rounds above.

### Kept

| Item | Where | What was measured |
|---|---|---|
| Guest registers as C++ locals for the length of a call-free loop | `tools/loop_locals.py` (`SA2_GEN_LOOP_LOCALS`) | 5,767 loop regions in 3,044 functions, 25,967 registers (5,283 only read). In `sub_8286ADC8`'s loop the seven loads and stores of ctx per iteration are gone and the body is 33 instructions instead of about 45. The barrier stays. Whole files: instructions unchanged, 0.8 to 1.6 % fewer loads |
| Promoted calls with two or three targets | `tools/indirect_calls.py`, `tools/indirect_promotions.py` | 243 sites (21 with several targets): the guards catch 84.9 % of the census's indirect calls, up from 80.6 %. 74.3 million promoted calls checked in one run, 0 wrong |
| The integer divisions as the instruction | generator option `div_inline`, `rex::ppc::divw` ... | 1,978 sites. With the Switch compiler: `divwu` 2 instructions instead of 4, `divw` 6 instead of 9, no branch; a constant divisor still becomes a multiplication |
| Vector loads from read-only data as constants | `tools/ro_constants.py` (`REX_RO_V128`) | 94 `lvx` sites; they ran 22 million times in one PC run (about 1,300 a frame). A literal instead of a guest load, REV64 and EXT |
| `kernel_state()` inline off Windows | `rex/system/kernel_state.h` | 378 call sites in the runtime, 12,000 calls a second in SA2. The PC build keeps the function (the pointer is in the runtime DLL), so this one is compile-checked only |
| Every start checks the forms only one processor has | `app/src/cpu/sa2_forms_selfcheck.cpp` | 248 results (the conversions to integer, the divisions, the float copies, twelve vector helpers, the two saturating conversions) against plain C++ references; one `[enh] instruction forms self-check` line. On the PC it proves the expected values; on the console it is the first time the AArch64 forms run. Not a speed item: the idea is banjo-nx's |

### Measuring tools that came out of it

- `tools/loop_body_census.py`: instructions, loads and stores inside loops of an AArch64 object (a file's size cannot
  judge a change inside loops).
- A census of calls per kernel export (`rex/hook.h` counts every `REX_EXPORT` in PC builds; `[census] kernel exports
  by calls`). SA2's attract demo: `RtlLeaveCriticalSection` 28,000 a second, then nothing above 900 a second
  (`NtWaitForSingleObjectEx`, `NtSetEvent`, `KeSetEvent`, `KeTlsGetValue` ...). Beyond the critical sections the
  kernel side has nothing left to win in this game.
- CPU cycles per thread over a lockstep capture (`[capture] CPU over the N recorded frames`). Two identical runs
  differed by 13 % on the game thread, so it shows proportions (the renderer's command thread costs about three times
  the game thread on the PC), not a step's gain.

### Tried and dropped

All three wanted the same thing as the first item: a loop barrier that does not force every ctx register through
memory on each iteration.

| Experiment | Result with the Switch compiler | Why it was dropped |
|---|---|---|
| The loop's own loads as volatile reads instead of a barrier | +1.2 / +2.3 / +3.0 % instructions, +2.0 to +5.9 % loads | A volatile load cannot take its value from a store of the same iteration, and the game stores an integer to the stack and loads it back as a float all the time |
| The guest-memory base pointer made opaque at the loop header | +3.4 / +4.5 / +3.3 % instructions, +5.8 to +7.9 % loads | The compiler then no longer believes that guest stores leave ctx alone and reloads ctx after every one of them |
| An opaque zero added to the base pointer at the loop header | +1.3 / +3.5 / +1.1 % instructions; in one loop 7 loads and stores of ctx per iteration became 1 | It works as intended in the loop looked at, but the files grow, the gain cannot be shown without the console, and it changes the one mechanism that keeps waiting loops from hanging. The first item reaches the same loops and leaves that mechanism alone |

### The audit

1. **Loop locals load every register the loop mentions on the way in.** A loop that usually runs once loads a few
   registers it then does not use. Half of the regions carry at most 4 registers, 90 % at most 8, none more than
   22, so that cost is small and bounded; from the second iteration on the step only saves. Every `goto` keeps its
   plain shape (jumps across a region's border go through stubs), which the other tools depend on: without that a
   second run of the post-codegen sequence lost flush-mode proofs and added barriers.
2. **The divisions use inline assembly**, which hides them from the compiler: it cannot merge two equal divisions
   any more. A divisor known at compile time keeps the C expression for that reason.
3. **`kernel_state()` inline has not run anywhere yet**: the PC keeps the old form. It reads the same pointer.
4. **A second or third guard** costs one compare more on the calls that match none (under 0.5 % of a promoted
   site's calls by the rule that selects them).
5. **Vector constants** rest on the same assumption as the scalar ones (nothing writes the image's `.rdata`); the PC
   build compares each one with memory every time it runs.
6. **The forms self-check** can only raise a false alarm, never hide a fault; its expected values are proven by
   the PC run of the same table.

### Verification (PC, the round's final build)

- Lockstep capture against `r2f_crcensus0`: 2,926 of 2,979 frames identical in place, the other 53 the known
  loading frames; the same in verify mode (1,943 of 1,996) and with the run-time keys off (2,930 of 2,983).
- Verify mode: 208.8 million native checks, 0 differences. Self-checks of one run: 81.6 million folded constants,
  74.3 million promoted calls, 7.8 million critical section owners, 0 wrong. Forms self-check: 248 of 248.
- The post-codegen sequence run twice leaves the tree unchanged (compared by hash).
- Switch compiler: the changed and new sources and four generated files compile; `tools/arm_forms_check.py` holds
  (now also the divisions); no `fcvtas` in the generated objects (the old rounding, as banjo-nx checks it).

Of the SDK list above, this round built items 4 (`kernel_state()`), 12 (several targets per site) and 13 (the
divisions).

## Link-time optimisation of a recompiled program: what the release's link did and did not do (2026-10-12)

The user's question: can LTO be improved for ReXGlue ports in general? Measured on the released v1.0.0 ELF (LTO and
PGO, `out/nx/elf/sa2_v1.0.0_pgo_use_20261008_0105.elf`) and with the Switch compiler (GCC 16.1). Nothing here has
changed a build yet; the last part says why that is the right order.

The LTO unit is the game code and the app: 177 of the Switch build's 1,253 compile commands. The SDK, the
third-party libraries, the stage module (127 functions, 55 KB) and the driver stay outside.

| Question | Measured | Conclusion |
|---|---|---|
| Does the inliner take the small functions? | 110,826 direct call sites are left between game functions. 2.0 % of them call a function of 32 instructions or fewer, 14.1 % one of 64 or fewer, 46 % one of 128 or fewer | Yes. What stays out of line is medium and large |
| What about functions called from one place? | 8,579 of the 30,766 game functions have one direct call statement in the generated code and no address anywhere in the image (`tools/address_taken_census.py`). In the ELF, 7,326 functions with one call site left hold 28.5 % of the game code's instructions | In an ordinary program GCC inlines a function that is called once and drops its body (`-finline-functions-called-once`). It needs the function to be unreachable otherwise, and the function table takes the address of every function, so the rule never applies. These functions meet the limit for any other function instead (`max-inline-insns-auto` = 30) |
| Are functions split into a hot and a cold part? | No: 0 `.cold` parts in the release. `-freorder-blocks-and-partition` is off by default on AArch64. Tried on two game files: without a profile 2 cold parts (248 bytes); with the committed profile 4 parts in file 16 (328 bytes of 179 KB), and file 64 does not assemble (`invalid operands (.text.unlikely.X and .text.X sections) for '-'`: a jump table whose targets were split off) | Dropped: it breaks with this compiler and moves next to nothing |
| Do the interprocedural passes specialise functions? | 437 `.constprop`, 1,083 `.isra` and 218 `.part` clones in the program, 24 of them of game functions | Not in the game code: every game function has the same two parameters and all of its state in memory, so there is nothing to specialise |
| What crosses the border of the LTO unit? | From game code: the critical sections (2,220 call sites into one-instruction thunks), the cold fallback of an indirect call (22,866 sites), and in the release the logging inside every trap site (round 2 moved that out of line) | In SA2 the kernel is rarely called (the third round's census: only the critical sections are frequent), so putting the SDK into the unit would win little here. A port with more kernel traffic should measure it before deciding |
| How is the link partitioned? | 512 LTRANS partitions (GCC 16's default). `-fipa-ra` only helps a call whose callee was compiled earlier in the same partition, and `-fipa-pta` runs per partition | A link-time experiment (fewer, larger partitions); game functions push 2.99 callee-saved registers on average |

### The order of work

GCC inserts the profile's counters before its interprocedural inliner runs, and matches a function with its profile
by the function's own control flow. So:

- what is decided at the link can still change after a profile is recorded, without losing it: the inliner's
  limits, the partitions, which functions the table lists;
- what must be final before the console session is the source (every round above) and the set of instrumented
  objects.

The instrumented build of this branch therefore keeps the release's LTO settings, and the items below wait for the
new profile, whose counts can then judge them (`-fdump-ipa-inline` at the link lists every decision with the
call site's count).

### Candidates for the build that uses the new profile

1. **A higher `max-inline-insns-auto` for the game code** (a compile option of the game files: GCC keeps its
   parameters per function). With a profile the inliner takes the hottest call sites first and leaves cold ones
   alone, so this reaches the hot medium-sized functions the limit of 30 keeps out, including the hot ones that are
   called once. Semantics cannot change; the cost is code size, bounded by `inline-unit-growth`.
2. **Fewer, larger partitions**, for `-fipa-ra`.
3. **A function table without the functions nobody takes the address of.** The largest lever (the called-once rule
   and smaller code), and the only one that can break the game: a function reached by an address the analysis
   missed ends in the invalid-function trap. The loaded image has no relocation table to make the list exact (its
   `.reloc` range holds other data), so this needs a conservative address scan and a PC census proving that every
   indirect target of long runs is on the list. Not started.

GCC 16.1 also has `-flto-incremental=<folder>` (with `-flto-partition=cache`): unchanged partitions are reused
from one link to the next, which would shorten the A/B builds. Not for a release build.

Tools: `tools/lto_census.py` (what a link left in an ELF) and `tools/address_taken_census.py`.

### The link with the new profile (2026-10-13): the inline limit of the game code raised to 64

The user played the instrumented build of this branch and sent its profile (one session; 6,862 of the game's 33,303
functions ran, the release's session ran 6,506). The first build from it kept the release's link settings and wrote
the inliner's dump (`SA2_SWITCH_LTO_DUMP`), which ends with the reason every remaining call site was refused and how
often those calls ran in the session:

| Reason | Call sites | Calls in the session |
|---|---|---|
| inlined | | 22.5 billion |
| `max-inline-insns-auto` limit reached | 105,317 | 3.88 billion |
| `max-inline-insns-single` limit reached (functions declared inline: the app's) | 466 | 0.32 billion |
| `large-function-growth` limit reached | 563 | 0.24 billion |
| optimization level attribute mismatch | 25 | 0.05 billion |
| the others together | | 0.06 billion |

So one limit refuses 86 % of the executed calls that are still calls: the size limit for functions not declared
`inline` (30). In hand-written code that keyword carries the author's judgement; in recompiled code no function has
it, so the limit is the default nobody chose. With a profile the inliner only grows hot call sites (a cold one is
refused as "call is unlikely and code size would grow"), so raising it adds code where the session ran.

Five builds from the same profile (`tools/lto_inline_report.py`; the time estimate is GCC's own, weighted by the
profile, and knows nothing about caches; the hot functions are the ones GCC placed in `.text.hot`):

| Build | Limit / unit growth | Executed calls left | Time estimate | Hot functions | `.text` | NRO |
|---|---|---|---|---|---|---|
| A: the release's settings | 30 / 40 % | 4.54 billion | baseline | 1.72 MB | 37.8 MB | 53.8 MB |
| A2: A and the two fixes below | 30 / 40 % | 4.51 billion | -0.04 % | 1.76 MB | 37.8 MB | 53.8 MB |
| B | 200 / 40 % | 2.03 billion | -2.1 % | 2.46 MB | 45.6 MB | 61.7 MB |
| C: with the fixes | 200 / 12 % | 1.89 billion | -2.2 % | 2.51 MB | 38.9 MB | 54.9 MB |
| **D: with the fixes (kept)** | **64 / 12 %** | **2.65 billion** | **-1.5 %** | **2.06 MB** | **38.9 MB** | **55.0 MB** |

- **B** shows the gain and two costs. The whole unit grew to the 40 per cent the compiler allows, most of it in code
  the session never ran (with `-fprofile-partial-training` that code is optimised as without a profile, and the
  higher limit applies to it too). And the hot functions grew by 46 %.
- **C** caps the unit's growth at 12 % (`SA2_SWITCH_INLINE_UNIT_GROWTH`). The inliner takes the best call sites
  first (the most calls for the least growth), so the cap only cut the cold growth: the NRO is 2 % larger than A,
  the hot functions as large as in B. What they grew by, against A2: 30 % of the added size is callees that now
  have one copy (the function moved into its only hot caller: nothing is executed twice), half is callees with
  four or more copies, among them `sub_82544128` (about 2.3 KB) in 35 places and `sub_82546268` in 34. A function
  that ran from one address and stayed in the instruction cache then runs from 35.
- **D** keeps the call sites where the call itself is a large part of the work: 41 % fewer executed calls than A2
  for 17 % more hot code. The step from D to C buys another 0.7 % of the estimate with 0.45 MB of hot code, most of
  it the large multi-copy callees, which is the part a 48 KB instruction cache and a 2 MB second-level cache
  shared by four cores cannot be assumed to take. 64 and 12 are the defaults of a build that uses a profile.

Two fixes for the localized copies and native replacements (`app/src/cpu/sa2_hot_guest.cpp`, the hottest game code,
compiled as part of the app), found on the way:

- **The errno rule.** GCC does not inline between two functions that both compute with floats and differ in
  `-fmath-errno`. The game code has `-fno-math-errno` since the second round and this file did not, so the native
  replacements and the game functions that call them stayed apart at the link: the "attribute mismatch" row above.
  The file gets the flag (it calls no function that sets errno, so its own code is unchanged).
- **The frame pointer.** The first round's `-fomit-frame-pointer` covered the generated files only; this file gets
  it too.

Verified: the profile fits with and without the parameters (`tools/pgo_fit.py`: 0 of 443 functions lose it), every
build passes `tools/check_switch_build.py` (which now follows a known guest halt through a chain of inlined calls:
with the higher limit `sub_828C5D60` inlines `sub_828D71E8`, which inlines the halt of `sub_828DB988`). Not
measured: the console. The session's length is not recorded, so the counts above cannot be turned into calls per
frame; at 20,000 to 70,000 frames, D removes roughly 27,000 to 93,000 executed calls a frame over all threads.

Not tried: fewer partitions (`SA2_SWITCH_LTO_PARTITIONS`) and the pruned function table.

## Fifteen CPU candidates for any ReXGlue port (2026-10-12; as found. Built since: see the second round)

The user's request after the round above. Each has what was counted for it in SA2, on the PC or with the Switch
compiler; none has run on the console. "Sites" are places in the generated code (`tools/generated_code_census.py` counts them for any port), "per
Swap" comes from the census run. Order: what the numbers promise against the work.

| # | Candidate | Where | What was counted | Exactness / check |
|---|---|---|---|---|
| 1 | `-fno-math-errno -fno-trapping-math` on the game code. sasrt-nx and masseffect-nx ship both; sa2-nx and nfsmw-nx do not | build flag | 1,223 `sqrt` calls in 667 game functions of the released ELF (a compare and a branch to the library after every `fsqrt`). Three files with the Switch compiler: -0.3 / -0.6 / -1.4 % instructions, -1.9 % at best with both | Values unchanged: nothing reads `errno` or the host's float flags. Lockstep and verify mode |
| 2 | Loop barriers only where a guest load could be hoisted | `tools/loop_barriers.py` | 20,249 barriers. 1,653 loops read no guest memory at all, and 4,232 contain a call the compiler cannot see through (redundant where every path back to the loop header passes one): up to 29 % protect nothing. A barrier clobbers all memory, so every register still in `ctx` is stored and reloaded on each iteration | The barrier's own argument, per loop; `tools/find_self_loops.py` on the ELF as today |
| 3 | Argument registers (r3-r10, f1-f13, v0-v13) as locals in functions with loops | generator | The 9,012 functions with a barrier hold 56 % of the 1,503,601 uses of those registers; 4,881 functions are leaves. The localized copies do this by hand for about 50 functions. masseffect-nx measured full arguments-in-registers as no gain on top of locals, but it has no loop barriers | Exact by construction, like `tools/localize_native.py`; lockstep |
| 4 | A float load and store of the same register as a copy that needs no mode | generator | 40 % of SA2's remaining mode switches: a vector matrix multiply calling scalar matrix copies (`n_82140A90` 21.7 %, `n_82146E48` 13.2 %, `n_82146DD8` 5.6 %). 2,399 such adjacent pairs in the code | One denormal test per pair gives the released bits; with the test dropped a signalling NaN is copied unchanged, as on the 360. Needs its own unit test |
| 5 | The cost of one `msr fpcr` on the A57, measured at start and logged | profiler | sasrt-nx's lookahead threshold and candidate 4 both hang on this number, which nobody has | A timing line, no behaviour |
| 6 | Indirect calls with one usual target called directly | post-codegen, from a census | 21,365 indirect call sites, 16 % of all call sites (range check, table load, null check, `blr`). How often each runs and where to is not counted yet: the flush census's site table can count it. GCC's own value profiles would do the same in the PGO session; they are off there because their updates are not atomic | The guard compares the target; the slow path is today's code |
| 7 | Flush-mode proofs across calls into a function (its entry mode from its direct callers) | `tools/flush_mode_flow.py` | 24,691 checks left in 7,684 functions; at least one per function is "unknown at entry". 55,097 proven checks run per Swap already; the unproven ones are not counted yet | The PC self-check tests every proof it executes |
| 8 | NEON forms for the small VMX helpers | `rex/ppc/intrinsics.h` | With the Switch compiler: vcfux 10 instructions (`ucvtf` is one), `simde_mm_sllv_epi8` 40, `simde_mm_cvttps_epi32` 7, `simde_mm_dp_ps` 4-8. SA2 uses them 33, 2, 4 and 34 times; vector-heavy games more | sasrt-nx's `vmx_small_helpers_test.cpp` already reports 0 differences for five of them |
| 9 | `fctidz` as `fcvtzs` and a NaN select, like `fctiwz_inline` | generator | 843 sites in SA2, each an `isnan` test, a range compare and a select around the conversion | The same argument as `fctiwz_inline`; a unit test over the edge values |
| 10 | `fctid` / `fctiw` on ARM round halves away from zero (**a difference from the PC build and the 360, not a speed item**) | generator / simde | simde's non-x86 `simde_mm_cvtsd_si64` is C `round()`; x86 and PowerPC round to even. 10 sites in SA2 | `frinti` + `fcvtzs`; a unit test of the ties |
| 11 | No address loads from the GOT in the libraries outside the LTO link | build (hidden visibility on declarations) | 38,528 loads of an address from the GOT in the released ELF: 3,555 in game functions, 34,973 in the SDK, the renderer and the driver; 5,685 slots | Addressing only |
| 12 | Critical sections without the thread lookup | SDK | `RtlEnterCriticalSection` calls `XThread::GetCurrentThread()` (out of line, a thread-pointer call) for the owner id that `ctx.r13` already gives, as the spin locks do. 437 enters per Swap in SA2, 85,000 a second in sasrt-nx | The same id from another place; an assert in the PC build |
| 13 | One signature table of the C runtime and math functions for all ports | tools | Every port finds `memcpy`, `memset`, `sin`... by hand. SA2's `memcpy` hook takes about 400,000 calls a second, `sin` and `cos` 19,000 each | The existing guard (native against original) |
| 14 | LTO and PGO for the SDK's runtime libraries too | build | A kernel call is `__imp__X` -> `HostToGuestFunction<>` -> `X_entry` -> its helpers, each a real call. 2,140 import call sites, about 1,100 counted runtime calls per Swap in SA2 | No source change |
| 15 | The four hot words of the shared constant block as push constants (the renderer all masseffect-nx-based ports share) | renderer | The shared block changes on 90 % of draws, mostly in words 0, 48, 90 and 91; 0.92 binds of set 4 per draw at about 1.3 µs each on the console: about 1.3 ms per frame of the ring thread in play | Pixel-identical: lockstep capture |

For SA2 itself the first three and 6 are the ones with weight: its game thread is drawing code in loops, and the
kernel traffic (12, 14) and the mode switches (4, 5, 7) are small here. For sasrt-nx the order is the other way
round.

