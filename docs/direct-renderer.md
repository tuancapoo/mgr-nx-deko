# The direct renderer: removing the ring renderer's emulation, step by step (sa2-nx, round 5)

The other ports (futari-nx, banjo-nx, peggle2-nx, transformed-nx) run the same native ring renderer (`src/me`, from
masseffect-nx). This is the plan SA2 follows to take the emulation out of it, and what each step needs from a game.

## Why

- **SA2 at 1020 MHz, round 4 (console):** the ring thread took about 22 ms a frame (19.5 µs a draw, about 1,200
  draws a frame), and the game thread about 14 ms. 60 FPS needs both under 16.7 ms.
- **The target:** 8-10 ms of ring thread a frame, about 7 µs a draw. That is MarathonRecomp-NX's API-level cost, which
  `docs/api-renderer-assessment.md` measured.
- **Where the ring's time went** (round 4 console profile, self samples):

  | Part | Share | Handled by |
  |---|---|---|
  | PM4 parsing (`Packet`, `Type3`, register runs) | about 21 % | step 1 |
  | EDRAM emulation (`PrepareDrawEDRAM4`, sync, publish) | about 19 % | step 1b memo |
  | Vulkan recording (`nvk_*`, `vk_*`) | about 15 % | deferred recording |
  | Vertex and constant fingerprints, dedupe, copies (XXH3, `Equal`) | about 14 % | step 3 |
  | Shader identity and pairing | about 6 % | step 3 |

- **The approach:** keep the back end (shader translation, pipelines, textures, Vulkan) and remove the emulation in
  front of it, one layer at a time. Every step is pixel-identical by construction, checks itself, and has its own key.
  That is cheaper and safer than UnleashedRecomp's rewrite of the whole Direct3D layer.

## Step 1 (done in SA2): the draw state without PM4, `sa2_cpu_direct_state`

### What it does

- **On the game thread:** the hooks of the Draw* functions call `BeginDrawDirect` before the original and
  `EndDrawDirect` right after it (`src/me/sa2_me_hooks.cpp`). `EndDrawDirect` (`src/me/me_native_system.cpp`) decodes
  the packets the call wrote before its draw packet into a host record (`namespace direct`):
  - register runs in host byte order;
  - type-1 pairs;
  - the type-3 packets the ring handles, as they were.

  It then covers the region with one NOP packet: the header, `'SA2D'` and the record's number. The draw packet stays
  where it was, and so does every address Direct3D keeps. The records live in an 8 MB ring of host memory, and the
  decode writes them there directly.
- **On the ring thread:** `Type3`'s NOP case calls `ApplyDirectRecord`. It applies the record with the same register
  writes the packets would have made, at the same place in the stream:
  - register runs through `WriteConstantRunHost`, which is `WriteConstantRun` a part at a time with memcmp/memcpy;
  - single-register runs and type-1 pairs through `WriteRegister`;
  - type-3 packets through `Type3`, with their original words and physical address.
- **Concurrency:** a draw keeps its packets when a submission happened during the call. `WriteMmio` counts every
  CP_RB_WPTR write in `direct::g_wptr_writes`. A submission that races with a marker being written waits for it
  (`direct::g_marking`, seq_cst on both sides): either the draw thread sees the submission and leaves its packets, or
  the submission publishes the marker whole.

### What a port needs: the game-specific part

1. **The Draw\* functions** your game calls: wrap each hook as SA2 does. A draw function you do not wrap just keeps
   its PM4. Direct3D's own Clear and Resolve draws stay PM4 in SA2 too.
2. **The device's command-buffer fields** in your XDK:
   - SA2 (2012 XDK): the write pointer is at device+48. It is the last word written, because Direct3D writes with
     `stwu` pre-increment.
   - The segment limits are at device+52 and +56. A draw during which they change keeps its packets.
   - To find them in another XDK: the function that makes command-buffer space (SA2: sub_8254EBB8, with 71 callers)
     loads the write pointer and returns it.
3. **The carried type-3 opcodes** (`direct::Carried`): only opcodes whose `Type3` handler touches nothing but the
   register file and the shader state. The report line `[native] direct state ...` gives "unknown packet N (last
   opcode XX)": add an opcode only after reading its handler.
4. **Nothing else:** the ring side, the record format and the checks do not depend on the game.

### Traps for other games

