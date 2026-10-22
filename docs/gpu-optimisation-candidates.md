# GPU optimisation candidates for the native renderer (Tegra X1 / NVK)

## Status (2026-10-05): implemented, waiting for console measurements

Implemented and verified pixel-identical on PC: a lockstep (`vblank_per_swap`) capture of the optimised build against
the previous native build had 226 of 330 matched frames bit-exact; every other pair differed only by wall-clock
effects such as the blinking "Press START". On PC, alternating runs with the switchable items on and off gave the same
swap rates. Speed has to be judged on the console: on the PC these paths are CPU-bound and the machine was shared.

| # | Item | Where | Switch (`sa2.toml`) | Evidence on PC |
|---|---|---|---|---|
| 1 | D3D's aliased color + depth Clear cleared in the 1x owners (`PlanClearColor0EDRAM4` / `CommitClearColor0EDRAM4`; color from the VS interpolator 0, `ProvePixelColor0IsR0`) | targets.cpp, system.cpp, estimator.h, ps_no_kill.h | `sa2_native_edram4_clear_color0` | EDRAM conversions per submission 0 (were ~440 per gameplay frame) |
| 2 | Conversion descriptor budget 256 -> 2048 per slot | targets.cpp `kConversionsEDRAMPerSlot` | always | `conversion capacity totals: 0 rollovers` |
| 8 | Redirected clears merged into rectangles, one barrier pair | targets.cpp `RedirectClearDepthEDRAM4` | always | report line `redirected clears` |
| 10 | Render pass end dependency names its consumers | draws.cpp `CreatePass` | `sa2_native_pass_narrow_end` | |
| 12 | Timestamp queries: reset only those written; per-category marks off on Switch | targets.cpp | `sa2_gpu_marks_categories` | |
| 13 | VS identity audit: failed pairs memoized | system.cpp | always | `known mismatches skipped` |
| 14 | Per-transfer pair diagnostics off on Switch; allocation-free operation counter | targets.cpp | `sa2_native_edram4_pair_report` | |
| 15 | Constant blocks with unchanged bytes reused | draws.cpp | `sa2_native_constants_dedupe` | set-4 line `same bytes reused` |
| 18 | `masseffectApplyTextureSigns`: one uniform branch when there are no signs | shader_common.h | always (shader package) | |
| 19 | `remapInput`: select chains instead of a dynamically indexed vector | shader_common.h | always (shader package) | |
| 7 | ZCULL plane kept on 1x depth views | targets.cpp `GetDepth` | `sa2_native_zcull_1x_depth`, **off** until measured | not measurable on PC |
| 9 | NVK per-draw contract bound to the name the Mesa tree exports (`nvk_switch_dibujo`) | draws.cpp | always | Switch only |

Not done:
- #3 and #17 are moot now that #1 leaves no transfers.
- #4, #6 and #11 change synchronisation or resource sharing for small gains: they are left for console profiles.
- #5 saves nothing in SA2's frame order: the next frame's draws would force the deferred copy anyway.
- #16 and #20 are not started.

These are render-cost reductions that keep the output pixel-identical. They are ranked by how much they should save on
the Switch: GM20B (Maxwell), 2 SMs / 256 cores, 256 KB L2, about 25.6 GB/s of LPDDR4 shared with the CPU, running
NVK + NAK from `C:\devkitPro\mesa-switch-main-nfsmw-source`.

The report was written by reading code and existing logs only. Nothing was built or run. Every claim cites a file and
line. "Estimate" marks a number derived here. "Unverified" marks something that could not be confirmed from the
sources.

## Paths used below

