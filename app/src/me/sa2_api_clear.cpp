// Sonic Adventure 2: the native Direct3D layer, D3DDevice_Clear (sa2_api_clears). See sa2_api.h and
// docs/api-renderer.md.
//
// Direct3D clears with a draw: its own clear shaders (loaded inline), a rect list whose vertices carry the clear color
// and depth, the target's surface registers, and depth/stencil state that writes the clear values; large color-only
// clears of most formats take a fast path that binds the color buffer as a depth buffer and clears "depth" with the
// color's packed value; a tile-aligned interior can go through a 4x-MSAA alias of the same EDRAM. Afterwards the window
// scissor is restored and the groups the clear changed are marked dirty for the next draw.
//
// Here the same work is native code (sub_82563AB0 -> sub_82563978 -> sub_825632F8 -> sub_82562D58 -> sub_82562AF0 ->
// sub_82562838, transliterated with the Rlwinm helpers), writing a record of ops: exactly the original's packets as
// register runs and type-3 ops, the vertices in Direct3D's inline-data area as before. The packed value of the fast
// path comes from sub_82563DF0, a pure function of the color and the format (the recompiled original, called with the
// same inputs: to be replaced by a native version). Tiling (+11068 flags 0x30, +11071 flag 0x20) is not reached in SA2:
// a clear in that state runs the original.
//
// Verify mode: the original runs on a device copy (its inline data in the shadow area); the native code runs for real;
// device, regions, ops (inline-data addresses mapped) and the vertex data are compared.
//
// On by default since 2026-10-07: 9,649 clears (1,098,494 records) compared in verify mode with 0 differences, and the
// lockstep capture pixel-identical at full resolution (2,998 of 2,998 frames, with sa2_api_draws).

#include <cmath>

#include "me/sa2_api_internal.h"

SA2_ENHANCEMENT(g_api_clears, sa2_api_clears, sa2::enh::kCpu, true,
                "Native Direct3D Clear: the clear's state, shaders, vertices and draw as a record (no PM4)");

REX_EXTERN(sub_82563DF0);         // the fast clear's packed value (pure)
REX_EXTERN(__imp__sub_82563AB0);  // the original (verify mode's shadow run, and the fallback)

