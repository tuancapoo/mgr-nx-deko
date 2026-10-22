# An API-level renderer for SA2 (UnleashedRecomp-NX style): assessment (2026-10-06)

SA2 draws through a **PM4 ring renderer** (`src/me`, from masseffect-nx/nfsmw-nx): the game's own Xbox 360 Direct3D
runs as recompiled code and writes PM4 packets, and a host "GPU ring" thread parses them, rebuilds the draw state from
the GPU registers (EDRAM included) and records Vulkan. UnleashedRecomp-NX and MarathonRecomp-NX use an **API-level
renderer** instead: they replace the game's D3D entry points and translate the calls directly. There is no PM4 at all.

## Why it matters: the numbers

| | Measured | Source |
|---|---|---|
| SA2 ring thread, slow gameplay (about 30 FPS, 1020 MHz) | about 26 ms per frame, 70-84 % of a core, for 1,200-1,600 draws and about 20,000 PM4 packets per frame (about 18 µs per draw) | round-2 console log and profile (`logs2 cpu on`) |
| SA2 game thread there | about 16.5 ms per frame; 12.1 % of it in the guest D3D library, mostly packet building, plus 3.4 % in our renderer hooks | same profile, D3D range 0x82540000-0x82570000 |
| MarathonRecomp-NX render thread, CPU-limited at 1020 MHz | about 6 ms per frame for 791 draws (6.8 commands per draw): **about 7.5 µs per draw** | its `cpu-limited-segment.log` |

60 FPS needs the ring under 16.7 ms. At Marathon's cost per draw, SA2's 1,200-1,600 draws would take **about 9-12 ms of
render-thread time per frame instead of 26**, in parallel with the game thread, which would also lose most of its D3D
packet building. Both threads would be under the 60 FPS budget, which no amount of tuning the ring renderer has come
close to: round 4 found its remaining costs spread thin.

## How UnleashedRecomp-NX does it

Read from `C:\devkitPro\UnleashedRecomp-NX` (`UnleashedRecomp/gpu/video.cpp`, 14,989 lines, about 7,100 of them
Switch work; `docs/SWITCH-PERFORMANCE.md`).

### Hooks

- 42 D3D functions are replaced, in these groups:
  - device and Present;
  - CreateTexture/Surface/VertexBuffer/IndexBuffer/VertexDeclaration/VertexShader/PixelShader;
  - Lock/Unlock;
  - SetRenderTarget/DepthStencil/Viewport/Scissor/Texture/StreamSource/Indices/VertexDeclaration/VS/PS;
  - DrawPrimitive/DrawIndexedPrimitive/DrawPrimitiveUP, Clear, StretchRect (the resolve).
- Render states go through the device's own function table: its hooked CreateDevice points 17 of them at host thunks
  and the rest at a no-op. There is no stencil.
- The constant setters stay guest code: they write the device's constant arrays and dirty bits.

### Draws

- At each draw the game thread copies only the dirty state.
- It packs commands into 512-command buffers.
- A render thread consumes them from a lock-free queue. It looks up the pipeline by an XXH3 of the pipeline state,
  copies the constants into a 16 MB per-frame ring and binds only what changed.
- Textures are bindless.

### What is gone, and what replaces it

- **Gone:** PM4, the ring, the EDRAM model. Vd* are log-only stubs.
- **Resolves:** a lazy link from texture to surface, copied only when the surface changes.
- **Shaders:** XenosRecomp, offline; identified by an XXH3 of the container when the shader is created.

### Not needed for SA2

Unleashed-NX also links a patched NVK. The driver changes are not part of the architecture's gain, and SA2 would stay
on the SDK's driver (no driver changes).

## What SA2 already has that would carry over

