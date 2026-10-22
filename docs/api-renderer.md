# The API-level renderer: removing the guest Direct3D, PM4 and EDRAM (SA2, 2026-10-07)

**Status: in progress.** Native today, verified and on by default (2026-10-07): DrawVertices, BeginVertices /
DrawVerticesUP, the window-scissor writer, Clear. Remaining PM4 writers: resolve, present, fences/locks/release, init.
Then the transport switch (no ring, no markers), then EDRAM. The "Status" table below is kept current.

This document is both SA2's design record and the guide for the other ports (Peggle 2, Banjo-Kazooie: Nuts & Bolts,
Sonic & All-Stars Racing Transformed, Futari): the method is the same for any Xbox 360 game that ships the XDK's
Direct3D; the addresses and device offsets are per game (per XDK build).

## What is being removed, and what stays

Before: the game's own Xbox 360 Direct3D (the XDK library, recompiled like the rest of the game) turned every call
into PM4 packets in a command buffer in guest memory; it submitted them by writing the GPU's ring write pointer
(MMIO); the renderer's "GPU ring native" thread parsed the PM4 like the Xenos command processor, kept a Xenos register
file, emulated EDRAM (render targets as EDRAM tiles, ownership, transfers) and recorded Vulkan.

After:

- **Direct3D**: every entry point the game calls is native code. It keeps the Direct3D device structure exactly as the
  game sees it (the game's own code reads and writes parts of it inline: shader constants, dirty bits, render state),
  and it writes **host records of operations** for the renderer instead of PM4.
- **PM4**: none. Records go to the renderer through a host queue (today the ring of direct-state records, named by
  3-word marker NOPs in the command buffer while other Direct3D functions still write PM4; at the end the queue alone).
- **EDRAM**: render targets become images per Direct3D surface; clears and resolves act on surfaces.
- **Stays**: the renderer's Vulkan backend (pipelines, shaders from the package, textures, deferred recording) and its
  Xenos register file as the *state vocabulary* of the operations (register values are how the device's shadows,
  which the game itself writes, describe the state).

## Results so far (PC)

| Step | Verified |
|---|---|
| Native DrawVertices + BeginVertices/DrawVerticesUP (state flush, shader load, vertex shader binding/patching, draw) | 2,150,579 native records against the original on a device copy, 0 logic differences (only GPU-written device fields, now excluded) |
| Native window-scissor writer (registers 0x2080-0x2082) | included above (138,593 calls) |
| Native Clear (state, clear shaders, rect vertices, fast path, 4x alias interior) | 9,649 clears / 1,098,494 records against the original on a device copy, 0 differences |
| Production run | 714,556 native records in 90 s, 40-60 fps on the PC; no fallback |
| Pixel identity, draws (lockstep, full resolution) | 2,982 of 2,982 frames identical (shipped defaults vs sa2_api_draws) |
| Pixel identity, draws + Clear | 2,998 of 2,998 frames identical (shipped defaults vs sa2_api_draws + sa2_api_clears) |

## Method

### 1. Census: what the game really calls

`app/src/me/sa2_d3d_api.cpp` (cvar `sa2_diag_d3d_census`) counts calls of Direct3D functions and decodes the PM4 each
outermost call writes (the device's write pointer +48 before and after; the space function's segment switches
followed). A census build hooks every function of the Direct3D range (`tools/d3d/d3d_census_build.py` generates
`out/census/sa2_d3d_census_all.cpp` and `out/extra_hooks.txt`; configure with
`-DSA2_EXTRA_SOURCES=<that file>`; `--restore` empties the list). `sa2_diag_d3d_census_dump = A;B;...` logs the first
calls of those functions word by word.

SA2's facts (attract demo, 200 s): 109 Direct3D functions called; 14 write PM4 at run time (per frame): DrawVertices
257, DrawVerticesUP 178, Clear 2.8, the resolve 3.4 (+1 in present), the window-scissor setters (SetViewport,
SetScissorRect, the surface setters) ~17, present 1, Release 9.5 (rarely PM4), the two locks. DrawIndexedVertices is
never called in the demo.

**Trap: the "Direct3D range" is not all Direct3D.** SA2's 0x82540000-0x82546EFF is XAPI (heap alloc/free,
GetProcessHeap, WaitForSingleObject). A wait in another thread then "sees" PM4 the drawing thread writes. Find the real
start (the first function that reads the device or a resource header).

### 2. Device layout (per XDK build; SA2 = 2012)

