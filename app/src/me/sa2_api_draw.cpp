// Sonic Adventure 2: the native Direct3D layer, draws (sa2_api_draws). See sa2_api.h and docs/api-renderer.md.
//
// Direct3D's DrawVertices (and its other draw functions) began with the draw-time state flush (FlushState): the dirty
// shader constants, the shader load, the dirty register groups, the dirty fetch constants and the misc groups went out
// as PM4 from the device's shadows, then the draw packet. Here the same work is native: the shadows go into a host
// record of ops (kRun register runs; kType3 for the few packets whose effect is more than registers: shader loads,
// literal constant loads, events, the draw itself; kPair names the call's shader objects so the ring needs no pairing
// heuristics), and the device is left exactly as Direct3D leaves it (dirty masks cleared, the shader load's shadow and
// flag updates, the vertex shader bindings). While the rest of Direct3D still writes PM4, the record is named by a
// 3-word marker NOP in the command buffer, so the ring applies it in order (the direct-state transport).
//
// The code follows Direct3D's instructions (sub_82565C60 and the functions it calls), transliterated with Rlwinm /
// Rlwimi helpers where the original computes with them, so each line can be held against the listing. It covers the
// device states SA2 reaches (sa2_diag_d3d_census: flag byte +11068 is always 0 at a draw, i.e. no predicated tiling,
// and +11071 is 0): a call in any other state, or one that would reach Direct3D's assertion, runs the original instead
// (a check before anything is changed; counted in the [api] line).
//
// Verify mode (RunShadow / CompareShadow in sa2_api_internal.h): the original runs first on a copy of the device whose
// command buffer is a scratch area (it never asks for space or submits), the guest regions it writes outside the device
// are captured and put back, then the native code runs for real and the two are compared:
//   - its ops against the original's packets, both reduced to the same canonical form (register values between
//     effectful type-3 packets; fillers, NOPs and Direct3D's cache invalidations dropped; registers 0x5000+ dropped:
//     the fetch flush's end marker, which no renderer code reads);
//   - the device's bytes after each (the transport's fields and the fields other threads write excepted);
//   - the captured regions, and the inline allocations (mapped in order, contents compared).
// A difference is logged (the first 16 in full) and counted in the [verify] line.
//
// On by default since 2026-10-07: 2,150,579 records compared in verify mode with 0 differences, and the lockstep capture
// pixel-identical at full resolution (2,982 of 2,982 frames).

#include "me/sa2_api_internal.h"

SA2_ENHANCEMENT(g_api_draws, sa2_api_draws, sa2::enh::kCpu, true,
                "Native Direct3D draws: DrawVertices with Direct3D's draw-time state flush and shader load as native "
                "code that writes the ring's records directly (no PM4 for the draw)");

// The pure shader-patching helpers the native shader load calls: Direct3D's vertex fetch patcher (its localized copy
// through its hook) and its export/interpolator relink. They only read the shader objects and write the microcode
// copy they are given (and their own stack frames).
REX_EXTERN(sub_82555E18);
REX_EXTERN(sub_825565C8);
// The command buffer's space function and the inline-data allocator (transport: the marker NOP and the inline
// vertices, until no Direct3D code writes PM4).
REX_EXTERN(__imp__sub_8254EBB8);
REX_EXTERN(__imp__sub_8254D9A8);
REX_EXTERN(__imp__sub_82565C18);
REX_EXTERN(__imp__sub_8254FCE8);

namespace sa2::api {
using namespace detail;

static bool DrawVerticesNative(PPCContext& ctx, uint8_t* base) {
  ThreadData* const tp = Enabled(g_api_draws, ctx);
  if (!tp) return false;
  ThreadData& t = *tp;
  const uint32_t d = ctx.r3.u32, type = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  if (const Fallback fb = Check(Direct{base}, d)) [[unlikely]] {
    Bump(g_fallbacks[fb]);
    return false;
  }
  Shadow shadow;
  if (sa2::enh::VerifyMode()) [[unlikely]] shadow = RunShadow(t, base, ctx, d, &sa2::hot::Copy_82565C60);
  NoteObjects(t, base, d);
  Direct m{base};
  Ops ops = OpenOps(t);
  Ctx<Direct> c{m, ops, base, ctx, d, Scratch(), t};
  DrawVerticesOps(c, type, start, count, 0);
  const uint32_t submissions = m.L32(d + 11088);
  const uint32_t w = MarkerSpace(t, base, ctx, d);
  SetPacketAddresses(ops, w + 16);
  uint32_t seq = 0;
  if (!Publish(seq, ops)) return true;  // the device is already updated: this draw is lost (logged)
  rex::memory::store_and_swap<uint32_t>(HostOf(base, d + dev::kWptr), PutMarker(base, w, seq));
  if (shadow.ok)
    CompareShadow(t, "DrawVertices", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + dev::kWptr)),
                  m.L32(d + 11088) != submissions, {});
  return true;
}