- **Predicated tiling (Banjo).** Direct3D runs a recorded command-buffer range once per tile, so the ring reaches the
  same marker several times. SA2 applies each record once and releases it. A second pass then finds no record: it
  logs an error and the game thread makes no more records. Either keep the records of a tiled range until its last
  pass (index them by number and release them at the frame's end), or do not convert draws between BeginTiling and
  EndTiling. The report's `tile replays reusing a pairing N` shows whether a game replays (SA2: 0).
- **Command buffers recorded once and run several times** (BeginCommandBuffer/RunCommandBuffer): the same problem.
- **Inline data** (DrawPrimitiveUP vertices in a NOP payload): converted, because the NOP stays in memory under the
  marker. Only a NOP in the region's first three words, which the marker overwrites, keeps the draw's packets.
- **Direct3D reading back or patching a state packet after the call returns:** SA2 has none. The lockstep captures
  are what would show one.

### How SA2 checked it

- **Self-checks:** the first 4,096 records (all of them in verify mode) are re-encoded and compared word for word with
  what Direct3D wrote. The ring checks that every marker finds its record, in order.
- **Verify mode (PC):** 1,924,356 records, all identical, 0 lost, 135.8 million checks in total with 0 differences.
- **Lockstep captures (PC):** `vblank_per_swap`, `sa2_video_frame` = 6000, 3,000 frames, each frame labelled with its
  game frame (`.frames`). The defaults were compared with the CPU group off (the original paths) by matching every
  frame's hash against all of the other run's frames, at any offset: 2,988 of 3,000 frames identical. The other 12
  come before the baseline's first frame.
- **Numbers (PC):** 99 % of the draws converted (the other 1 % had a submission during the call). The PM4 packets the
  ring parses fell from about 21 to about 4 a draw; the constant flush's alignment fillers alone had been 11.

## Step 1b (done in SA2): the ring's back end

- **`sa2_cpu_ring_edram_memo`:** a draw whose 14 render-target registers equal the previous draw's reuses that
  draw's EDRAM preparation, but only when the full path would change nothing. 97 % of SA2's draws hit; verify mode
  runs the full path too and compares.
- **`sa2_cpu_ring_compare_uploaded`:** the constants dedupe compares with the block already uploaded (CPU-cached
  memory) instead of a second copy of every upload.
- **Deferred Vulkan recording on by default:** `sa2_native_deferred_recording`,
  `sa2_deferred_native_fast` and `sa2_deferred_native_update`. masseffect-nx measured its console ring
  going from 62 to 40 % of a core.
- **The ring's own timer:** `[native] ring thread: X ms of work per Swap (Y µs per draw, ...)`, with the WAIT_REG_MEM
  waits for the game subtracted.

## Step 2 (done in SA2, 2026-10-07): Direct3D's state flush writes the record, `sa2_cpu_direct_flush`

### Why

After step 1 the game thread still encoded every dirty register into PM4 (Direct3D's flush), and EndDrawDirect
decoded it straight back into the record. On SA2's console (round 7, the heaviest scene), the D3D draw path
(DrawVertices with everything it calls) was about 16 % of the game thread's running samples. EndDrawDirect alone was
5.3 %, a third of it decoding, a third memory-barrier stalls. The flush functions themselves were 3-4 %.

### What it does

- **The flush functions SA2 converts** (called by each Draw* function's inlined FlushState; DrawVertices calls them in
  this order):
  - `sub_82555CC8`, the shader constants: 16 registers per dirty bit, registers 0x4000 (VS) and 0x4400 (PS), shadows
    at device+1920 and +6016.
  - `sub_82555930`, the register groups: 1 register per dirty bit. DrawVertices calls it up to nine times, at
    0x2000, 0x2100, 0x2180, 0x2200, 0x2280, 0x2300, 0x2380 and 0x4900, each with its own shadow in the device.
  - `sub_82555B70`, the fetch constants: 6 registers per dirty bit from 0x4800, shadow at device+1152. It ends with a
    3-register packet at 0x5000: (0, 0x25000, 0) on its fast path.

  They all take (r3 = device, r4 = dirty mask with bit 63 first, r5 = first register, r6 = shadow); the fetch flush
  fixes r5 and r6 itself. The shader load (`sub_82556A60`), `sub_825555A0`, the 0x2388 bank (`sub_825559F0`) and the
  cache invalidation (`sub_8254F6A8`) still write PM4: together they are rare (a shader load on 11 % of draws, other
  render state on about 2 %).
- **Inside a converted draw call** (between BeginDrawDirect and EndDrawDirect), each hook (`src/me/sa2_me_hooks.cpp`)
  calls `FlushRunsDirect` (`src/me/me_native_system.cpp`):
  1. It follows the original's fast path on paper: the space check of every run against device+52, and the fillers it
     would write. If the original would take its slow path (it makes room, may submit, and the fetch flush then ends
     with other values), the hook runs the original instead.
  2. It writes the register runs, values byte-swapped from the shadow, into the drawing thread's pending ops.
  3. It writes a 3-word placeholder NOP into the command buffer where the first packet would have started
     ([0xC0011000]['SA2F'][spot]), and advances device+48 by 12. The dirty masks are the caller's business, as before.
- **EndDrawDirect** decodes the region as in step 1. On a placeholder it copies that spot's ops into the record at that
  place. The draw packet and every address Direct3D keeps stay where they were.
- **When the record is not made** (any of step 1's reasons: a submission during the call, a segment change, an
  unknown packet...), each placeholder becomes a record of its own (`PublishSpots`): the placeholder is rewritten into
  a step-1 marker ([header]['SA2D'][seq]), which is the same three words. The ring applies it where the packets would
  have been. **The ring side is unchanged.**

### Concurrency (the part to get right)

- A placeholder must never reach the ring before its ops are published.
- **From the call's first placeholder to the end of the call, the drawing thread holds `g_marking`.** A submission
  from another thread then waits (WriteMmio already does), so it publishes every placeholder whole.
- **A submission the drawing thread makes itself inside the call** (Direct3D kicking from within the draw function):
  WriteMmio publishes this thread's pending placeholders first (`FlushOwnKick`), and does not wait for the marking it
  holds itself.
- `g_marking` is one flag, so only one thread may draw this way. A second drawing thread turns step 2 off for the
  session.

### Self-check

- **The original, run into a scratch buffer:** the first 4,096 flushes, and every flush in verify mode.
  - The hook copies the device into a SystemHeapAlloc scratch area, points the copy's write pointer and limits at a
    scratch command buffer, and runs the original on the copy (r3, and the shadow argument, rebased into it).
  - It decodes what the original wrote and compares it word for word with the native ops. It also compares the copy's
    dirty masks (+0..47, +60..63), then puts the PPC context back.
  - The real device is never touched: a submission from another thread may read its write pointer at any moment.
  - A difference is a DIFFERENCE line, step 2 turns off, and that call runs the original for real.
- **Comparing the whole device fails falsely:** something in the 24 KB device changes from another thread while the
  check runs (seen once in about 20,000 checks, gone when looked for). The first version compared the real device in
  place; compare only what the flush functions own, on the copy.
- **A placeholder that reaches the ring unpublished** is a DIFFERENCE too ("reached the ring unpublished").
- **Step 1's own re-encode check** (SameAsWritten) treats a placeholder as its spot's ops.

### What a port needs

1. **Its flush functions.**
   - Look at the Draw* function your step-1 hooks wrap: its inlined FlushState tests the device's dirty masks
     (device+0, +8, +16, +24, +32 in SA2's 2012 XDK) and calls the flush helpers with (device, mask, first
     register, shadow).
   - The three helpers to convert all look the same: `cntlzd` over the mask, a space check against device+52, a
     type-0 header per run, then the copy.
   - What differs between them: the registers per bit (16, 6, 1), the fillers (the constants align their values on 16
     bytes, the fetch constants on 8), and the fetch flush's end packet. Read your own listing for the end packet's
     values: they differ between the fast path and the slow path.
2. **Your XDK's device offsets:** the write pointer and limits (SA2: +48/+52/+56, the same as step 1).
3. **Run verify mode before anything else.** The scratch-buffer check runs your original on every call and tells you
   exactly which run or value differs.

### Traps

- **Predicated tiling / replayed ranges (Banjo):**
  - A placeholder rewritten into a marker is a one-shot record, just like step 1's.
  - A replayed range must keep its records until its last pass. With `sa2_cpu_direct_state_restore`, the words
    written back after the record are the placeholder: a replay would find 'SA2F' and report it.
- **A Draw* function that is not wrapped** by Begin/EndDrawDirect never activates step 2: its flushes write PM4 as
  before. That is safe.
- **Record size:** the values are now in the record (SA2: about 316 record words per draw instead of 55 with step 1's
  references). The 8 MB record ring held up in SA2 (0 "ring full", 0 "no room"). Watch both counters in the
  `[native] direct flush` line.

### How SA2 checked it (PC)

- **Verify mode (lockstep capture run):**
  - 8.26 million native flushes, each compared with the original's output: 0 differences (222 million checks in
    all, 0 differences).
  - Every path ran: 13,028 placeholders published alone, 6,580 of the game's own submissions inside a call,
    3,799 slow paths and 3,701 "submitted first" calls left to the original. 0 "no room".
- **The copy-based self-check** (the final version): 5.2 million flushes compared, 0 differences.
- **Lockstep captures:** the defaults with step 2 against the same build's GPU-round reference, 2,998 of 2,998
  overlapping frames pixel-identical. The verify run's 2,414 frames that overlap the references are identical too.

## Step 3: what the ring still guesses

These are the last pieces of emulation:

- **Shader identity:** today it comes from IM_LOAD addresses and microcode hashes. It can come from the bound objects
  instead (the record can carry them), with the vertex-declaration patch known from the shader-load hook.
- **Vertex and texture coherency:** today by fingerprints. It can come from Lock/Unlock and the game's fences.
- **EDRAM ownership:** today rebuilt from registers. It can come from SetRenderTarget and Resolve.

Each of these is a separate key, checked against the path it replaces.

## Round 6 (SA2, 2026-10-07): verified on PC

- **`sa2_cpu_direct_pair`:** the Draw* pairing record rides in the direct record (op `kPair`).
  - The game thread no longer pushes it through the record queue, and the ring no longer looks it up in the address
    table.
  - It also fixes a real problem: the table keeps 8 records per 8 KB bucket, but SA2 writes about 13 draws per 8 KB.
    When the game runs ahead of the ring, the oldest records are evicted first. In verify mode 40 % of the records
    were gone before the ring reached them; those draws fell back to ring-side shader identification.
  - A submission during the call pushes the held-back record before it publishes the write pointer
    (`FlushPendingPairs`).
- **`sa2_cpu_ring_identity_memo`:** a directly paired draw with the same shader objects, object-table version and
  loaded VS/PS generations reuses the previous draw's pairing and final choice. 86 % of draws hit; verify mode runs
  the full checks and compares.
- **`sa2_cpu_note_shader_pair`:** both NoteShaderObject calls are skipped when both objects are already finished
  (four calls a draw become two table loads).
- **`sa2_cpu_direct_state_refs`:** the record keeps the packet skeleton (register, count, offset); the ring reads the
  values in place from the command buffer. 54.7 record words per draw instead of about 110. Only the packets in the
  marker's three words are copied.
- **`sa2_cpu_ring_bound_plan_memo`:** `PlanEdramBoundRanges` is reused for an identical request.
- **`sa2_cpu_vertices_base_zero_r0`:** the base-zero vertex binding (upload buffer bound once, draw shifted by
  firstVertex/vertexOffset) now also covers vertex shaders that read r0. SA2's all do, so every draw used to record a
  vkCmdBindVertexBuffers, about a microsecond of NVK on the A57.
  - XenosRecomp's r0 is float(SV_VertexID + g_VertexIndexBase) in unsigned arithmetic, with SV_VertexID including
    the base vertex.
  - Setting g_VertexIndexBase = vmin - first_vertex therefore gives the same r0. The decision moved ahead of the
    shared block.
  - Check your translator emits the same r0 line and that DXC runs without -fvk-support-nonzero-base-vertex.
- **`sa2_native_fingerprint_mode` = 2 on the Switch:** the A57's hardware CRC32 pair instead of XXH3. The PC
  falls back to XXH3 by itself; the logic was checked on PC with a software shim.
- **The census** (`sa2_direct_state_census`):

  | What a draw really changes | Share of draws |
  |---|---|
  | Vertex fetch | 97 % |
  | Texture fetch | 76 % |
  | Shader constants | 33 % |
  | A shader load | 11 % |
  | Render state | about 2 % |

  The Draw path's state derivation can therefore be memoised per state epoch. That is the next target.
- **Exact lockstep captures** (`sa2_video_lockstep`): the ring waits after each presented swap until the recorder has
  taken the frame (`SetPresentHook` / `OnPresent`), and frames are labelled by presented-swap number. No flip is
  skipped however loaded the PC is. Holding the game at its flip does not work: Direct3D submits a frame's swap
  lazily, with the next frame's work.

## Round 7 (SA2, 2026-10-07): for every port

- **FPCR writes are expensive on the A57.** In SA2's console profile, the VMX-heavy game thread had 20-26 % of its
  samples right after an `msr fpcr`, its hottest function 65 %. The cause is the generator's flush-mode switches:
  `ctx.fpscr.enable/disableFlushModeUnconditional` around scalar float instructions in VMX code.
  - SA2's fix is a generator option, `fp_moves_modeless` (sdk: `config.h`, `config.cpp`, `builders/context.cpp`,
    `builders/memory.cpp`, `builders/floating_point.cpp`; helpers in `include/rex/ppc/diet_ops.h`).
  - Pure float moves (lfd/stfd forms, stfiwx, fmr, fabs, fnabs, fneg) switch nothing.
  - In VMX mode, lfs/stfs convert through mode-independent helpers: only a denormal single input or
    0 < |d| < 2^-126 takes a cold path with the mode cleared.
  - Unit-tested exact in both modes.
  - To check your own threads, count console samples whose previous instruction word is `0xD51B44xx`
    (`msr fpcr, xN`).