Dirty masks: +0 VS constants, +8 PS constants, +16 register groups (and shader load bits 17-20), +24 fetch
constants, +32 misc, +40 the groups re-sent by sub_825555A0. Command buffer: +48 last word written, +52 segment end,
+56 soft limit (end - 160). Shadows: fetch +1152, VS constants +1920, PS constants +6016, bool/loop +10112, register
groups 0x2000 @+10368, 0x2100 @+10444, 0x2180 @+10528, 0x2200 @+10548, 0x2280 @+10596, 0x2300 @+10680, 0x2380
@+10832, 0x2388 @+10272. Flags +11068..+11072. +11024 -> the GPU's read pointer write-back; +11036 the next
submission. Bound VS +13072, PS +13068, declaration +12240, render targets +12816..+12828, depth +12832. Global holder
of the device: 0x8200070C.

Draw-time flags SA2 reaches: +11068 always 0 (no predicated tiling), +11071 0; +11070 0x24/0x04; +11069 0/0x04; +11072
0/0xC0. The native code covers exactly these; any other state runs the original (counted; 0 in SA2).

### 3. Transliteration

Each native function follows Direct3D's instructions, written with helpers that mirror PowerPC rotate-and-mask
(`Rlwinm`, `Rlwimi`, `Rldicr`, `Mask32/64`, `IsZero` = cntlzw + rlwinm 27,31,31) so every line can be held against the
listing (`tools/d3d/ppcfn.py <address>` prints a function's PPC from the generated code's comments). Rules that
mattered:

- **Order of loads**: Direct3D loads all dirty masks at the top of FlushState; later code sees those values even if a
  helper changes the device.
- **Masks are rotates**: `rldicr r4,r30,47,4` is rotl(G,47) & top 5 bits, not G << 47.
- **Floats**: `lfs`/`stfs` are exact float<->double; `fctiwz` truncates and saturates (NaN -> 0x80000000); VMX
  `vmaddfp` is a separate multiply and add in the recompiled code (no fusing; the app builds with
  `-ffp-contract=off`); `vupkd3d128` type 0 makes `0x3F800000 | byte`.
- **Branches the game never takes** (tiling, debug assertions) are checked BEFORE anything is changed, and the
  original runs instead.