- **Shaders:** translation and the shader package (`sa2_shaders.mesp`, `tools/build_shader_library.sh`).
- **Pipelines:** the pipeline cache and prewarm.
- **Textures:** decoding and untiling, and the texture cache and pool.
- **Vertices:** the vertex input conversion, and the constants in a dynamic UBO.
- **Vulkan setup:** the Vulkan device, the presenter, and the NVK-specific paths.
- **The D3D map** (`docs/sa2-notes.md`, "Named D3D functions"): CreateTexture/Surface/VB/IB/VertexDeclaration/VS/PS,
  Clear, Resolve, BeginTiling/EndTiling, DrawVertices/DrawIndexedVertices/DrawVerticesUP, Swap, SetGammaRamp,
  AddRef/Release. The bound VS and PS are at device +0x3310 and +0x330C.
- **SA2's draws come from very few places:**
  - DrawVertices from 8 call sites, DrawIndexedVertices from 3 and DrawVerticesUP from 6;
  - CreateTexture from 31 and CreateSurface from 25;
  - so the engine funnels Direct3D through a handful of paths.
- **Textures are created through D3D** (unlike Unleashed, whose game textures bypass CreateTexture as DDS files).

## What would be new work

1. **The rest of the D3D map.** Still to find:
   - the 2012 XDK device layout: render-state function table, dirty flags, constant arrays, sampler/fetch state;
   - SetTexture, SetStreamSource/SetIndices and SetVertexDeclaration;
   - Lock/Unlock, and how predicated tiling is used.
   Unleashed's and Marathon's device layouts differ (0x5E00 and 0x5000 bytes), so SA2's must be derived:
   `tools/d3d_match.py` against named XDK functions, then a trace pass.
2. **Game thread:** the device mirror and dirty-state capture at draw time, and the command buffers.
3. **Render thread:**
   - state to Vulkan, reusing today's pipeline, texture and constants code;
   - render targets as plain images with resolves as copies. Most of PrepareDrawEDRAM4, Synchronize, Publish and
     Redirect (about 15 % of today's ring) disappears;
   - SA2's Clear (a rect list drawn through a 4x alias of the back buffer's EDRAM, `docs/sa2-notes.md`) becomes a
     hooked Clear.
4. **Stencil and the full render-state set.** Unleashed handles 17 states; SA2 uses stencil.
5. **Checks:** lockstep captures (`vblank_per_swap`) against today's renderer, frame by frame.

## Effort and risks

- **Effort:** a multi-week project, not a round of enhancements. Marathon's renderer is about 16,700 lines and
  Unleashed's about 15,000, with the Switch work. SA2's engine (Ninja, D3D9-style, few call sites) is simpler, and the
  shader, texture and pipeline code carries over, so a first PC prototype is a matter of weeks.
- **Pixel identity (the project's rule for GPU changes) is the main risk.**
  - Today's renderer emulates EDRAM: aliasing between formats and sample counts, depth/stencil packing, resolve
    conversions.
  - Wherever SA2 relies on those, a surface-based renderer must reproduce them bit for bit to keep frames identical,
    for example D24S8: Unleashed turns it into D32F and drops stencil.
  - The lockstep comparison makes every difference visible and localisable, but some passes may need extra work or a
    decision.
- **Fallback:** it would be one key, `sa2_api_renderer` (off = today's renderer), so it can be A/B tested and switched
  off at any time.

## A smaller option, not recommended

Keep the ring renderer but skip PM4: hook the draws and feed the register state straight to the renderer. This needs
almost the same front end, because D3D's register shadows and flush logic must be understood. It keeps the costliest
back-end parts (the EDRAM emulation, shader identification and pairing), and saves only the PM4 parsing (about 8-10 %
of the ring) and part of the packet building.

## Recommendation

1. **First, test deferred recording on the console** (`configs/sa2_test_deferred_on.toml`, PC-verified
   pixel-identical). It moves the NVK recording (about 30-35 % of the ring) to another thread today.
2. **Then the API-level renderer as the next big project, in this order:**
   - complete the D3D map (device layout, Set*, Lock/Unlock, tiling);
   - a PC prototype behind `sa2_api_renderer` reusing today's shader, texture and pipeline code;
   - lockstep comparison against today's renderer;
   - the console.