- **The codegen diet** (`non_volatile_as_local` + `elide_lr_stores`, as SASRT did):
  - In SA2, `read_before_write.py` found one real fragment (a pointer-table entry that reads its parent's r31).
  - Pass the C++ throw function as `--noreturn`: catch funclets merged after it otherwise look like reads before
    writes.
  - **Trap:** never put a `[functions]` entry in a flavour toml that a DLL module also includes. The DLL makes a stub
    at that address. Put `share_registers` marks in the main module's own overrides.
  - GCC may then inline a known guest halt into its callers; `check_switch_build.py` now accepts that.
  - Futari's caution: locals are synced into `ctx` only around DIRECT calls to a `share_registers` function, so a
    marked function that is also reached indirectly (REX_CALL_INDIRECT_FUNC) sees stale non-volatiles. SA2's one mark is
    named only in an exception scope table (no guest SEH dispatch is ever logged).
- **SA2's PC result for all of round 7 together:** verify mode 166 M checks, 0 differences; every frame of the exact
  lockstep captures pixel-identical to frames of the round-6 captures.
- **The dedupe's sync epoch** (Banjo's and SASRT's finding): `g_synchronizations_ring` was never advanced. SA2
  advances it at EVENT_WRITE_SHD, MEM_WRITE, REG_TO_MEM, COND_WRITE memory writes and INTERRUPT
  (`sa2_native_dedupe_sync_epochs`).
- **Texture fingerprint sampling** (`sa2_native_fingerprints_sampling`): SA2 turned it off by default. It
  tripped in 56 of 58 PC runs, after its 3,000 double-checked rechecks, on a texture whose changes the sample misses.
  Look for "SWITCHED OFF" in your logs.
- **The shared constant block** changes on 99.6 % of SA2's draws: g_VertexIndexBase every draw, texture slot 0 on about
  65 %. Holding the index base for shaders that do not read r0 would gain nothing in SA2.
- **Futari's deferred-update question**, from SA2's code:
  - UpdateSets deep-copies every array.
  - vkResetDescriptorPool, vkFree* and vkDestroy* drain first; vkAllocateDescriptorSets runs direct.
  - A descriptor update that bypasses the proxy table, or `vkUpdateDescriptorSetWithTemplate` (not in the drain list:
    it runs at once while vkUpdateDescriptorSets is queued), would reorder updates on the same set.

## What the other ports found (2026-10-07)

- **Futari:**
  - It draws through BeginVertices/BeginIndexedVertices. The draw packet's end is at device+13652, not +48, and a
    failed allocation must be dropped.
  - Records are held until XE_SWAP, so a replayed range finds them again.
  - SET_CONSTANT2 and SET_SHADER_CONSTANTS are carried.
  - Its in-run A/B measured no PC gain from step 1: parsing was about 8 % of its PC ring.
  - **Deferred recording is not pixel-identical in Futari:** a menu fade differed on 4 frames by up to 65 levels,
    differently in each run. It is explicit-only there.
  - **SA2's check of the same question (exact lockstep, 2026-10-07):** deferred on vs off was identical on every
    frame that both captures hold (2,519 of 2,519). That includes a 47-frame fade from black (presented swaps
    6383-6430, brightness 0.8 to 143.8). So whatever Futari hits is on a path SA2 does not exercise. SA2's gamma ramp,
    for instance, is set once at boot. SA2 keeps deferred recording on.
- **Banjo:**
  - Step 1 and 1b verified, no PC ring gain (in-run A/B). About 80 % of its PC ring per draw is the Vulkan side.
  - Bin-mask packets are placeholders patched after the draw call: anything carried by reference needs a ring-time
    re-decode check.
  - Deferred recording hangs its loading screens.
- **Transformed (via Futari):** a ring-side re-check at apply time, plus writing the three original words back after
  the record is applied.
- **Futari on step 2 (2026-10-07):** deferred. Its console game thread spends only about 1.5 % of a core in Direct3D
  (build E). Its records are held until XE_SWAP, so a placeholder's lifetime needs care there. It has 0 rejected
  draws, so the linear-fetch cause-109 case does not occur.
- **Peggle 2 on step 2 and the GPU round (2026-10-07):** not done, because there is nothing to win.
  - It draws about 33 times a frame (SA2 about 1,650). On its PC profile, Direct3D (0x8260-0x8263) and the draw hooks
    are each under 0.1 % of the game thread; its game code and Wwise take the time.
  - It resolves about twice a frame, so the GPU items wait for a console GPU profile.
  - 0 rejected draws in 40 PC logs, so no cause 109.
  - Rule of thumb from both: step 2 pays only where the draw path shows in the console profile.
- **Banjo on step 2 (2026-10-07):** deferred until the console A/B of its step 1, which is off by default (no PC ring
  gain; about 80 % of its ring is Vulkan).
  - Its helpers match SA2's: sub_8222CB90 (groups, called 8 times by DrawIndexedVertices), sub_8222D0E8 (constants)
    and sub_8222AB88 (fetch). All share the slow path sub_821E2940 and device +48/+52/+56.
  - Difference: its fetch flush checks the soft limit (+56) on the FAST path too, and kicks (sub_82210AB0). Crossing
    +56 must count as a slow path there. SA2's fast path never kicks.
  - It draws on 4 devices from 4 threads, mostly into command buffers the main ring runs later. SA2's single held
    g_marking would switch step 2 off at once.
  - **Banjo's alternative for several drawing threads:** publish each flush's record at flush time (under its step-1
    lock) behind a final marker [header][magic][seq] instead of a placeholder. Then no marking hold, no own-kick
    publication and no thread limit are needed.
  - EndDrawDirect folds the inner markers' ops into the call's record, or leaves the region as PM4. The ring's
    existing skip of older seqs releases the folded inner records ("released unreached") with no ring change.
  - The cost is one release store per flush (2-3 per draw). On SA2's A57 each one after a burst of record writes cost
    about 9 profile samples, which is why SA2 keeps the placeholder design for its single drawing thread.
- **SASRT on step 2 (2026-10-07): done, verified, packaged** (`sasrt_cpu_direct_flush`).
  - Its three flush functions match SA2's instruction for instruction (sub_82CE1760 constants, sub_82CE15C0 fetch,
    sub_82CE1310 groups). It runs SA2's design unchanged, including the device-copy self-check.
  - **Beyond SA2: two drawing threads.** Its step 1 already had a producer, record ring and marking per thread, so
    step 2 has no one-thread limit there. Per-thread marking is the other answer to Banjo's problem, without a release
    store per flush.
  - Its ring-side region check (Banjo's) now treats a placeholder as its spot's ops.
  - The key is read once per call, so an in-run A/B can flip it mid-call (110 flips, clean).
  - **Verify mode:** 34.2 million flushes on three tracks, 0 differences. 0 placeholders reached the ring, 0 lost.
    Every path ran: 11,875 placeholders published alone, 1,534 slow paths, 198 "submitted first"; 0 "no room".
  - **Coverage:** 43 % of its flushes stay with the original, because their draws are in depth-2 command buffers that
    Direct3D patches after the draw (its step-1 limit).
  - **PC in-run A/B** (race 2.3 at the game's 30 fps cap): whole-process CPU per frame -1.6 %, ring time per draw
    -3.7 %. The console decides.
- **The PC hides what the console shows.** SA2's round 5 was -21 % on the PC ring but -38 % per draw on the console
  (19.5 -> 11.4 µs). Use an in-run A/B on the shared PC and let the console decide.

## Measuring

- **Console:** `rex_profile.log` (the CPU sampler is on in every config except verify) gives each thread's CPU % and
  its hottest functions.
- **`[native] ring thread: ...`:** the ring's work per frame and per draw.
- **`[native] direct state (cumulative): ...`:** what was converted, and why the rest was not.
- **A/B configs:** `sa2_test_direct_state_off.toml` and `sa2_test_deferred_off.toml`.