namespace {
// D3DDevice_BeginVertices(device, type, count, stride) natively; false when it does not apply. `data` = the inline
// vertices' address (0: no space, or the record could not be published).
bool BeginVerticesCall(ThreadData& t, const PPCContext& ctx, uint8_t* base, uint32_t d, uint32_t type, uint32_t count,
                       uint32_t stride, uint32_t& data) {
  if (const Fallback fb = Check(Direct{base}, d, 0x80000)) [[unlikely]] {
    Bump(g_fallbacks[fb]);
    return false;
  }
  Shadow shadow;
  if (sa2::enh::VerifyMode()) [[unlikely]] {
    PPCContext c = ctx;  // the original's arguments (DrawVerticesUP's registers differ)
    c.r4.u64 = type;
    c.r5.u64 = count;
    c.r6.u64 = stride;
    shadow = RunShadow(t, base, c, d, &sa2::hot::Copy_82565758);
  }
  NoteObjects(t, base, d);
  Direct m{base};
  Ops ops = OpenOps(t);
  Ctx<Direct> c{m, ops, base, ctx, d, Scratch(), t};
  uint32_t marker_end = 0;
  bool ok = true, submitted = false;
  data = BeginVerticesNative(c, type, count, stride, marker_end, ok, submitted);
  if (!ok) {
    data = 0;
    return true;
  }
  if (data) m.S32(d + 13844, marker_end);  // EndVertices moves the write pointer there
  if (shadow.ok) {
    // The inline vertices' address (the shadow run's allocator answered with its own block): mapped by CompareShadow.
    CompareShadow(t, "BeginVertices", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + 13844)), submitted,
                  {{13844, 13852}});
  }
  return true;
}
}  // namespace

static bool BeginVerticesEntry(PPCContext& ctx, uint8_t* base) {
  ThreadData* const tp = Enabled(g_api_draws, ctx);
  if (!tp) return false;
  uint32_t data = 0;
  if (!BeginVerticesCall(*tp, ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, data)) return false;
  ctx.r3.u64 = data;
  return true;
}

bool ShadowInlineAlloc(PPCContext& ctx, uint8_t* base) {
  // Verify mode, inside the original's shadow run (on the device copy): Direct3D's inline-data allocator would work on
  // the copy's stale bookkeeping and could wait for GPU progress the copy never sees. Its address does not matter to
  // the comparison (the native side's own allocation stands in for it): a scratch area, at once.
  if (!g_shadow) return false;
  ThreadData& t = Thread();
  if (!t.in_original) return false;
  (void)base;
  // The shadow buffer's unused upper half, one block per allocation (aligned as asked), recorded in order.
  const uint32_t words = ctx.r4.u32, align = std::max<uint32_t>(4, ctx.r5.u32);
  uint32_t a = (t.shadow_bump + align - 1) & ~(align - 1);
  if (a + words * 4 > g_shadow + kShadowCopy + kShadowBuffer - 8) a = g_shadow + kShadowCopy + kShadowBuffer / 2;
  t.shadow_bump = a + words * 4;
  t.shadow_allocs.push_back({a, words, false});
  ctx.r3.u64 = a;
  return true;
}

static bool DrawVerticesUPNative(PPCContext& ctx, uint8_t* base) {
  // D3DDevice_DrawVerticesUP(device, type, count, data, stride): BeginVertices, the vertices copied in, EndVertices.
  ThreadData* const tp = Enabled(g_api_draws, ctx);
  if (!tp) return false;
  const uint32_t d = ctx.r3.u32, count = ctx.r5.u32, src = ctx.r6.u32, stride = ctx.r7.u32;
  uint32_t data = 0;
  if (!BeginVerticesCall(*tp, ctx, base, d, ctx.r4.u32, count, stride, data)) return false;
  if (data) {
    std::memmove(HostOf(base, data), HostOf(base, src), size_t(count) * stride);
    rex::memory::store_and_swap<uint32_t>(HostOf(base, d + dev::kWptr),
                                          rex::memory::load_and_swap<uint32_t>(HostOf(base, d + 13844)));
  }
  return true;
}