namespace sa2::api {
using namespace detail;
namespace {

constexpr uint32_t kColorScale = 0x820045D0, kColorBias = 0x820045C0;  // D3DCOLOR -> float: u * scale + bias
constexpr uint32_t kDefaultColor = 0x820EE230;                          // color when none is given
constexpr uint32_t kDefaultZ = 0x82001630;                              // z when the depth is not cleared
constexpr uint32_t kClearVs = 0x82004548, kClearPs = 0x82004520;        // Direct3D's clear shaders (27 / 9 words)
constexpr uint32_t kFastK1 = 0x820045E8, kFastK2 = 0x820045E0;          // packed value -> depth (doubles)
constexpr uint32_t kAliasInterior = 0x82980B34;                         // global: interiors through the 4x alias

std::atomic<uint64_t> g_clears{0};

inline double F64At(const Direct& m, uint32_t a) { return std::bit_cast<double>(m.L64(a)); }
inline float F32At(const Direct& m, uint32_t a) { return std::bit_cast<float>(m.L32(a)); }

// fctiwz: double -> int32 toward zero, saturated (NaN -> 0x80000000).
inline int32_t Fctiwz(double v) {
  if (std::isnan(v)) return INT32_MIN;
  if (v >= 2147483647.0) return INT32_MAX;
  if (v <= -2147483648.0) return INT32_MIN;
  return int32_t(v);
}

struct Color {
  float v[4];  // R, G, B, A as Direct3D computed them
  uint8_t guest[16];  // the same four floats in guest byte order (the fast path's vector argument)
};

// sub_82562838: one rect-list draw of `n` rects through the loaded clear shaders.
void RectDraw(Ctx<Direct>& c, uint32_t n, const int32_t (*rects)[4], const Color& color, uint32_t surface_info,
              uint32_t color_info, uint32_t depth_control, double z, uint32_t mode_control) {
  Direct& m = c.m;
  const uint32_t d = c.d;
  const uint32_t words = n * 21;
  const uint32_t data = AllocInline(c.t, c.base, c.ppc, d, words, 4, true);
  if (!data) return;  // the original: nothing drawn
  const float bias = (m.L32(d + 10688) & 1) ? 0.0f : 0.5f;
  const float zf = float(z);
  for (uint32_t i = 0; i < n; ++i) {
    const float x0 = float(rects[i][0]) - bias, y0 = float(rects[i][1]) - bias;
    const float x1 = float(rects[i][2]) - bias, y1 = float(rects[i][3]) - bias;
    const float vtx[21] = {x0, y0, zf, color.v[0], color.v[1], color.v[2], color.v[3],
                           x1, y0, zf, color.v[0], color.v[1], color.v[2], color.v[3],
                           x1, y1, zf, color.v[0], color.v[1], color.v[2], color.v[3]};
    for (uint32_t k = 0; k < 21; ++k) m.S32(data + 84 * i + 4 * k, std::bit_cast<uint32_t>(vtx[k]));
  }
  Ops& o = c.ops;
  o.Regs(0x4800, {Physical(data) | 3u, Rlwimi(0x10000002u, words, 2, 6, 29), 0, 0, 0, 0});
  o.Regs(0x2000, {surface_info, color_info});
  const uint32_t msaa = Rlwinm(surface_info, 16, 30, 31);
  o.Reg(0x2301, msaa == 0 ? 0 : msaa == 1 ? 0x8001u : 0xC003u);
  o.Reg(0x2200, depth_control);
  o.Reg(0x2208, mode_control);
  o.Type3(0xC0003601u, 0, {Rlwimi(n * 0x30000u, 17, 3, 16, 31)});
  uint64_t g = m.L64(d + dev::kDirtyGroups);
  g |= 1ull << 57;
  g |= 1ull << 56;
  g |= 2048;
  g |= 8;
  m.S64(d + dev::kDirtyGroups, g);
  m.S64(d + dev::kDirtyFetch, m.L64(d + dev::kDirtyFetch) | 0x80000000ull);
  m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | (1ull << 36));
}

// sub_82562AF0: the rect split into a tile-aligned interior (through the 4x alias, when Direct3D's global says so) and
// edge strips.
void SplitDraw(Ctx<Direct>& c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const Color& color,
               uint32_t surface_info, double z, uint32_t color_info, uint32_t depth_edge, uint32_t depth_interior,
               uint32_t mode_edge, uint32_t mode_interior) {
  const uint32_t msaa = Rlwinm(surface_info, 16, 30, 31);
  int r27 = 1, r26;
  if (msaa == 0) {
    r26 = 1;
  } else {
    r26 = 0;
    if (msaa != 1) r27 = 0;
  }
  const int32_t tw = 40 << r27, th = 8 << r26;
  const int32_t ix0 = ((x0 + tw - 1) / tw) * tw, ix1 = (x1 / tw) * tw;
  const int32_t iy0 = ((y0 + th - 1) / th) * th, iy1 = (y1 / th) * th;
  if (ix0 < ix1 && iy0 < iy1 && c.m.L32(kAliasInterior) != 0) {
    int32_t edges[4][4];
    uint32_t n = 0;
    if (ix0 > x0) {
      const int32_t r[4] = {x0, y0, ix0, y1};
      std::memcpy(edges[n++], r, sizeof(r));
    }
    if (ix1 < x1) {
      const int32_t r[4] = {ix1, y0, x1, y1};
      std::memcpy(edges[n++], r, sizeof(r));
    }
    if (iy0 > y0) {
      const int32_t r[4] = {ix0, y0, ix1, iy0};
      std::memcpy(edges[n++], r, sizeof(r));
    }
    if (iy1 < y1) {
      const int32_t r[4] = {ix0, iy1, ix1, y1};
      std::memcpy(edges[n++], r, sizeof(r));
    }
    if (n) RectDraw(c, n, edges, color, surface_info, color_info, depth_edge, z, mode_edge);
    // The interior through the 4x alias: MSAA field 2, pitch and the 14-bit field above it scaled down.
    const uint32_t s23 = Rlwimi(surface_info, 1, 17, 14, 15);
    uint32_t r11 = (s23 & 0x3FFF) >> r27;
    r11 = Rlwimi(r11, s23, 0, 0, 17);
    uint32_t r8 = r11;
    const uint32_t hi = Rlwinm(r11, 14, 18, 31) >> r27;
    r8 = Rlwimi(r8, hi, 18, 0, 13);
    const int32_t interior[1][4] = {{ix0 >> r27, iy0 >> r26, ix1 >> r27, iy1 >> r26}};
    RectDraw(c, 1, interior, color, r8, color_info, depth_interior, z, mode_interior);
  } else {
    const int32_t whole[1][4] = {{x0, y0, x1, y1}};
    RectDraw(c, 1, whole, color, surface_info, color_info, depth_edge, z, mode_edge);
  }
}

// sub_82562D58: one target's clear (target -1: depth/stencil only).
void TargetClear(Ctx<Direct>& c, uint32_t flags, int32_t target, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 const Color& color, double z, uint32_t stencil) {
  Direct& m = c.m;
  const uint32_t d = c.d;
  const uint32_t r15 = m.L32(d + dev::kShadow2000);
  uint32_t r14 = 5, r21 = 0;
  if (target != -1) {
    r14 = 4;
    r21 = m.L32(m.L32(d + 12816 + 4 * uint32_t(target)) + 28);
    if (Rlwinm(r21, 0, 12, 15) == 0xA0000u) r21 = Rlwimi(r21, 1, 17, 12, 15);
    if (Rlwinm(r21, 0, 12, 15) == 0xC0000u) r21 = Rlwimi(r21, 3, 16, 12, 15);
  }
  uint32_t r30 = Rlwinm(m.L32(d + dev::kReg2203), 0, 0, 27);
  uint32_t r19 = 0;
  if (flags & 0x10) {
    r19 = 118;
    r30 |= 1;
  }
  if (flags & 0x20) {
    r19 |= 34561;
    r30 |= 4;
    if (flags & 0x40) {
      r30 = Rlwimi(r30, stencil, 8, 16, 23);
      r30 = Rlwinm(r30, 0, 27, 25);
    } else if (flags & 0x80) {
      r30 = Rlwimi(r30, stencil, 8, 16, 23);
      r30 |= 32;
    }
    c.ops.Reg(0x210D, Rlwimi(stencil, 255, 16, 0, 23));
    m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 0x10000000ull);
  }
  Sub825555A0(c, 256, r30);
  c.ops.Regs(0x2080, {0, 0, 0x20002000u});
  uint32_t r17 = r19, r18 = r14;
  if (!(flags & 0x30)) {
    const uint32_t fmt = Rlwinm(r21, 16, 28, 31);
    if (fmt != 5 && fmt != 7 && fmt != 15) {
      const uint32_t msaa = Rlwinm(r15, 16, 30, 31);
      const int32_t tw = msaa == 2 || msaa == 3 ? 40 : 80, th = msaa == 0 ? 16 : 8;
      if (x1 - x0 >= tw && y1 - y0 >= th) {
        // The color's packed value (sub_82563DF0: format, exponent bias, the color vector) in the scratch.
        const uint32_t out = c.scratch + 0x10100;
        CallHelper(c.t, &sub_82563DF0, c.ppc, c.base, {fmt, Rlwinm(r21, 12, 26, 31), out, out + 4}, color.guest);
        const uint32_t packed = rex::memory::load_and_swap<uint32_t>(HostOf(c.base, out));
        c.ops.Reg(0x2203, 0);
        c.ops.Reg(0x2002, Rlwinm(r21, 0, 20, 31));
        const uint32_t r5 = Rlwinm(packed, 24, 8, 31);
        z = double(float((double(int64_t(r5)) - F64At(m, kFastK1)) * F64At(m, kFastK2)));
        c.ops.Reg(0x210D, (packed & 0xFF) | 0x00FF0000u);
        uint64_t g = m.L64(d + dev::kDirtyGroups);
        g |= 256;
        g |= 1ull << 55;
        g |= 0x10000000ull;
        m.S64(d + dev::kDirtyGroups, g);
        r17 = 34679;
        r18 = 5;
      }
    }
  }
  const uint8_t b71 = m.L8(d + dev::kFlags71), b72 = m.L8(d + dev::kFlags72);
  if ((b71 & 0x02) || (b72 & 0x80) || (b72 & 0x40)) Sub8254F6A8(c);
  SplitDraw(c, x0, y0, x1, y1, color, r15, z, r21, r19, r17, r14, r18);
}