- **Pure helpers** (the vertex-fetch patcher sub_82555E18, the interpolator relink sub_825565C8, the clear's packed
  value sub_82563DF0, the resolve's layout helpers sub_82547E20 / sub_82547378) are called as they are, on the
  thread's guest-call context with the arguments set (`CallHelper`, `CallGuest`); they read the shader objects and
  write only the buffers they are given. (To be replaced by native translations verified word for word.)
- **Stack arguments and frames.** A function with more than eight arguments reads the rest from its caller's frame
  (r1+84, r1+92, ...). The native code reads them from the caller's r1 on entry.
  - When the native code calls such a helper itself (`CallGuest`), it builds the frame below the caller's stack
    pointer, where the original's own frame would be.
  - The original's local slots that a helper reads or writes ([128], [140], ...) become a "frame" in the scratch
    area, filled exactly as the original filled its stack first.
- **Never take a pointer to an original through `sub_X` when there is no hook:** `REX_EXTERN(__imp__sub_X)` and
  `&__imp__sub_X`. A `sub_X` in code marks the function hooked for tools/direct_calls.py, and its callers lose the
  direct (inlinable) call.

### 4. Records instead of PM4

Ops (the direct-state record format, `me_native_system.cpp` namespace `direct`): `kRun [tag|count<<8][register]
[values, host order]`, `kType3 [tag|count<<8][header][packet address][payload, guest order]` (the ring runs it through
its Type3 handler: draws, shader loads, literal constant loads, events), `kPair [tag][packet][type r5 r6 r7 vs ps start
-1 device]` (the draw's shader objects: no pairing heuristics). `me::native::PublishApiRecord` publishes a record; the
caller writes the marker NOP `[0xC0011000]['SA2D'][seq]` where its packets would have begun (Direct3D's own space
check, which may submit). Draw packets get unique addresses (the ring treats a repeated packet address in a frame as a
tile replay).

Inline data (UP vertices, the clear's rect vertices) still comes from Direct3D's inline-data allocator during the
transition; the transport switch replaces it with a native arena.

### 5. Verify mode: shadow runs of the originals

With `sa2_cpu_verify`, every native call first runs the **original on a copy of the device** (`RunShadow`): the copy's
command buffer points at a large scratch buffer, so the original never asks for space, never submits and never
switches segments. Afterwards the guest regions outside the device the original may write (the bound vertex shader's
binding states, program descriptors and microcode, the declaration's id, Direct3D's globals) are captured and put
back. Then the native code runs for real (the renderer draws what the native code produced), and `CompareShadow`
compares:

- the ops against the original's packets, both reduced to a canonical form (register values between effectful type-3
  packets; fillers, NOPs, INVALIDATE_STATE and registers 0x5000+ dropped);
- the device bytes (copy after the original vs device after the native code), except the transport's fields and the
  fields other threads write (below);
- the captured regions against what the native code wrote;
- inline allocations: the k-th native one is mapped onto the k-th of the shadow run (its address words in the ops are
  rewritten before the comparison) and, when the call wrote it, its contents are compared word by word.

Traps found on the way (each cost a run):

1. **Transport fields**: +48..+59, the submission bookkeeping (+11088 the ring write position, +11152, +12256,
   +13600.., +13772, +13936..), and when the native side itself submitted (its marker's space check) +11020..+11159 and
   +13560..+13949.
2. **Fields other threads write**: +11120..+11127 (the interrupt callback), +11032 (written by the GPU, i.e. the
   ring's memory writes). A race, not a difference.
3. **The read-pointer write-back** (+11024 -> word): the shader binding's "is the GPU done with this binding" test
   reads it; the shadow and the native side read it microseconds apart and could take different paths. Verify mode
   snapshots it once per call and points the copy at the snapshot.
4. **The inline-data allocator inside a shadow run** waits for GPU progress when its (copied, stale) area looks full;
   with the snapshot above it waited forever. Its hook answers shadow runs at once from the scratch area (one block per
   allocation, recorded for the mapping).
5. **Nested hooks**: during a shadow run every native hook passes through to the original (`t_in_original`).

### 6. ARM first: what the first version cost on the Switch, and why

The first console run of the layer (round 9, 2026-10-07: verify mode on the console, 322,267 records compared, 0
differences) was correct but slow. In CPU-bound stretches at 1020 MHz it took about 18 % of the drawing thread, where
the path it replaced (round 8: decode the PM4 again in the draw hooks) took about 12 %:

| Function (round 9) | Share of the drawing thread |
|---|---|
| BindInline (the inline vertex shader copy) | 5.6 % |
| FlushState (the draw-time state flush) | 5.6 % |
| DrawVertices (the rest of the call) | 3.1 % |
| PublishApiRecord | 2.2 % |
| `__aarch64_read_tp` (thread_local), memmove (extra) | about 2 % |

The disassembly showed six causes. None of them is visible on the PC, where the same code ran fine. They are generic
to any port's native layer:

1. **The op-writing cursor was a `uint32_t` count in the same struct as the `uint32_t*` buffer.** A store of an op word
   may alias the count, so GCC reloaded and stored the count around every word: about 11 instructions per op word.
   With a pointer cursor (`uint32_t* cur`, which a `uint32_t` store cannot alias) and local copies in the loops, it
   is about 1 instruction per word.
2. **Byte-swapped copies one word at a time.** The register runs' values come from Direct3D's shadows in big-endian
   guest memory. `SwapCopy` now uses NEON `rev32` on 16 bytes at a time, 9 instructions per 8 words in the inner
   loop.
3. **`thread_local` is a function call on the Switch** (`__aarch64_read_tp`, libnx's TLS). One call sat inside a
   per-word loop (a `thread_local std::vector` payload). Each call now reaches its per-thread state once (`Thread()`)
   and hands it down (`Ctx::t`). The thread-local is a constant-initialized pointer, so there is no TLS wrapper
   function either.
4. **Copies of the whole guest context.** Every call of a pure guest helper, Direct3D's inline-data allocator or its
   space function copied the caller's `PPCContext` (2.7 KB, mostly the 128 vector registers). `GuestCall` keeps a
   context per thread (a small stack for nested calls) and copies in only r1, r2, r13 and the FP state. The
   recompiled callees read nothing else of it: their other registers are C++ locals, or are written before they
   are read.
5. **Copies that were not needed.** The vertex shader's microcode was copied byte by byte into the patch scratch,
   then word by word into a vector with a byte swap, then again into the ops with a swap back. Now it is one memcpy
   in and one memcpy out (the payload stays in guest order). The record was also built in a thread buffer and copied
   into the ring. Now it is written in place (`ReserveApiRecord` / `CommitApiRecord`, safe with one producer).
6. **The publish's store-release waited for the record's cache misses.** The ring of records (8 MB) does not fit the
   Switch's 2 MB L2, so every record line was a write miss, and the release store of the head waited for them all.
   The publish now prefetches the next 4 KB of the ring for writing, and reads the consumer's tail only when a cached
   older value says there is no room.

The fixed position scan (which words get the draw packet addresses) is now a list noted while the ops are written.
The `[api]` line reports the layer's own cost on the drawing thread: every 16th call is timed, giving µs per call
and the share of one core.

Round 10 on the console (the line above): 1.0-1.5 µs per call in heavy scenes. In profiler samples at 1020 MHz the
layer went from about 18 % of the drawing thread to 8-9 %: FlushState 2.5 %, the rest of DrawVertices 2.8 %, the
publish 1.9 %, the BeginVertices path 0.7 %. BindInline dropped out of the top 60. Round 11 tightens what is left:

7. **The ring's lines are allocated without reading DRAM** (`sa2_api_ring_zero_ahead`). The publish was still
   stalled at its store-release, and at its write prefetches: an 8 MB ring spans more 4 KB pages than the A57's TLB
   holds. `DC ZVA` allocates a 64-byte block of zeros in the cache without a memory read (allowed in user mode when
   DCZID_EL0.DZP is clear, as newlib's memset checks). Free ring space is zeroed 4 KB ahead of the head:
   - whole blocks only, each once, from the block boundary past the head;
   - only space below the consumer's tail plus the ring size;
   - checked block by block in verify mode.

   On the PC a memset of the same blocks runs the same logic, so the PC tests cover which words are zeroed.
8. **The shader pair memo.** Each draw read two slots of NoteShaderObject's shared "finished" table, which the record
   stream keeps evicting from the cache. "Finished" is permanent (the sets behind it are never erased), so the drawing
   thread remembers the last pair found finished and skips both loads while the pair repeats.

### 7. Pixel tests

Lockstep captures (`vblank_per_swap`, `sa2_video_frame=6000`, 3000 frames, labels = presented swaps). The video is
half resolution and samples every other pixel (a quarter of the image), so the capture now also writes `<name>.hash`:
the XXH3-64 of every frame at full resolution (RGB, the wall-clock regions "Press START" and the loading spinner
zeroed). `tools/d3d/hashcmp.py A B` compares two runs label by label.

## Status (per entry point)

| Entry point | SA2 address | State |
|---|---|---|
| D3DDevice_DrawVertices | 82565C60 | native, on by default (sa2_api_draws) |
| BeginVertices / DrawVerticesUP | 82565758 / 82565C18 | native, on by default (sa2_api_draws) |
| window scissor writer (SetViewport, SetScissorRect, surface setters) | 8254FCE8 | native leaf, on by default (sa2_api_draws) |
| D3DDevice_Clear | 82563AB0 | native, on by default (sa2_api_clears) |
| DrawIndexedVertices | 82566078 | to do (not called in the demo) |
| resolve | 825645A0 (core; called by the wrapper 825656F8 and by the present path) | native, being verified (sa2_api_resolves, default off). Runs the original for: clear after resolve, the present variant (6th/7th arguments), tiling, the pre-submission fence list |
| present | 82559600 -> Swap 82558E98 | to do |
| Release / locks / fences | 8254A4A8, 825490E8, 8254AA08 | to do |
| init (CreateDevice, ring setup, GPR allocation) | 8254FC10, 8254EF40, 8255E218 | to do |
| transport switch | - | after all PM4 writers |
| EDRAM -> surfaces | - | after the transport switch |
| state setters (no PM4) | ~90 functions | last |

## For the other ports

### Multi-module games (futari-nx, 2026-10-07)

Mushihimesama Futari has nine modules, each with its own copy of Direct3D (XDK 2.0.9328). Its port of this layer:

- **Where the code lives.** The native code is in the exe. Each module passes a Module table (its own `__imp__`
  originals and its globals: the interpolator mask / flag / declaration ids are three contiguous words per module),
  and the cores call the layer through exe exports. `tools/d3d_module_table.py` (futari-nx) finds each core's copy of
  a function by its instructions, with branch targets and immediates ignored.
- **Rare paths run Direct3D's own helper.** The shader load and the RB re-send (about 2 % of draws) run with
  +48/+52/+56 pointed at a scratch buffer, and their packets are decoded into ops: exact by construction. The common
  flush and the draw are native.
- **Census.** The runtime PM4 writers were BeginVertices (360 k calls in 100 s), DrawVerticesUP, the resolve, Swap
  (with a WAIT_UNTIL each), Clear, SetShaderGPRAllocation and four window-scissor writers. There was no
  DrawIndexedVertices, and the draw-time flags were 00 04 04 00 00 on every one of about 640 k draws (no predicated
  tiling).
- **Layout differences from SA2's 2012 XDK:**
  - flags at +10940 (SA2 +11068);
  - declaration +11992;
  - viz table / index +12298 / +12363;
  - stream strides +12704;
  - PS / VS +12868 / +12872;
  - inline draw end / data / words +13652 / 13656 / 13664;
  - debug object +13712.

  The dirty masks, the shadows and the command buffer pointers are the same. The FlushState misc order differs
  (0x2300, 0x2380, misc, 0x4900; no misc-bits-62-63 block). BeginVertices' slot 31 writes {1,0,1,0} first. The RB
  re-send uses SET_CONSTANT 0x2D.
- **More verify traps.**
  - The CP scratch registers' memory writeback: Swap writes SCRATCH_REG 0x578/0x57C, and they land in device bytes
    (+10992..+10999 there).
  - The inline allocator's own bookkeeping: only the native side runs the real allocator in verify mode, the shadow's
    being answered from scratch (trap 4).
- **Results (PC):**
  - 699,410 native calls compared with the original on a device copy: the ops always equal the canonical packets;
    device differences only in ring-written fields.
  - 0.44 µs per call and 57.7 op words per record without verify.
  - Menus 431 / 431 frames identical.

### Judging a lockstep difference: look at the frames

A lockstep capture pins presented swaps, not wall-clock time.
- **What looks like a rendering difference:** an animation that runs on elapsed time can show up as one, for
  example a blinking "PRESS START", ranking panels, or a background ripple during a fade. Futari's native-on frames
  equalled native-off frames 3-7 labels later.
- **Before calling it a rendering difference,** look at the raw frames, and find whether the same image exists at
  another label (`cmp/segoff.py` does it).
- **This already misled Futari once:** the fade spot that showed the native layer's "difference" is where it
  earlier rejected deferred recording and EDS3, so those rejections may have been timing too.

**The record ring assumes one command stream.** ApplyDirectRecord releases every record older than the marker it
reaches. With two devices or two drawing threads (SASRT), a marker in one stream can be reached while an older record
of another stream is still unsubmitted, and that record is lost (the report counts it: "released unreached" /
"lost"). Until records are kept per stream, take only one device / thread natively and let the others run the
original. Verify mode cannot show this: each call is compared on its own.

**Open (Peggle 2, 2026-10-07):** native Clear alone stops Peggle's submissions about 20 s in. All its clears compare
equal in verify mode, there is one device, and no record is lost.
- What it looks like: the game thread loops in Direct3D's GPU wait while the read-pointer write-back creeps
  (0x34F -> 0x3CB in 2 s) and device+11088 says 0xA09. There are 0 packets and 0 swaps per 10 s; vblanks go on.
- Native draws alone run normally; Peggle keeps native Clear off.
- Its cause is not known yet; check for this symptom when enabling native Clear in another game.

**From SASRT (2026-10-07, each verified on its PC):**
- **DrawIndexedVertices native** (98.4 % of SASRT's draws; SA2 never calls it): DrawIndexedVerticesOps in
  D:\sonic transformed\sasrt-nx\app\src\me\sasrt_api_internal.h.
  - Per chunk: register 0x2102 = base vertex, the kPair, then Type3 0xC0032201 [VIZ, initiator, indices, size].
  - More than 65535 indices are split with the primitive table's per/overlap pair.
  - 32-bit indices: size = rlwinm(count,1,8,30) | endian, initiator |= 2048.
  - Verify: 3.0 M records, 0 differences.
- **Shader load with flag 0x08 of +11071** (both load sites in SASRT) is handled natively there, instead of falling
  back.
- **Verify artifact in MarkerSpace:** Direct3D's space function runs only on the native side (the shadow copy's
  buffer is big enough) and can change device bytes (+32 in SASRT). Snapshot the device around the space call and
  skip the bytes it changed.
- **Shadow-guard race (localized copies, sa2_hot_guest.cpp `Check`):** a copy whose first call sets statics behind a
  guard word can take another path in the shadow run when another thread's first call sets the guard in between. A
  correct copy was then switched off. Fix: a per-copy lock (16 striped mutexes by code address) held from the shadow
  run through the comparison (the shadowed copies are leaves).

Known per-port constraints (from their sessions):

- **SASRT**: two drawing threads, each with its own write pointer; 50-64 % of draws go into Direct3D command buffers
  that it runs from inside a segment (indirect buffers at depth 2) and patches after the draw call returns, and runs
  many times. The base here assumes one drawing thread and no replayed command buffers: SASRT needs per-thread record
  streams and recorded op lists (replayed by RunCommandBuffer, with Direct3D's patches applied to the recorded ops).
- XDK 21119 (SASRT): its flush functions match SA2's instruction for instruction (sub_82CE1760 / 82CE15C0 / 82CE1310),
  the shader load is sub_82CE27D8, the device's command buffer pointers are the same (+48/+52/+56).