// sub_8254FCE8(device, x0, y0, x1, y1): the window offset and scissor (registers 0x2080-0x2082), which Direct3D writes
// straight into the command buffer from SetViewport / SetScissorRect / the render target setters (they keep no shadow
// of them). Tiling (+11068 flags 0x10 / 0x20) wraps them per tile: never in SA2, excluded by the check.
static bool WindowScissorNative(PPCContext& ctx, uint8_t* base) {
  ThreadData* const tp = Enabled(g_api_draws, ctx);
  if (!tp) return false;
  ThreadData& t = *tp;
  const uint32_t d = ctx.r3.u32;
  const uint32_t x0 = ctx.r4.u32, y0 = ctx.r5.u32, x1 = ctx.r6.u32, y1 = ctx.r7.u32;
  const Direct m{base};
  if (m.L8(d + dev::kFlags68) & 0x30) [[unlikely]] {
    Bump(g_fallbacks[kFbFlags]);
    return false;
  }
  Shadow shadow;
  if (sa2::enh::VerifyMode()) [[unlikely]] shadow = RunShadow(t, base, ctx, d, &__imp__sub_8254FCE8);
  Ops ops = OpenOps(t);
  ops.Regs(0x2080, {0, Rlwinm(y0, 16, 2, 15) | Rlwinm(x0, 0, 18, 31), Rlwinm(y1, 16, 2, 15) | Rlwinm(x1, 0, 18, 31)});
  const uint32_t submissions = m.L32(d + 11088);
  const uint32_t w = MarkerSpace(t, base, ctx, d);
  uint32_t seq = 0;
  if (!Publish(seq, ops)) return true;
  rex::memory::store_and_swap<uint32_t>(HostOf(base, d + dev::kWptr), PutMarker(base, w, seq));
  Bump(g_window);
  if (shadow.ok)
    CompareShadow(t, "WindowScissor", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + dev::kWptr)),
                  m.L32(d + 11088) != submissions, {});
  return true;
}

// The public entry points (the hooks): each native call timed now and then (Timed: the [api] line's cost).
bool DrawVertices(PPCContext& ctx, uint8_t* base) { return Timed([&] { return DrawVerticesNative(ctx, base); }); }
bool BeginVertices(PPCContext& ctx, uint8_t* base) { return Timed([&] { return BeginVerticesEntry(ctx, base); }); }
bool DrawVerticesUP(PPCContext& ctx, uint8_t* base) { return Timed([&] { return DrawVerticesUPNative(ctx, base); }); }
bool WindowScissor(PPCContext& ctx, uint8_t* base) { return Timed([&] { return WindowScissorNative(ctx, base); }); }

uint64_t ClearCount();

void Report() {
  const uint64_t native = g_native.load(std::memory_order_relaxed);
  const uint64_t original = g_original.load(std::memory_order_relaxed);
  std::string fb;
  for (uint32_t i = 1; i < kFbCount; ++i)
    if (const uint64_t n = g_fallbacks[i].load(std::memory_order_relaxed)) fb += fmt::format(" {} {};", kFallbackNames[i], n);
  if (!native && !original && fb.empty()) return;
  REXLOG_INFO("[api] native records (sa2_api_*, cumulative): {} ({:.1f} op words each; {} window scissor, {} clears, {}), {} by the original in verify "
              "mode (on a device copy); fallbacks to the old path:{}; verify: {} compared, {} differences{}",
              native, native ? double(g_ops_words.load(std::memory_order_relaxed)) / double(native) : 0.0,
              g_window.load(std::memory_order_relaxed), ClearCount(), ResolveReport(), original,
              fb.empty() ? " none" : fb, g_checked.load(std::memory_order_relaxed),
              g_differences.load(std::memory_order_relaxed), g_failed.load(std::memory_order_relaxed) ? " (STOPPED)" : "");
  // The layer's cost on the drawing thread since the last report (about 10 s): the timed calls' average times all
  // native calls (Timed).
  static uint64_t last_calls = 0, last_ns = 0, last_timed = 0;
  const uint64_t calls = g_calls.load(std::memory_order_relaxed), ns = g_timed_ns.load(std::memory_order_relaxed),
                 timed = g_timed_calls.load(std::memory_order_relaxed);
  const uint64_t dc = calls - last_calls, dns = ns - last_ns, dt = timed - last_timed;
  last_calls = calls;
  last_ns = ns;
  last_timed = timed;
  if (dt) {
    const double per_ns = double(dns) / double(dt);
    const double total_ms = per_ns * double(dc) / 1e6;
    REXLOG_INFO("[api] drawing-thread cost (10 s): {:.2f} us per native call (every 16th timed: {} of {}), {:.1f} ms "
                "in all = {:.2f} % of one core",
                per_ns / 1000.0, dt, dc, total_ms, total_ms / 100.0);
  }
}

}  // namespace sa2::api