// sub_825632F8: one rect's clear.
void RectClear(Ctx<Direct>& c, uint32_t flags, const int32_t rect[4], const Color& color, double z, uint32_t stencil) {
  Direct& m = c.m;
  const uint32_t d = c.d;
  if (!m.L32(d + 12832)) {
    flags = Rlwinm(flags, 0, 28, 23);
    if (!flags) return;
  }
  const int32_t vx = Fctiwz(double(F32At(m, d + 13024))), vy = Fctiwz(double(F32At(m, d + 13028)));
  const int32_t vw = Fctiwz(double(F32At(m, d + 13032))), vh = Fctiwz(double(F32At(m, d + 13036)));
  int32_t x0 = rect[0] > vx ? rect[0] : vx;
  int32_t y0 = rect[1] > vy ? rect[1] : vy;
  int32_t x1 = rect[2] < vw + vx ? rect[2] : vw + vx;
  int32_t y1 = rect[3] < vh + vy ? rect[3] : vh + vy;
  if (m.L32(d + 12288) != 0) {
    const int32_t sx0 = int32_t(m.L32(d + 13052)), sy0 = int32_t(m.L32(d + 13056));
    const int32_t sx1 = int32_t(m.L32(d + 13060)), sy1 = int32_t(m.L32(d + 13064));
    if (!(x0 > sx0)) x0 = sx0;
    if (!(y0 > sy0)) y0 = sy0;
    if (!(x1 < sx1)) x1 = sx1;
    if (!(y1 < sy1)) y1 = sy1;
  }
  if (x1 <= x0 || y1 <= y0) return;
  // The dirty render-state groups first (Direct3D's partial flush).
  PartialFlush(c);
  // Direct3D's clear shaders, loaded inline; the clear's own state.
  // [type][size][microcode]: 0 = vertex, 27 words; 1 = pixel, 9 words (copied as they are, guest byte order).
  c.ops.Type3Guest(0xC01C2B00u, 0, 0, 27, m.Host(kClearVs), 27);
  c.ops.Type3Guest(0xC00A2B00u, 0, 1, 9, m.Host(kClearPs), 9);
  auto run = [&](uint32_t reg, std::initializer_list<uint32_t> values) { c.ops.Regs(reg, values); };
  run(0x2180, {0x10010001u, 0});
  run(0x2100, {0xFFFF, 0, 0});
  c.ops.Reg(0x2293, 0);
  run(0x2204, {0x10000, 0x10000, 0x300});
  c.ops.Reg(0x2312, 0xFFFF);
  c.ops.Reg(0x200D, 0);
  {
    uint64_t g = m.L64(d + dev::kDirtyGroups);
    g |= 1ull << 41;
    g |= 1ull << 40;
    g |= 1ull << 39;
    m.S64(d + dev::kDirtyGroups, g);
    m.S64(d + dev::kDirtyFetch, m.L64(d + dev::kDirtyFetch) | (1ull << 35));
    g = m.L64(d + dev::kDirtyGroups) | 128 | 64 | 32;
    m.S64(d + dev::kDirtyGroups, g);
    m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | 0x80000ull);
    g = m.L64(d + dev::kDirtyGroups) | (1ull << 44) | 0x80000ull | 0x100000ull;
    m.S64(d + dev::kDirtyGroups, g);
  }
  if (!(flags & 0xF)) {
    TargetClear(c, flags, -1, x0, y0, x1, y1, color, z, stencil);
  } else {
    c.ops.Reg(0x2104, 0xFFFF);
    c.ops.Reg(0x2202, 0);
    c.ops.Reg(0x2201, 0x10001);
    m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | (1ull << 37) | 512 | 1024);
    for (int32_t t = 0; t < 4; ++t) {
      if (!((1u << t) & flags) || !m.L32(d + 12816 + 4 * uint32_t(t))) continue;
      TargetClear(c, flags, t, x0, y0, x1, y1, color, z, stencil);
      flags = Rlwinm(flags, 0, 28, 23);
    }
  }
  // The window scissor back (Direct3D's shadow of it), as sub_8254FCE8 writes it.
  {
    const uint32_t br = m.L32(d + 10440), tl = m.L32(d + 10436);
    const int32_t y1w = int32_t(Rlwinm(br, 1, 0, 30)) >> 17, x1w = int32_t(Rlwinm(br, 17, 0, 14)) >> 17;
    const int32_t y0w = int32_t(Rlwinm(tl, 1, 0, 30)) >> 17, x0w = int32_t(Rlwinm(tl, 17, 0, 14)) >> 17;
    run(0x2080, {0, Rlwinm(uint32_t(y0w), 16, 2, 15) | Rlwinm(uint32_t(x0w), 0, 18, 31),
                 Rlwinm(uint32_t(y1w), 16, 2, 15) | Rlwinm(uint32_t(x1w), 0, 18, 31)});
  }
  m.S8(d + dev::kFlags69, uint8_t(m.L8(d + dev::kFlags69) | 4));
  m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 0x20000ull);
}