| Short name | Path |
|---|---|
| `targets.cpp` | `app/src/me/native/sa2_native_targets.cpp` |
| `draws.cpp` | `app/src/me/native/sa2_native_draws.cpp` |
| `system.cpp` | `app/src/me/me_native_system.cpp` |
| `estimator.h` | `app/src/me/me_native_draw_extent_estimator.h` |
| `mesa:` | `C:\devkitPro\mesa-switch-main-nfsmw-source\mesa-switch-main\` |
| `nfsmw:` | `D:\sonic adventure 2\_ref\nfsmw-nx\` (the newest copy of the sibling port) |
| logs | `out/pc/logs/sa2_0NN.log` (PC runs, AMD driver 24.3.1: `sa2_070.log:20-22`) |

About the Mesa line numbers:
- Lines marked † were re-opened while writing this report.
- The others come from a read-only research pass over the same tree.

## 1. What a frame costs today (evidence)

The PC GPU is an AMD card, so its timings show where time goes, not Switch milliseconds.

**Gameplay window** (`sa2_066.log`, about 39 fps):

| Item | Value | Source line |
|---|---|---|
| Swaps / draws per 10 s | 394 / 726,870, so about 1,845 draws per frame; 57 pipelines, 316 textures | 5768 |
| GPU time per Swap, by category (AMD) | scene 3.1 ms, copies 2.3, edram_import 1.9, edram_alias 2.3, 640 0.2, 320 0.2, smaller 0.1 | 5760 |
| Share of busy GPU time | about 6.5 of 10.1 ms is copies plus EDRAM transfers (64 %) | 5760 |
| EDRAM ops | color alias 85,456 and depth import 85,456 per 10 s, about 218 + 218 per frame; batching saved 53,704 import passes | 5928 |
| Transfer volume | `draw/depth-repr-switch=85456ops/583296tiles draw/color->color=85456ops/583296tiles; whole-target runs: none`, so 1,488 tiles per frame of each kind, in about 3-7 tile pieces | 5929 |
| Transfer pairs | 4x 280-pitch alias → 1x 560 (C5A0/D680, 128 ops per frame each) and 4x 640-pitch alias → 1x 1280 (C000/D2D0, 90 ops per frame each) | 5934 |
| Why the existing clear redirect declines | `also writes color (depth info 000002D0): 784`, `(... 00000680): 3136`, i.e. 2 + 8 per frame | 5935 |
| Ring CPU stall | `submissions since last report: 784 ... slot fence waits 784 (2094.4 ms)`: 2 submissions per frame, and the ring thread blocked 21 % of the time | 5942 |
| Forced fenced submissions | `conversion capacity totals` went 379 → 771 → 1241 → 1712 rollovers in successive 10 s windows, about one per frame | 4852, 5926, 6938, 8036 |
| Resolves | 1E7A0000 (512², format 22 = k_24_8) and 1E8A0000 (512², format 6): 1,568 copies each (4 per frame), 0 with draws, 0 reads. 1EFB0000 and 1F6F8000 (1280x720): one copy each per frame, 0 reads | 5927 |
| Vertex binds | `zero-base vertices (on): 0 of 243121 draws ...; 0.99 vkCmdBindVertexBuffers per draw` | grep `zero-base` in `sa2_066.log` |

**Title / attract window** (`sa2_070.log:12855-12930`, 60 fps):
- Both kinds transfer one whole target per frame (`whole-target runs: depth-repr-switch=600 color->color=600`).
- 84 % of all log lines are `VS identity mismatch`: 11,185 of 13,249 lines.

**The two resolves are back to back.** A frame-trace log (`sa2_062.log`) shows two consecutive resolves with no draws
between them:
- `RESOLVE src color (color base 0 ...) pitch 1280 msaa 0 -> 1EFB0000 1280x720 fmt 6`
- then the same source `-> 1F6F8000`.

**Clear and copy costs measured on the console** (these figures are in the code):
- `targets.cpp:8277-8278`: "0.78 real ms per Mpixel copied and 0.075 per Mpixel cleared, measured here".
- The lazy-front report uses 0.60 ms per Mpixel (`targets.cpp:8324-8326`).
- The sibling port quotes 0.60-0.78 ms per Mpixel for copies and 0.075 for clears (`nfsmw:app/src/nfsmw_nativo_destinos.cpp` around 168-173 and 338, from the research pass).

## 2. NVK / Maxwell cost model

Everything below is in `mesa:src/nouveau/vulkan` unless another path is given.

**Every barrier is a full GPU idle.**
- `nvk_barrier_flushes_waits` (`nvk_cmd_buffer.c:800-827` †) emits `NVK_BARRIER_WFI` whenever the source stages
  intersect ALL_COMMANDS.
- That covers every real stage. COLOR_ATTACHMENT_OUTPUT → FRAGMENT costs the same idle as ALL_COMMANDS → ALL_COMMANDS.
- The *destination* access mask decides the extra work:
  - SAMPLED_READ adds `INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI`.
  - UNIFORM/STORAGE/MEMORY_READ adds shader-cache (global + constant) invalidates, `INVALIDATE_SKED_CACHES` and the
    Switch MME-data sync (`nvk_cmd_buffer.c:829-877`, `1101-1185`).
  - SHADER_STORAGE_WRITE as a source adds `FLUSH_SHADER_DATA`.
- Image layout transitions do no decompress. The only extra is a ZCULL zero-fill from UNDEFINED
  (`nvk_cmd_buffer.c:1188-1205`).

**Render passes cost barriers through their dependencies.**
- The renderer uses legacy render passes with explicit external dependencies (`draws.cpp:9864-9884`).
- The common runtime turns those into `CmdPipelineBarrier2` at subpass begin and end
  (`mesa:src/vulkan/runtime/vk_render_pass.c:2393` † and `:2630` †, external-dst filter at `:2557` †).
- So with `dependencies[0]` (src ALL_COMMANDS) and `dependencies[1]` (dst MEMORY_READ|WRITE), **every pass pays one
  WFI at begin, and one WFI plus the full invalidate set at end.**

**What `vkCmdBeginRendering` emits.**
- It re-emits all 8 color target slots, the depth target, the ZCULL setup and the sample layout: about 150-200 dwords
  (`nvk_cmd_draw.c:1464-1833`).
- It dirties the CB, DS and MS dynamic-state groups, so the next draw re-emits them (`nvk_cmd_draw.c:751-777`).
- End rendering has no WFI of its own (`nvk_cmd_draw.c:2028-2142`).

**Clears.**
- loadOp CLEAR and `vkCmdClearAttachments` both use the hardware `CLEAR_SURFACE`: about 6 dwords per rect, no WFI, no
  pass break (`nvk_cmd_draw.c:1973-2023`, `nvk_cmd_clear.c:102-220`).
- The clear colour goes in as raw 32-bit words (`nvk_cmd_clear.c:194-201` †).
- `vkCmdClearColorImage` and `vkCmdClearDepthStencilImage` open an internal BeginRendering(loadOp CLEAR) for each mip
  (`nvk_cmd_clear.c:239-334`).
- For block-linear color, LOAD and DONT_CARE emit nothing.
- For depth with ZCULL: CLEAR emits `CLEAR_ZCULL_REGION`, LOAD emits `LOAD_ZCULL` (`nvk_cmd_draw.c:1923-1953` †).

**ZCULL.**
- A depth image gets a plane only with no TRANSFER_DST and no STORAGE usage (`nvk_image.c:1094-1125`).
- ZCULL runs only if the image has a plane or the pass uses loadOp CLEAR (`nvk_cmd_draw.c:1923-1953` †).

**Compression.**
- It is enabled on Switch (`nvkmd/switch/nvkmd_switch_pdev.c:509-514`).
- `is_compressed` needs a dedicated allocation (`nvk_image.c:1698-1706` †). The driver prefers dedicated allocations
  for compressible images over 1 MiB (`nvk_image.c:1448-1466` †).
- The renderer's targets use `CreateDedicatedAllocationImage` (`targets.cpp:7442-7448`), which chains
  `VkMemoryDedicatedAllocateInfo` (`sdk/src/ui/vulkan/vulkan_util.cpp:143-150`). So render targets should already be
  compressed. Inference, unverified on the console.
- There is no fast-clear or ZBC-add code. Only existing system ZBC slots are honoured (`nvk_queue.c:34-78`).

**Copies.**
- Color `vkCmdCopyImage` is a vk_meta graphics draw, and afterwards *all* dynamic state is marked dirty
  (`nvk_cmd_meta.c:519-543`, `193-195`).
- Depth/stencil copies use compute or the copy engine.
- Using the copy engine was measured slower and is off (`nvk_cmd_meta.c:422-442`).

**Compute.**
- A dispatch memcpys the roughly 2 KiB root table and builds a QMD (`nvk_cmd_dispatch.c:163-192`, `299-353`).
- Moving between 3D and compute relies on the implicit WFI of the subchannel switch (`nvk_cmd_buffer.c:952-958`).

**Push constants.** They are inline `LOAD_CONSTANT_BUFFER` data at call time (`nvk_cmd_buffer.c:2042-2064`).

**Timestamps.**
- A timestamp is `FLUSH_PENDING_WRITES` plus a report semaphore released after all preceding writes
  (`nvk_query_pool.c:400-425`).
- Resetting more than one timestamp query is a copy-engine fill plus `SET_REFERENCE` (`nvk_query_pool.c:338-398` †).
- Raw values must be multiplied by 1.627 on the Switch (`nfsmw:docs/measuring.md:64-68` †).

**Varyings.**
- NVK already drops VS outputs that the fragment shader never reads, in monolithic pipelines (`NVK_LINK_VARYINGS`,
  default on; `nvk_shader.c:1428-1445` †).

**Lessons from the sibling port.**
- "Passes that cover the whole screen are expensive", and load/store ops "barely matter" (`nfsmw:docs/platform-notes.md:46-52` †).
- Every `vkQueueSubmit` is an IPC. Splitting the frame into two submits left the GPU idle 3.6 ms per frame (`nfsmw:docs/platform-notes.md:178-181` †).
- Extended dynamic state is a loss on NVK/Maxwell (`nfsmw:docs/mesa.md:146-147` †).
- "NVK only waits for the GPU and flushes the texture cache at a barrier ... Each one costs a pipeline drain"
  (`nfsmw:app/src/nfsmw_nativo_destinos.cpp:4291-4296` †).

## 3. Ranked list

The benefit column is the expected saving on the Switch in gameplay at 1280x720.

| # | Candidate | Main saving | Expected benefit (Switch) | Risk |
|---|---|---|---|---|
| 1 | Redirect D3D's aliased color+depth Clear into the 1x views | about 436 transfer ops, about 300 passes, about 19 MB per frame | **several ms**: about 2.7 ms from bandwidth alone (estimate) plus about 600 WFIs | medium |
| 2 | Remove the fenced "conversion capacity rollover" | 1 full CPU↔GPU drain plus 1 extra submit per frame | restores overlap; the PC stall is 5.3 ms per frame | low |
| 3 | Coalesce the transfers that remain (fallback for #1) | about 218 color passes → about 1 per sync | ms-range wherever #1 declines | low-medium |
| 4 | Batch and narrow barriers around copies and clears | about 30 WFIs per frame → about 8 | 0.1-0.5 ms (estimate) | low |
| 5 | Defer or drop the never-read 512² scratch resolves | up to 8 copies, 2.1 Mpixel, 16.8 MB per frame | up to 1.3-1.6 ms (conditional) | medium |
| 6 | Share the duplicate 1280x720 resolve (1F6F8000 = 1EFB0000) | 1 copy (7.4 MB) + 2 WFIs per frame | 0.55-0.72 ms | low-medium |
| 7 | Give the 1x scene depth its ZCULL plane back | hierarchical Z rejection | unverified; NFSMW measured about 2 ms | medium |
| 8 | Clears inside the pass, multi-rect, no per-row ALL→ALL barriers | prerequisite for #1 to be cheap | avoids up to about 180 WFIs per full-screen redirected clear | low |
| 9 | Make the NVK per-draw fast paths actually active (symbol contract) | NVK draw emission | unverified; CPU per draw | low |
| 10 | Narrow the render-pass end dependency's dst access | shader/SKED invalidates + MME sync per pass end | small per pass, multiplied by the pass count | low |
| 11 | Zero-base vertex binding for every VS | about 1,800 binds per frame (5 dwords + MME macro each) | 0.6-0.8 ms CPU (estimate) | low |
| 12 | Release build: GPU category timestamps off, reset only used queries | about 25 serialising timestamps + an 8,192-query CE fill per submit | small (unverified) | none |
| 13 | VS identity audit: negative memo + log throttling | about 10k compares/s, 84 % of log lines | CPU + SD I/O | none |
| 14 | Remove diagnostic `fmt::format`/`std::map<std::string>` from the EDRAM hot path | about 1,500 formats per frame in gameplay | CPU (estimate 0.3-1 ms on the A57) | none |
| 15 | Deduplicate constant re-uploads by content (CPU shadow) | 77 % of draws re-upload VS constants | CPU + fewer cbuf rebinds | low |
| 16 | One `vkQueueSubmit` per frame (merge the output submit) | 1 IPC per frame | small CPU | medium |
| 17 | Finer state invalidation after utility passes | re-binding after each utility pass | small (mostly moot after #1) | low |
| 18 | Shader: fast path in `masseffectApplyTextureSigns` | per-tfetch ALU | small GPU (unverified) | none |
| 19 | Shader: branch-free / specialised `remapInput` | per-vertex ALU | small GPU (unverified) | none |
| 20 | Shader: structured control flow instead of `skipTo` chains | 72 compare+branch per vertex in skinned VS | small GPU (unverified) | low |

---

## 4. Candidates in detail

### 1. Redirect D3D's aliased color+depth Clear into the 1x views

**1. Title.** Turn the D3D `Clear` that draws through the 4x alias (VS n11 / PS n20) into clears of the 1x color and
1x depth views, which become the tile owners.

**2. Location.**
- `targets.cpp:2289-2297` `Draw()` → `RedirectClearDepthEDRAM4` (`targets.cpp:4257-4552`). It declines at
  `targets.cpp:4266`: `if (p.edram_overwrite_slots != 1) return no("also writes color");`
- The proof is `system.cpp:2111-2254` `ProveFullOverwrite`:
  - depth slot bit at 2163;
  - color slot bits `2u << i` at 2165-2178;
  - constant depth at 2225-2244.
- The 4x→1x depth alias map is filled only for depth, at `targets.cpp:3985-3989`.
- The transfers this replaces:
  - `SynchronizeEDRAM4` (`targets.cpp:3744-4065`) → `ConvertAliasEDRAM` (`targets.cpp:6505-6518`) →
    `ConvertAliasColorFragEDRAM4` (`targets.cpp:6373-6503`);
  - depth: `ImportColorDepthEDRAM4` (`targets.cpp:5728-6025`).

**3. Cost today.**
- Per gameplay frame (section 1): about 218 color runs, each its own render pass (`targets.cpp:6475-6495`).
- About 218 depth-import runs in about 81 passes ((85,456 − 53,704) / 392).
- Up to 67 scene-pass breaks for syncs, each a `FinishPass` (`targets.cpp:3865`; `sync 26456 calls` at `sa2_066.log:5939`).
- About 13 extra alias clear passes (cat 3/4/5 `VS11 PS20` at `sa2_066.log:5938`).
- One `vkAllocateDescriptorSets` + `vkUpdateDescriptorSets` per run (`targets.cpp:6448-6463`, `5800-5822`).
- Bandwidth (estimate, 1,488 tiles per kind per frame):
  - writes 2 × 1,488 × 5,120 B = 15.2 MB;
  - alias reads about 3.8 MB;
  - the alias clear draws themselves write about 3.8 MB;
  - about 23 MB per frame in total.
- At the console figures (copy 0.78 ms/Mpixel, clear 0.075): about 3.0 ms of transfers become about 0.29 ms of clears.
- On top of that, about 300 passes × (WFI at begin + WFI and invalidates at end).
- On the PC, `edram_import 1.9` + `edram_alias 2.3` ms per Swap (`sa2_066.log:5760`).
- In the title screen it is 2 full-target passes per frame (720 tiles each).

**4. Change.** Add a combined redirect, attempted before `PrepareDrawEDRAM4`:

```
RedirectClearAliasEDRAM4(p):                      // new, called at targets.cpp:2295
  require p.edram_overwrite_rect && p.edram_overwrite_slots == (1 | 2)   // depth + color0 only
  require p.edram_overwrite_depth                  // already proven (system.cpp:2225-2243)
  require stencil off, or a constant REPLACE over the same rect     // reuse targets.cpp:4275-4286
  require PS is a pure pass-through of interpolator 0 to oC0
          (D3D's clear PS: ps_ba62fb45b3a13bb2.hlsl:812,851; check microcode / library entry n20)
  require VS exports the same float4 to interpolator 0 at all 3 rect vertices
          (extend NativeDrawExtentEstimator::Diagnostics, estimator.h:24-30, to return export values;
           its ALU interpreter already admits interpolator exports, estimator.h:247-254)
  drawn_c = 4x color view, drawn_d = 4x depth view
  c1 = alias1x_color[drawn_c]  (new map, filled like targets.cpp:3985-3989 when a 4x->1x RGBA8 transfer of
                                same base / pitch-tiles / format runs)
  d1 = edram4_alias_1x_[drawn_d]
  rect1x = {2*x0, 2*y0, 2*x1, 2*y1}                // mx1my1: the transfer replicates alias pixel (x>>1,y>>1)
                                                   //   into 2x2 1x pixels (me_edram_color_to_color.frag)
  require tile-aligned (or the existing partial-tile rules)
  FinishPass(); queue PendingClear{c1, word, rect1x} and {d1, depth, stencil, rect1x} (see #8)
  for each tile t of rect: owners_tiles_edram4_[t] = {c1 or d1, local, ++edram4_version_}; ++edram4_epoch_
  return true                                       // the draw itself is not recorded
```

- **Colour word.** Compute the RGBA8 bytes on the CPU (round-to-nearest-even of `v*255`). Decline when any `v*255` is
  within 2^-8 of a .5 tie. Pass `b/255.0f` as the clear value.
- **Alternative (b).** Re-issue the same clear draw into the 1x views with the viewport scaled by 2. The bytes then
  come from the same shader-export → ROP path, so they are identical by construction. It costs a full-screen draw
  instead of `CLEAR_SURFACE`, but still removes every transfer.

**5. Why the output stays bit-identical.**
- Today the alias draw writes a constant word W into every covered alias pixel. The transfer copies words verbatim:
  `round(v*255)` → bytes → `/255` (`me_edram_color_to_color.frag`), and each alias pixel becomes a 2x2 block of 1x
  pixels. So the 1x tiles end up holding W over the rect, which is exactly a clear with W.
- Depth/stencil reuses the redirect logic already shipped for depth-only clears (`targets.cpp:4296-4311`, `4517-4519`).
  That includes the D24FS8 0/1-only rule (`targets.cpp:4288-4289`).
- The 4x alias images keep stale content but no longer own any tile. If something binds them later, the normal sync
  transfers from the 1x owner.
- Risks:
  - the colour rounding (handled above);
  - multi-rect clears (`system.cpp:2039-2103`) need the per-rect form;
  - alpha test / alpha-to-coverage are already excluded (`system.cpp:2160-2166`).

**6. How to verify.**
- `EDRAM mode4 transfer causes` (`targets.cpp:8055`): `draw/color->color` and `draw/depth-repr-switch` should drop to
  about 0 per frame.
- `EDRAM mode4 depth clear redirect declined by` (`targets.cpp:8105`): no more `also writes color`.
- `EDRAM mode4 operations since last report` (`targets.cpp:8037`): about 0 `color alias` / `depth import`.
- `GPU time by pass` (`targets.cpp:8129`): cat 10, cat 12 and the cat 3/4/5 `VS11 PS20` entries disappear.
- `GPU per Swap` (`system.cpp:2572`): edram_import and edram_alias go to about 0.
- Pixel A/B against the current build with the existing PC capture flow (`out/pc/*_8s.rgba`, `out/pc/captures`).
  Unverified which comparison script is canonical.

**7. NVK / Maxwell specifics.**
- It removes about 300 pass begin/end pairs: WFI via `vk_render_pass.c:2393/2630` → `nvk_cmd_buffer.c:800-827`, plus
  150-200 dwords of `nvk_CmdBeginRendering` re-setup each.
- The clears are `CLEAR_SURFACE` (`nvk_cmd_clear.c:102-220`): no WFI, about 6 dwords per rect.
- On compressed targets (dedicated allocations, `nvk_image.c:1698-1706`) clear traffic is probably below the 15 MB of
  raw writes. Unverified.
- It also unlocks #7 (ZCULL), because no shader-depth-export import touches the 1x depth view any more.

### 2. Remove the fenced "conversion capacity rollover"

**1. Title.** Stop forcing a waited submission when 256 EDRAM-conversion descriptor sets run out.

**2. Location.**
- `targets.cpp:433` sets `kConversionsEDRAMPerSlot = 256`.
- `EnsureCapacityConversionEDRAM4` (`targets.cpp:7593-7622`) calls `SendWork(true)` at `targets.cpp:7615`.
- `SendWork` waits at `targets.cpp:8007-8009` (`Complete` → `vkWaitForFences`, `targets.cpp:7748`).
- Per-op allocations: `targets.cpp:5800-5822` (import), `6448-6463` (color frag), `6557-6581` (compute).

**3. Cost today.**
- About 1 rollover per gameplay frame (`sa2_066.log:4852/5926/6938/8036`).
- Each one is a full drain: the GPU finishes, the CPU waits, then the GPU idles while the CPU records.
- Submissions are 2 per frame and the ring waited 2,094 ms per 10 s (`sa2_066.log:5942`).
- On Horizon each extra submit is an IPC and each fence check an ioctl (`nfsmw:docs/platform-notes.md:175-181` †).

**4. Change.** The descriptor contents depend only on the source view (plus the fixed sampler), so cache them:

```
VkDescriptorSet SetFor(layout, view_a, view_b):
  key = {layout, view_a, view_b}
  if (cache.contains(key)) return cache[key]
  allocate from a long-lived pool; write once; cache[key] = set
// erase on Destroy(image) / view destruction (targets.cpp:7476)
```

- Alternatively use `VK_KHR_push_descriptor` (supported, `nvk_physical_device.c:190`).
- Or chain extra per-slot pools instead of submitting.
- `EnsureCapacityConversionEDRAM4` then never calls `SendWork(true)`.

**5. Why the output stays bit-identical.** The descriptors are identical and the command order is unchanged. Only a
submission boundary and a CPU wait disappear. Risk: the lifetime of cached sets when views are destroyed.

**6. How to verify.**
- `EDRAM mode4 conversion capacity totals: N rollovers` (`targets.cpp:8230`) stays flat.
- `submissions since last report` ≈ Swaps, and `slot fence waits (ms)` drops (`targets.cpp:8172`).
- `[hitch] frame of ... N GPU waits` (`targets.cpp:2136`).

**7. NVK / Maxwell specifics.**
- A plain set bind is a 16 B root write (`nvk_cmd_buffer.c:1749-1913`).
- A push descriptor set is memcpy'd into upload memory with 256 B alignment when dirty (`nvk_cmd_buffer.c:2131-2176`).
- A kickoff goes through `nvGpuChannelKickoff` (`mesa:src/nouveau/horizon/nouveau_horizon_channel.c:1176-1325`).

### 3. Coalesce the transfers that remain (fallback for #1)

**1. Title.** Expand each transfer run to the whole contiguous range the same owner holds, and record all color
conversion runs of one sync in one render pass.

**2. Location.**
- `SynchronizeEDRAM4` phase 1 (`targets.cpp:3797-3940`) splits non-full-width areas into per-row runs (`3800-3805`).
- Batching (`targets.cpp:3946-3975`) shares only the depth-import pass (`5839-5864`) and compute barriers. Every color
  fragment run opens its own pass (`6480-6495`).
- `sa2_native_edram4_area_rect` (`targets.cpp:116-118`, applied at `4697-4702`) restricts syncs to each draw's
  proven rect.

**3. Cost today.** In gameplay, `whole-target runs: none` (`sa2_066.log:5929`). The same 1,488 tiles move in about 218
pieces per kind per frame instead of 2-4.

**4. Change.**
- (a) In phase 1, when the owner of a needed tile also owns the neighbouring tiles of the target with an unchanged
  version (`synchronized()`, `targets.cpp:3788-3796`), grow the run to the owner's whole contiguous range. Keep
  `clear_cutout` exclusions.
- (b) In phase 2, open one `PassConversionColorFrag` pass for all color runs of the sync. Inside it, issue one
  `vkCmdSetScissor` + `vkCmdPushConstants` + `vkCmdDraw` per run, as the import batch does.

**5. Why the output stays bit-identical.** A tile transferred early receives exactly the bytes the later transfer
would have written, because the source version is checked and the target does not own those tiles. Later binds see it
as synced. Risk: extra bandwidth for tiles that are later fully overwritten, so keep the proven-overwrite cutouts.

**6. How to verify.** In `EDRAM mode4 transfer causes`, `whole-target runs` > 0 and ops per pair drop to 1-2 per frame.
`EDRAM mode4 operations ... batched` (`targets.cpp:8037`).

**7. NVK / Maxwell specifics.** One pass instead of about 218 saves 2 barrier-WFIs and one `BeginRendering` re-setup per
avoided pass. Each fragment run is a full-viewport triangle scissored to the run, so rasterisation cost is the same.

### 4. Batch and narrow barriers around copies and clears

**1. Title.** One barrier pair around a sequence of resolves or clears instead of one pair per operation. No
ALL_COMMANDS/MEMORY masks where a precise mask is enough.

**2. Location.**
- `BarrierBeforeCopyResolve` / `BarrierAfterCopyResolve` (`targets.cpp:7255-7287`) around every copy (`1603-1606`,
  `2000-2001`, `2045-2046`, `2661-2664`).
- `BarrierClearEDRAM4` (ALL→ALL, MEMORY_READ|WRITE; `targets.cpp:3476-3484`) before and after every clear region
  (`1638-1641`, `3710-3716`, `4492`, `4499`).
- `CopyTilesEDRAM4` uses ALL→TRANSFER and TRANSFER→ALL (`targets.cpp:4153-4166`).

**3. Cost today.** About 15 resolves per gameplay frame (5 + 4 + 4 + 1 + 1 from `sa2_066.log:5927`) means about 30
barriers. Each is a WFI. The ALL→ALL ones also flush and invalidate the shader caches, SKED and the Switch MME sync.

**4. Change.** Keep a "copy batch" open across consecutive resolves:

```
before first copy: BarrierBeforeCopyResolve()
copy, copy, copy ...      // no barrier between copies to distinct destinations
                          // close early if a copy's destination is the next copy's source
on next non-copy command (pass begin, clear, transfer, SendWork): BarrierAfterCopyResolve()
```

Do the same for clears: one before and one after the whole clear batch. Replace `MEMORY_*` dst masks with the real
consumers: SAMPLED_READ, TRANSFER_READ/WRITE and attachment read/write.

**5. Why the output stays bit-identical.** Writes to distinct images from the same source have no hazard between them,
and every real hazard keeps its barrier.

**6. How to verify.** Add a barrier counter to the 10 s report. Compare `GPU per Swap ... copies` (`system.cpp:2572`).
Pixel A/B.

**7. NVK / Maxwell specifics.**
- `nvk_cmd_buffer.c:800-827` †: the WFI cannot be avoided per barrier, only by having fewer barriers.
- Color copies are vk_meta draws that dirty all dynamic state (`nvk_cmd_meta.c:193-195`), so grouping them also cuts
  re-emission.

### 5. Defer or drop the never-read 512² scratch resolves

**1. Title.** Do not copy EDRAM into textures that are never sampled, read back, or presented, unless it is proven
needed.

**2. Location.**
- `CopyInternal` (`targets.cpp:1371-1693`) and `CopyDepth` (`targets.cpp:1813-2052`).
- The pending-copy machinery to generalise: `PendingFront` and `PostponeCopyFront` / `ResolverPreviousFront` /
  `BeforeWriteColor` (`targets.cpp:2597-2742`).

**3. Cost today.**
- 1E7A0000 (k_24_8) and 1E8A0000 (k_8_8_8_8), 512x512, each copied 4 times per frame with 0 draws in between.
- 0 reads in every one of 61 report windows (logs 060-072; `reads` counts `ResolvedTexture` lookups, `targets.cpp:2435-2449`).
- 512² ≤ the readback limit is false (4096 texels, `targets.cpp:260-263`), so the guest never receives them.
- Cost: 8 × 0.26 Mpixel = 2.1 Mpixel, about 16.8 MB per frame, about 1.3-1.6 ms (estimate), plus 16 barriers.

**4. Change.**
- First confirm the sequence with a gameplay frame trace. `banjo_trace_frame_s` together with
  `SA2_TRACE_ALL_DRAWS=1` (`system.cpp:2334-2337`) prints the `RESOLVE ... (clear c d)` lines (`system.cpp:2294-2300`).
  If these are D3D Resolve-with-clear operations into a dummy texture, they exist only to clear EDRAM.
- Then make any full-texture resolve a pending copy:
  - record it at first read (`ResolvedTexture`) or at Swap;
  - also record it before any write or transfer that touches its source tiles;
  - drop it when a later resolve covers the whole texture.

**5. Why the output stays bit-identical.** A deferred copy is recorded before anything could observe the difference
(reads) or change its source (writes), so the destination holds the same bytes whenever it is read. Dropped copies
are overwritten before any read. Risk: savings depend on the real order. If the source is rewritten before the next
covering resolve, the copy still happens (no loss, no gain). Unverified until the trace is taken.

**6. How to verify.** `targets: faces resolved` (`targets.cpp:8257`): fewer copies for 1E7A0000/1E8A0000. Add a
"dropped" counter, like `replaced_front_` in the lazy-front report (`targets.cpp:8316-8330`).

**7. NVK / Maxwell specifics.** The depth path may be a compute resolve (`ResolverDepthGuestspaceEDRAM4`,
`targets.cpp:1969`). That means a 3D↔compute subchannel switch (implicit WFI) plus a 2 KiB root memcpy and QMD per
dispatch (`nvk_cmd_dispatch.c:163-192`).

### 6. Share the duplicate 1280x720 resolve

**1. Title.** The second resolve of the same EDRAM to 1F6F8000 should reuse 1EFB0000's image instead of copying.

**2. Location.**
- `CopyInternal` copy path `targets.cpp:1591-1610`.
- The Swap reads `resolved_.find(base)` directly (`targets.cpp:2216-2231`), so the `reads` counter never sees the
  presented texture.
- `sa2_native_lazy_front` is off (`targets.cpp:239-242`).

**3. Cost today.**
- The trace shows the two resolves back to back (`sa2_062.log`).
- 1F6F8000 had 0 draw reads in all 143 report windows (it is presented, or unused). 1EFB0000 is occasionally sampled
  (2-7 reads in about 18 windows).
- Cost: 3.7 MB read + 3.7 MB write = 7.4 MB per frame, i.e. 0.92 Mpixel × 0.60-0.78 = 0.55-0.72 ms, plus 2 barriers.

**4. Change.** Add a "resolve alias":

- if this copy has the same source image, rect and format as the previous copy;
- and no draw, clear, transfer or restore touched the source since (`draws_before == 0` plus a source epoch);
- then point `resolved_[B].image` at `resolved_[A].image` (refcounted, copy-on-write when either is resolved again or
  recreated).

Lazy front (`PostponeCopyFront`, `targets.cpp:2697-2724`) only saves the copy when the next write is a *whole* clear
(`RotateFrontBeforeClear`, `targets.cpp:2748-2805`). Here the 1x target is 1280x1280 and the clear covers only 720 rows,
so without extra work lazy front would only delay the copy.

**5. Why the output stays bit-identical.** Both copies would hold the same bytes. Risk: the resolved-image pooling,
parking and swap paths (`targets.cpp:6825-6932`, `7107`, `8014-8018`) must treat a shared image as non-swappable.

**6. How to verify.** `targets: faces resolved`: 1F6F8000 copies → 0 (or a new "shared" count). `[hitch] frame of ...
N copies` drops by 1.

**7. NVK / Maxwell specifics.** It saves one vk_meta graphics copy (`nvk_cmd_meta.c:519-543`), its full dynamic-state
dirtying, and two WFIs.

### 7. Give the 1x scene depth its ZCULL plane back

**1. Title.** Create 1x depth views without TRANSFER_DST when they never receive copy-engine stencil, and clear them
with loadOp CLEAR.

**2. Location.**
- `GetDepth` usage: `targets.cpp:6744-6754`. `with_transfer_dst` is true for every 1x depth view because
  `sa2_native_edram4_stencil_copy` is on (`targets.cpp:158-161`).
- `accepts_target_of_copy` is set at `targets.cpp:6771`.
- ZCULL cvar: `targets.cpp:283-285`. `ClearDepthInPass`: `draws.cpp:5641-5675`.

**3. Cost today.**
- The 1x scene depth (D2D0) has TRANSFER_DST, so it gets no ZCULL plane (`nvk_image.c:1094-1125`).
- Its passes use LOAD, so ZCULL is off (`nvk_cmd_draw.c:1923-1953` †).
- The logs show `copy-engine stencil imports 0` (`sa2_070.log`, "9-pass imports" line). The usage flag is paid for
  without being used in SA2.

**4. Change.**
- After #1 the scene depth receives no imports.
- Decide TRANSFER_DST per base: only views that ever needed the copy path get it, recreating lazily on first need.
- Clear through `ClearDepthInPass` / loadOp CLEAR, which emits `CLEAR_ZCULL_REGION`.

**5. Why the output stays bit-identical.** ZCULL only rejects fragments that would fail the depth test, so the depth
and stencil results are the same. Risks:
- any remaining pass that writes depth from the shader (`gl_FragDepth` imports) into a ZCULL view must not leave
  stale ZCULL data. NVK handling of that is unverified, so keep such views without a plane;
- NVK's ZCULL direction defaults to LESS (`nvk_cmd_draw.c:1356-1392`, `1806-1813`).

**6. How to verify.** The scene category in `GPU per Swap` / `GPU time by pass`. NFSMW measured about 2 ms
(`nfsmw:docs/mesa.md:49-74`, research pass). Pixel A/B.

**7. NVK / Maxwell specifics.** With a plane, LOAD emits `LOAD_ZCULL` and STORE emits `STORE_ZCULL`
(`nvk_cmd_draw.c:2035-2040`). The UNDEFINED transition zero-fills the plane (`nvk_cmd_buffer.c:1188-1205`).

### 8. Clears inside the pass, multi-rect, no per-row ALL→ALL barriers

**1. Title.** Apply clears as loadOp CLEAR of the next pass on that image, or as one `vkCmdClearAttachments` with N
rects. Never one pass plus two ALL→ALL barriers per tile row.

**2. Location.**
- `ClearColorInPass` / `ClearDepthInPass` / `ClearOnlyDepthInPass` / `ClearStencilInPass` (`draws.cpp:5641-5806`): each
  calls `FinishPass` and opens its own pass.
- `RedirectClearDepthEDRAM4` builds one region per tile row (`targets.cpp:4425-4483`; runs stop at the row end,
  `4467-4472`), with `BarrierClearEDRAM4` before and after each region (`4492`, `4499`).
- Pass begin: `draws.cpp:9602-9618`.

**3. Cost today.** A full-screen redirected clear at 1280x720 is 45 rows × (2 ALL→ALL barriers + 1 pass with 2
dependencies), about 180 WFIs. This is rare today (redirects decline) but becomes the per-frame path after #1.

**4. Change.**
- Collect the regions per image.
- Merge vertically adjacent full-width rows into one rect.
- Either store a `PendingClear` that the next `BeginPass` on that image turns into `loadOp = CLEAR` (when it covers
  the renderArea), or record one `vkCmdClearAttachments(rects[])` right after that pass begins.
- Use one barrier pair, or none if the pass dependencies already order it.

**5. Why the output stays bit-identical.** Same values in the same pixels. loadOp CLEAR and `vkCmdClearAttachments`
are the same `CLEAR_SURFACE` path in NVK (`nvk_cmd_draw.c:1973-2023`).

**6. How to verify.** Pass and barrier counters; `GPU time by pass` category `cleared` (kGpuCleared, `draws.h:117`).

**7. NVK / Maxwell specifics.** `CLEAR_SURFACE` costs about 6 dwords per rect and does not break the pass
(`nvk_cmd_clear.c:102-220`).

### 9. Make the NVK per-draw fast paths actually active

**1. Title.** The app looks up `nvk_switch_draw`. The Mesa tree named as the Switch source exports
`nvk_switch_dibujo`.

**2. Location.**
- `draws.cpp:336` declares `extern NvkSwitchDraw nvk_switch_draw __attribute__((weak));`. The request cvars
  (emission, cbufs, dynamic, preload) are at `draws.cpp:262-277`.
- `mesa:src/nouveau/vulkan/nvk_cmd_buffer.c:1633` † defines `struct nvk_switch_dibujo nvk_switch_dibujo`
  (header `nvk_cmd_buffer.h:425-436` †).
- `vk_switch_preload_pipeline` is not found in that tree.
- The repo's own `mesa/mesa-switch-masseffect.patch:538,901-912` defines `nvk_switch_draw`.

**3. Cost today.** If the NFSMW tree is linked without `mesa/mesa-switch-masseffect.patch`, the weak symbol resolves to
null. The four NVK fast paths are then never requested (the app logs `NVK per draw: this NVK lacks the
nvk_switch_draw contract`, `draws.cpp:4413`). `nvk_switch_set4` has the same name in both trees
(`nvk_cmd_buffer.h:314` †). Whether the console build applies the patch is unverified.

**4. Change.** Build the Switch Mesa from `mesa/` (pinned base + patch, `mesa/README.md`). Or add an alias (or rename)
so the symbol names match. The struct layouts appear field-compatible (version, measure, measure_failures, environment,
ticks, parts, counts, improvements[5]); confirm the enum sizes.

**5. Why the output stays bit-identical.** Each fast path self-checks and turns off on a difference (guard fields
`off` / `apagado`).

**6. How to verify.** The console log `C6 NVK per draw` line (`draws.cpp:4408-4470`), and the set-4 report
(`draws.cpp:4562`).

**7. NVK / Maxwell specifics.** NFSMW measured `vkCmdDrawIndexed` at 1.4-1.6 µs and `vkCmdBindPipeline` at 3-3.7 µs on
the console, with patched fast paths (`nfsmw:app/src/nfsmw_nativo_dibujos.cpp:236-275`, research pass).

### 10. Narrow the render-pass end dependency's dst access

**1. Title.** Change `dependencies[1].dstAccessMask` from MEMORY_READ|MEMORY_WRITE to the real consumers.

**2. Location.** `draws.cpp:9877-9882` (`CreatePass`). Similar pass creation at `targets.cpp:5471-5498` and `6294-6319`.

**3. Cost today.** Every pass end emits WFI + texture invalidate + shader-cache (global + constant) invalidate +
`INVALIDATE_SKED_CACHES` + the deferred MME sync on Switch (`nvk_cmd_buffer.c:829-877`, `1101-1185`). That is about 300+
pass ends per gameplay frame today, about 20-30 after #1.

**4. Change.** Use `dstAccessMask = SHADER_READ (sampling) | TRANSFER_READ | TRANSFER_WRITE | COLOR_ATTACHMENT_READ/WRITE |
DEPTH_STENCIL_ATTACHMENT_READ/WRITE`.
- Compute conversions already issue their own ALL→COMPUTE barrier with SHADER_READ|WRITE (`targets.cpp:6611-6621`).
- Nothing reads attachments as UBO, vertex or indirect data.

**5. Why the output stays bit-identical.** The same hazards stay ordered for every real consumer. Risk: any future
consumer type must be added. Audit with the Vulkan validation layer (synchronisation validation) on PC.

**6. How to verify.** Validation-layer sync checks on PC; GPU time per Swap.

**7. NVK / Maxwell specifics.** The WFI remains (src side). What disappears is shader-cache / SKED invalidation and the
MME-data sync, which are re-fetch costs for the next draws.

### 11. Zero-base vertex binding for every VS

**1. Title.** Let draws whose VS reads r0 use the bound-at-0 upload buffer too, by compensating `g_VertexIndexBase`.

**2. Location.**
- The condition `!p.vs->reads_r0` is at `draws.cpp:4105`.
- `reads_r0` defaults to true (`sa2_native_shaders.h:63`) and is computed only for register-linked shaders
  (`sa2_native_shaders.cpp:316`).
- `shared[136] = vmin` (`draws.cpp:3605`); the bind is at `draws.cpp:4138-4139`; cvar at `draws.cpp:756-759`.

**3. Cost today.** `0 of 243121 draws` take the path. About 0.99 `vkCmdBindVertexBuffers` per draw, about 1,800 per frame.
In NVK each is 5 dwords + the `NVK_MME_BIND_VB` macro (comment at `draws.cpp:4124-4128`). NFSMW measured 0.32-0.46 µs per
bind on the console (`nfsmw:app/src/nfsmw_nativo_dibujos.cpp:786-813`, research pass). That gives about 0.6-0.8 ms of
CPU per frame (estimate).

**4. Change.**
- Compute `reads_r0 = AluReadsR0(microcode)` (`sa2_native_shaders.cpp:189-213`) for every VS.
- For VS that do read r0, decide `base_zero` before writing the shared block and store
  `shared[136] = vmin - (first_vertex - 0)`, so that `r0 = SV_VertexID + g_VertexIndexBase` keeps the same integer
  (`vs_0ecb6aad687f1365.hlsl:887`).

**5. Why the output stays bit-identical.** The GPU fetches the same vertex bytes (the existing argument in the cvar
text) and r0.x is the same integer converted to float.

**6. How to verify.** `C6 zero-base vertices` (`draws.cpp:4606`): high percentage, about 0 binds per draw.

**7. NVK / Maxwell specifics.** It removes about 1,800 MME macro invocations per frame from the GPU front end too.

### 12. Release build: GPU category timestamps off, reset only used queries

**1. Location.**
- `MarkGpu` (`targets.cpp:2885-2908`) writes BOTTOM_OF_PIPE timestamps (`sa2_native_precise_marks`,
  `targets.cpp:299-302`) at every category change (`sa2_gpu_marks_categories=true`, `targets.cpp:312-315`).
- `BeginRecording` resets all 8,192 queries of the slot (`targets.cpp:7665-7668`, `kMarksPerSlot` at `585`).

**2. Cost today.**
- About 25 marks per gameplay frame: the interval counts in `sa2_066.log:5937` sum to about 9,900 per 392 frames.
- Each is `FLUSH_PENDING_WRITES` + a semaphore released after all preceding writes (`nvk_query_pool.c:400-425`).
- Each reset is a copy-engine fill of 8,192 × stride + `SET_REFERENCE` per submission (`nvk_query_pool.c:352-397` †).

**3. Change.** In release, set `sa2_gpu_marks_categories=false` (first and last mark per submission only). Reset
only `[0, used_by_this_slot_last_time]`.

**4. Why the output stays bit-identical.** Queries do not touch rendering.

**5. How to verify.** `GPU per Swap` still reports total and gap; A/B fps (the cvar text asks for this).

**6. NVK / Maxwell specifics.** BOTTOM_OF_PIPE maps to PIPELINE_LOCATION_ALL (`targets.cpp:294-297` comment), so each
mark waits for preceding work.

### 13. VS identity audit: negative memo and log throttling

**1. Location.**
- `PairByAddress` → `AcceptVertexShaderIdentity` (`system.cpp:1939`, `1491-1587`).
- That calls `LogVertexWordDiff` (`1471-1488`) and `DumpLoadedVertexVariant` (`1782-1823`).
- Every 256th mismatch it does an XXH3 of the microcode and a `REXLOG_WARN` (`1574-1585`).
- The report is at `system.cpp:2602-2607`.

**2. Cost today.**
- 2.77 M mismatching checks after about 4 minutes (`sa2_070.log:12856`).
- 84 % of log lines (11,185 / 13,249; about 1 MB per minute).
- The same few (loaded hash, candidate n0) pairs repeat: `4AF649A9...`, `904D06F7...`, `CC085E1F...` vs `0ECB6AAD...`.
- The final selection is always proven (`FINAL VS selection audit ... 0 unproven`).

**3. Change.**
- Memo `(object address, current_vs_hash_) → mismatch`, and skip the candidate before comparing.
- Log the first occurrence per key and counts in the 10 s report.
- Or add the D3D-rewritten n0 variants to the library (`DumpRewrittenVertexShader`, `system.cpp:1828-1856`, writes
  them with `SA2_SHADER_MISSING`), so pairing succeeds directly.

**4. Why the output stays bit-identical.** The selection logic is unchanged; only a repeated negative result is
cached.

**5. How to verify.** `VS identity audit` stops growing; log MB per minute.

**6. Note.** The SDK "invalid fetch constant" warning (`docs/sa2-notes.md:126-128`) comes only from the emulated GPU.
There are 0 occurrences in the native logs `sa2_066/070/072.log`.

### 14. Remove diagnostic strings from the EDRAM hot path

**1. Location.**
- `SynchronizeEDRAM4`, for every transfer run (`targets.cpp:3904-3928`):
  - two `fmt::format` `describe()` calls plus concatenation and a `std::map<std::string, ParEDRAM4>` lookup (`3180-3185`);
  - another nested `fmt::format` draw key into a `std::map<std::string, ...>`.
- `++edram4_operations_[operation]` builds a `std::string` key per op (`targets.cpp:7592`, `7600`).
- The decline reasons format per draw (`targets.cpp:4260-4264`).

**2. Cost today.** About 436 runs per gameplay frame, about 1,300-1,700 formats and string-map lookups per frame on the
ring thread. Estimate 0.3-1 ms on the A57; unverified.

**3. Change.** Use integer keys (pack base, format, size and MSAA into `uint64_t`), aggregate in a flat map, and format
only in the 10 s report. Or gate the diagnostics behind a cvar.

**4. Why the output stays bit-identical.** Logging only.

**5. How to verify.** Ring-thread time (console profiler); the report lines stay the same with the diagnostics on.

### 15. Deduplicate constant re-uploads by content

**1. Location.**
- `draws.cpp:3444-3501`: a new upload whenever `generation_constants_vs/ps` changes.
- Each upload is a 4 KiB UBO reservation (`kUboBytesVs/Ps`, `draws.cpp:1181-1182`) and a new set-4 dynamic offset
  (`draws.cpp:4034-4056`).

**2. Cost today.** `C6 set 4: 0.84 binds per draw ... VS 187520 by generation, PS 180929` in 243,121 draws (`sa2_066.log`
set-4 line). About 77 % of draws re-upload VS constants.

**3. Change.** Keep a CPU-side shadow of the last uploaded block per stage. If the bytes the shader reads are equal,
reuse the offset. Never compare against the uncached upload mapping, because reads from it are slow.

**4. Why the output stays bit-identical.** The bytes are identical.

**5. How to verify.** The set-4 line: binds per draw and the re-upload counters.

**6. NVK / Maxwell specifics.** Each changed dynamic offset is 4 root-array writes plus a cbuf rebind at the draw
(`nvk_cmd_buffer.c:1749-1913`, `nvk_cmd_draw.c:4850-5047`). The set-4-by-differences patch reduces this but does not
remove it.

### 16. One `vkQueueSubmit` per frame

**1. Location.** The work submit is at `targets.cpp:7994`, called from `Present` (`targets.cpp:2213`). The output
(gamma-ramp) submit is at `targets.cpp:8649`.

**2. Cost today.** Two IPC submits per frame, plus the WSI's own, plus one more per rollover (#2)
(`nfsmw:docs/platform-notes.md:178-181` †).

**3. Change.** Record the output command buffer first, then submit `[upload, work]` and `[output]` as two `VkSubmitInfo`
in one `vkQueueSubmit`, with the acquire semaphore only on the second.

**4. Why the output stays bit-identical.** Same commands, same order.

**5. Risk.** Whether the presenter's `RefreshGuestOutput` callback allows recording before submission is unverified.

**6. How to verify.** `submissions since last report` ≈ Swaps; CPU time in Present.

### 17. Finer state invalidation after utility passes

**1. Location.** `NotifyGraphicsExternalState` (`draws.cpp:5123-5133`) is called after every conversion, import and
clear pass (`targets.cpp:6496`, `6010`, `4102`; `draws.cpp:5721`, `5767`). It forgets the pipeline, sets 0-4, the
dynamic state, the vertex bindings and the index type.

**2. Cost today.** The next draw re-records everything. NVK pipeline binds cost 3-3.7 µs on the console (NFSMW, see #9).

**3. Change.** Utility passes never bind vertex or index buffers, so keep `recorded_bindings_` and
`indices_type_recorded_` valid. Sets must still be rebound when the layouts are incompatible.

**4. Why the output stays bit-identical.** Only redundant re-recording is skipped.

**5. Note.** Mostly moot once #1 removes the utility passes.

### 18. Shader: fast path in `masseffectApplyTextureSigns`

**1. Location.** `shaders/XenosRecomp/shader_common.h:256-267`, called from every `tfetch*` (`269-285`, `372`, `379`,
`394`). The packed signs come from the descriptor index bits 24+ (`draws.cpp:3128`).

**2. Cost today.** Per fetch: 4 × (shift, and, two compares, a uniform branch) even when `packedSigns == 0` (no signed
or gamma components). The magnitude is unverified.

**3. Change.** In `tfetch*`, use `[branch] if (packedSigns != 0) value = masseffectApplyTextureSigns(value, packedSigns);`.

**4. Why the output stays bit-identical.** With `packedSigns == 0` the loop changes nothing; otherwise it runs
unchanged.

**5. How to verify.** Per-pipeline instruction counts via `VK_KHR_pipeline_executable_properties` (`nvk_shader.c:1839-1901`)
or `NVK_SHADER_STATS=1`; the scene GPU time.

### 19. Shader: branch-free or specialised `remapInput`

**1. Location.**
- `shaders/XenosRecomp/shader_common.h:421-438`; emitted per vertex input at `shader_recompiler.cpp:274`.
- The code comes from the shared constants (`g_InputRemap`, `vs_0ecb6aad687f1365.hlsl:79`).

**2. Cost today.** Identity (`0xFFF`) is one uniform branch. Otherwise there is a 4-iteration loop with dynamic
component indexing, which NIR turns into select chains. The share of non-identity draws is unverified.

**3. Change.** Emit a select chain per component. Or, when the pipeline key already pins the vertex format, bake the
code in as a specialisation constant.

**4. Why the output stays bit-identical.** The same component selection for every code.

**5. How to verify.** As #18.

### 20. Shader: structured control flow instead of `skipTo` chains

**1. Location.** `shaders/XenosRecomp/shader_recompiler.cpp:2134-2180`: every CF instruction is wrapped in
`if (skipTo <= N)`. Skinned VS have 72 such blocks (`vs_0ecb6aad687f1365.hlsl` and four others).

**2. Cost today.** Up to 72 compare+branch per vertex in the 864-word skinned VS. Already about 5 µs per vertex better
than the old dispatcher (`shader_recompiler.cpp:2136-2141`). The remaining gain is unverified.

**3. Change.** For jumps that nest properly, emit `if (!cond) { ... }` regions, as a structuriser would, so a skipped
region costs one branch.

**4. Why the output stays bit-identical.** The same instruction order and predicate semantics.

**5. How to verify.** As #18; VS time in the scene category.

---

## 5. Ideas checked and rejected (or already done)

| Idea | Verdict | Evidence |
|---|---|---|
| Half precision | Excluded: not pixel-exact; NAK cannot emit HADD2/HMUL2 before Turing | `nfsmw:docs/platform-notes.md:53-54` † |
| Extended dynamic state | Already off; measured as a loss on NVK/Maxwell | `draws.cpp:985`, `996`; `nfsmw:docs/mesa.md:146-147` † |
| Caching descriptor sets for game draws | Already bindless: sets 0-3 bind about 0.01 times per draw, set 4 is a dynamic UBO | `draws.cpp:4010-4056`; substages line in `sa2_066.log` |
| Dead interpolators (22 VS outputs) | NVK already drops FS-unread outputs in linked pipelines | `nvk_shader.c:1428-1445` †; outputs at `vs_0ecb6aad687f1365.hlsl:829-885` |
| 32 always-initialised `r0..r31` | Free after SSA; no relative register addressing in the 30 generated HLSL files (grep) | `shader_recompiler.cpp:1994-2034` |
| Bicubic paths | `tfetch2DBicubic` is defined but never called in the scene PS, so DCE removes it | `ps_64b67b0fdc954df0.hlsl:341` |
| Texture upload / untiling on GPU | Not a steady-state cost: `0 uploads, 0.0 MB` per frame in gameplay | `[hitch] ring:` lines, `sa2_066.log` |
| 9-pass stencil imports, copy-engine stencil | Already avoided: `9-pass imports: none`, `copy-engine stencil imports 0` | `sa2_070.log` report block |
| Turning compression on | Already in effect: targets use dedicated allocations | see section 2 |
| MSAA | Hangs NVK on the Switch; the renderer is single-sample by design | `targets.cpp:10`, `7424-7428` |
| Copy engine for copies | Measured slower on the console | `nvk_cmd_meta.c:422-442` |
| Mode-4 redundant-clear skip | Inactive in mode 4, but SA2 makes no resolve-clears (`0 clears` in hitch lines) | `targets.cpp:1662-1664` |

## 6. How to measure on the console

- **Use stock clocks and A/B one cvar at a time.** Toggle within one session where possible, as
  `nfsmw:docs/measuring.md:21-58` describes.
- **Correct the raw GPU timestamps.**
  - Multiply them by 1.627 (`nfsmw:docs/measuring.md:64-68` †).
  - Use BOTTOM_OF_PIPE, which is already the default here (`targets.cpp:299-302`).
  - Check that categories + gap ≈ wall time.
- **Watch these lines per change:**
  - `GPU per Swap` (`system.cpp:2572`) and `GPU time by pass` (`targets.cpp:8129`);
  - `EDRAM mode4 transfer causes` and `operations` (`targets.cpp:8037`, `8055`);
  - `conversion capacity totals` (`targets.cpp:8230`);
  - `submissions ... slot fence waits` (`targets.cpp:8172`);
  - `targets: faces resolved` (`targets.cpp:8257`);
  - `C6 zero-base vertices` (`draws.cpp:4606`);
  - `[hitch] frame of` (`targets.cpp:2136`).
- **Confirm pixel identity** with the existing PC capture A/B (native before vs after) over the attract loop and one
  gameplay stage. #1, #5 and #6 change command streams in non-trivial ways.