// sub_82563AB0 / sub_82563978: the color (D3DCOLOR -> floats as Direct3D's VMX code computes them: per channel
// (1 + c / 2^23) * scale + bias, a separate multiply and add), the rects (none: the bound target's whole surface), the
// default z, then each rect.
bool ClearNative(Ctx<Direct>& c, uint32_t count, uint32_t rects, uint32_t flags, uint32_t d3dcolor, double z,
                 uint32_t stencil) {
  Direct& m = c.m;
  const uint32_t d = c.d;
  Color color;
  const uint32_t bytes[4] = {(d3dcolor >> 16) & 0xFF, (d3dcolor >> 8) & 0xFF, d3dcolor & 0xFF, d3dcolor >> 24};
  for (int k = 0; k < 4; ++k) {
    const float u = std::bit_cast<float>(0x3F800000u | bytes[k]);
    const float product = F32At(m, kColorScale + 4 * k) * u;
    color.v[k] = product + F32At(m, kColorBias + 4 * k);
    const uint32_t b = std::bit_cast<uint32_t>(color.v[k]);
    for (int j = 0; j < 4; ++j) color.guest[4 * k + j] = uint8_t(b >> (24 - 8 * j));
  }
  auto one = [&](const int32_t* rect) {
    int32_t local[4];
    if (!rect) {
      const uint32_t rt = m.L32(d + 12816);
      const uint32_t surface = rt ? rt : m.L32(d + 12832);
      const uint32_t r9 = m.L32(surface + 36);
      local[0] = 0;
      local[1] = 0;
      local[2] = int32_t(Rlwinm(r9, 14, 18, 31) + 1);
      local[3] = int32_t(Rlwinm(r9, 29, 17, 31) + 1);
      rect = local;
    }
    const double zz = (flags & 0x10) ? z : double(F32At(m, kDefaultZ));
    RectClear(c, flags, rect, color, zz, stencil);
  };
  if (!rects) {
    one(nullptr);
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      int32_t r[4];
      for (int k = 0; k < 4; ++k) r[k] = int32_t(m.L32(rects + 16 * i + 4 * k));
      one(r);
    }
  }
  return true;
}

// The states in which the native clear does not follow Direct3D (checked before anything is changed).
bool ClearSupported(const Direct& m, uint32_t d, uint32_t rects) {
  if (m.L8(d + dev::kFlags68) & 0x30) return false;   // tiling
  if (m.L8(d + dev::kFlags71) & 0x20) return false;   // sub_825555A0's tiling part
  if (!rects && !m.L32(d + 12816) && !m.L32(d + 12832)) return false;  // no surface to take the rect from
  return true;
}

}  // namespace

static bool ClearNativeCall(PPCContext& ctx, uint8_t* base) {
  ThreadData* const tp = Enabled(g_api_clears, ctx);
  if (!tp) return false;
  ThreadData& t = *tp;
  const uint32_t d = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32;
  const uint32_t d3dcolor = ctx.r7.u32, stencil = ctx.r9.u32;
  const double z = ctx.f1.f64;
  if (!ClearSupported(Direct{base}, d, rects)) [[unlikely]] {
    Bump(g_fallbacks[kFbFlags]);
    return false;
  }
  Shadow shadow;
  if (sa2::enh::VerifyMode()) [[unlikely]] shadow = RunShadow(t, base, ctx, d, &__imp__sub_82563AB0);
  Direct m{base};
  Ops ops = OpenOps(t);
  Ctx<Direct> c{m, ops, base, ctx, d, Scratch(), t};
  const uint32_t submissions = m.L32(d + 11088);
  ClearNative(c, count, rects, flags, d3dcolor, z, stencil);
  const uint32_t w = MarkerSpace(t, base, ctx, d);
  SetPacketAddresses(ops, w + 16);
  uint32_t seq = 0;
  if (!Publish(seq, ops)) return true;
  rex::memory::store_and_swap<uint32_t>(HostOf(base, d + dev::kWptr), PutMarker(base, w, seq));
  Bump(g_clears);
  g_api_clears.UseSampled();
  if (shadow.ok)
    CompareShadow(t, "Clear", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + dev::kWptr)),
                  m.L32(d + 11088) != submissions, {});
  return true;
}

bool Clear(PPCContext& ctx, uint8_t* base) { return Timed([&] { return ClearNativeCall(ctx, base); }); }

uint64_t ClearCount() { return g_clears.load(std::memory_order_relaxed); }

}  // namespace sa2::api
