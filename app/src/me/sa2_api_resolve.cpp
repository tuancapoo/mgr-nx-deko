// Sonic Adventure 2: the native Direct3D layer, the resolve (sa2_api_resolves). See sa2_api.h and
// docs/api-renderer.md.
//
// Direct3D's resolve core, sub_825645A0 (D3DDevice_Resolve's wrapper calls it, and so does the present path): a copy
// of a render target's EDRAM into a texture.
//   - It computes the copy registers 0x2318-0x231B into the device's shadows and marks them dirty; format 61 also
//     gets the exponent / clear words 0x2320-0x2322.
//   - It flushes the dirty register groups and may move the window scissor over the copied rect.
//   - It loads its own copy vertex shader inline and draws one 3-vertex rect list over the rect, its vertices from
//     the inline-data allocator.
//   - It writes the event, puts the window back, and waits for the destination's memory where Direct3D's global
//     asks for it.
//   - It resets the copy registers and stamps the destination texture with the next submission's fence.
//
// Here the same work is native, transliterated from the listing. The two layout helpers (the destination's mip/face
// placement and its tiled extent) are called as they are: they are pure. Paths SA2 does not take natively yet run
// the original (counted in the [api] line):
//   - clear after resolve (flags 0x100 / 0x200);
//   - the present path's variant (the 6th and 7th arguments: its own state flush);
//   - tiling;
//   - the fence list Direct3D keeps before the first submission.
//
// Verify mode: the original runs on a device copy, its inline vertices from the shadow area and the destination's
// header captured and put back. The native code runs for real. Device, header, ops and vertices are compared.

#include "me/sa2_api_internal.h"

SA2_ENHANCEMENT(g_api_resolves, sa2_api_resolves, sa2::enh::kCpu, false,
                "Native Direct3D resolve: the copy registers, Direct3D's copy shader and rect draw, the coherency wait "
                "and the destination's fence as a record (no PM4)");

REX_EXTERN(__imp__sub_825645A0);  // the original (verify mode's shadow run, and the fallback)
REX_EXTERN(__imp__sub_82547E20);  // the destination's mip/face placement (pure; no hook)
REX_EXTERN(__imp__sub_82547378);  // the destination's tiled extent (pure; no hook)

namespace sa2::api {
using namespace detail;
namespace {

constexpr uint32_t kGlobalKeepDirty = 0x83854D08;  // zero, with flag 0x4000: the groups are not marked dirty after
constexpr uint32_t kGlobalKeepSetup = 0x83854D0C;  // zero, with flag 0x8000: no copy shader or state block
constexpr uint32_t kGlobalCoherency = 0x83854D04;  // nonzero: wait for the destination's memory after the copy
constexpr uint32_t kBitsPerBlock = 0x820013C9;     // one byte per format, every other byte
constexpr uint32_t kDefaultPoint = 0x820046BC;     // {0, 0}
constexpr uint32_t kCopyShader = 0x820045F0;       // Direct3D's copy vertex shader, 15 words
constexpr uint32_t kBiasHalf = 0x8200154C, kBiasCenter = 0x82001630;  // the rect's vertex offset (+10688 bit 0)
constexpr uint32_t kFrame = 0x10400;  // the call's frame in the scratch area: the helpers' in/out slots

enum ResolveFallback : uint32_t { kRfTiling, kRfVariant, kRfClear, kRfFenceList, kRfCount };
const char* const kResolveFallbackNames[kRfCount] = {"tiling", "present variant", "clear after resolve", "fence list"};
std::atomic<uint64_t> g_resolves{0}, g_resolve_fallbacks[kRfCount]{};

// sub_82547138: a format's block size in texels (x, y).
void BlockDims(uint32_t f, uint32_t& w, uint32_t& h) {
  if (f > 44) {
    if (f == 49) w = h = 4;
    else if (f <= 50) w = h = 1;
    else if (f <= 53) w = h = 4;
    else if (f <= 57) w = h = 1;
    else if (f <= 61) w = h = 4;
    else w = h = 1;
    return;
  }
  if (f == 44 || f == 39) {
    w = 4;
    h = 1;
  } else if (f < 11 || (f > 12 && f <= 17)) {
    w = h = 1;
  } else if (f <= 12 || f == 40) {
    w = 2;
    h = 1;
  } else if (f <= 20) {
    w = h = 4;
  } else {
    w = h = 1;
  }
}

// sub_82563B70: the formats whose copy uses the special sample mode 7.
bool SpecialFormat(uint32_t f) {
  return f == 30 || f == 31 || f == 32 || f == 36 || f == 37 || f == 38 || f == 57 || f == 63;
}

// sub_82546FA8: a texture's dimensions from its fetch constant (`hdr` = texture + 28).
void FetchDims(const Direct& m, uint32_t hdr, uint32_t& a, uint32_t& b, uint32_t& c) {
  const uint32_t extra = Rlwinm(m.L32(hdr + 12), 2, 30, 30) + 1;
  const uint32_t kind = Rlwinm(m.L32(hdr + 20), 23, 30, 31);
  const uint32_t size = m.L32(hdr + 8);
  if (kind == 0) {
    a = (size & 0x00FFFFFFu) + extra;
    b = c = 1;
  } else if (kind == 2) {
    a = (size & 0x7FFu) + extra;
    b = Rlwinm(size, 21, 21, 31) + extra;
    c = Rlwinm(size, 10, 22, 31) + extra;
  } else {
    a = (size & 0x1FFFu) + extra;
    b = Rlwinm(size, 19, 19, 31) + extra;
    c = Rlwinm(size, 6, 26, 31) + 1;
  }
}

struct Args {
  uint32_t d, flags, rect, r6, r7, tex, point, level, face, color, stencil, params;
};

// The window scissor's corners as Direct3D keeps them (+10436 top-left, +10440 bottom-right, 15-bit signed fields).
struct Window {
  int32_t x0, y0, x1, y1;
};
Window ReadWindow(const Direct& m, uint32_t d) {
  const uint32_t tl = m.L32(d + 10436), br = m.L32(d + 10440);
  return {int32_t(Rlwinm(tl, 17, 0, 14)) >> 17, int32_t(Rlwinm(tl, 1, 0, 30)) >> 17,
          int32_t(Rlwinm(br, 17, 0, 14)) >> 17, int32_t(Rlwinm(br, 1, 0, 30)) >> 17};
}

// The ops of sub_825645A0 for the paths taken natively (checked by the caller). False: no inline space for the rect's
// vertices; the original stops there (what it wrote so far stays, nothing else changes).
bool ResolveOps(Ctx<Direct>& c, const Args& a) {
  Direct& m = c.m;
  const uint32_t d = c.d;
  const uint32_t frame = c.scratch + kFrame;
  auto F = [frame](uint32_t slot) { return frame + slot; };  // the listing's stack slots, by offset
  uint32_t flags = a.flags;
  const bool keep_dirty = (flags & 0x4000) && m.L32(kGlobalKeepDirty) == 0;  // [144]
  const bool keep_setup = (flags & 0x8000) && m.L32(kGlobalKeepSetup) == 0;  // [136]
  const uint32_t src = flags & 7;                                            // [148]: render target 0-3, depth 4
  if (!(flags & 0x70)) {
    const uint32_t surface = src == 4 ? m.L32(d + 12832) : m.L32(d + (src + 3204) * 4);
    const uint32_t msaa = m.L16(surface + 24) & 3;
    flags |= msaa == 0 ? 0x10u : msaa == 1 ? 0x50u : 0x70u;
  }
  // The destination's header and placement.
  const uint32_t tex = a.tex;
  const uint32_t t28 = m.L32(tex + 28), t32 = m.L32(tex + 32), t36 = m.L32(tex + 36), t40 = m.L32(tex + 40);
  const uint32_t t44 = m.L32(tex + 44), t48 = m.L32(tex + 48);
  const uint32_t extra = Rlwinm(t40, 2, 30, 30) + 1;
  uint32_t width, height, slices = 1;  // r10, r9, r22
  if ((t48 & 0x600) != 1024) {
    width = t36 & 0x1FFF;
    height = Rlwinm(t36, 19, 19, 31);
  } else {
    width = t36 & 0x7FF;
    slices = 8;
    height = Rlwinm(t36, 21, 21, 31);
  }
  uint32_t r24 = height + extra;
  m.S32(F(128), r24);
  m.S32(F(132), width + extra);
  uint32_t bw = 1, bh = 1;
  BlockDims(t32 & 0x3F, bw, bh);
  m.S32(F(152), bw);
  m.S32(F(156), bh);
  const uint32_t face = a.face;
  m.S32(F(140), t32 & 0xFFFFF000u);
  const uint32_t bits = m.L8(kBitsPerBlock + Rlwinm(t32, 1, 25, 30));
  const uint32_t r30 = bh;
  const uint32_t r28 = Rlwinm(bits * bw * bh, 29, 3, 31);  // bytes per block
  uint32_t r19 = Rlwinm(t28, 15, 18, 26) / bw;               // the pitch in blocks
  m.S32(F(152), r28);
  uint32_t base = t32 & 0xFFFFF000u;
  if (a.level != 0 || face != 0) {
    CallGuest(c.t, &__imp__sub_82547E20, c.ppc, c.base,
              {tex, face & ~(slices - 1), a.level, F(156), F(192), F(176), F(168), F(132)},
              {{84, F(128)}, {92, F(160)}, {100, F(140)}});
    base = m.L32(F(140));
    r19 = r28 ? m.L32(F(156)) / r28 : 0;  // Direct3D traps on 0 (twllei)
    r24 = m.L32(F(128));
  }
  uint32_t rect = a.rect;
  if (!rect) {  // the whole destination
    rect = F(192);
    m.S32(F(204), r24);
    m.S32(F(192), 0);
    m.S32(F(196), 0);
    m.S32(F(200), m.L32(F(132)));
  }
  const uint32_t point = a.point ? a.point : kDefaultPoint;
  // The copy registers.
  const uint32_t px = m.L32(point), py = m.L32(point + 4), left = m.L32(rect), top = m.L32(rect + 4);
  const uint32_t r7 = Rlwinm(Rlwinm(base, 12, 20, 31) + 512, 0, 19, 19);
  const uint32_t dest = ((py - top) * r19 + Rlwinm(px - left, 5, 0, 26)) * r28 + r7 + (base & 0x1FFFFFFFu);
  const uint32_t r26 = (r30 ? (r24 - py) / r30 : 0) + top;
  uint32_t fmt = t32 & 0x3F;
  uint32_t r27 = Rlwinm(flags, 6, 26, 31);
  const uint32_t r14 = Rlwinm(t28, 30, 30, 31);
  const uint32_t r25 = (t40 & 0xE) != 0 ? 1u : 0u;
  switch (fmt) {
    case 54: fmt = 7; break;
    case 55: fmt = 16; break;
    case 56: fmt = 17; break;
    case 27: fmt = 30; break;
    case 28: fmt = 31; break;
    case 29: fmt = 32; break;
    case 22: case 23: case 50: fmt = 6; break;
    default: break;
  }
  uint32_t sample = IsZero(r14 - 1) + ((t40 & 1) == 1 ? 2u : 0u);
  if (SpecialFormat(fmt)) sample = 7;
  r27 = Rlwimi(r27, r25, 8, 23, 23);
  sample = Rlwimi(sample, r27 & 319, 3, 0, 28);
  uint32_t info = Rlwinm(Rlwimi(fmt, sample, 6, 0, 25), 7, 0, 24) | Rlwinm(t32, 26, 30, 31);
  const uint32_t pitch = Rlwinm(r26, 16, 2, 15) | (r19 & 0x3FFF);
  if ((t48 & 0x600) == 1024) {
    m.S32(d + 10820, r19 * r24);
    m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | 4);
    info |= Rlwinm((slices - 1) & face, 4, 25, 27) | 8;
  }
  const uint32_t kind = (flags & 0x2000) ? 3u : src == 4 ? 0u : fmt == 61 ? 2u : 1u;
  m.S32(d + 10788, info);
  m.S32(d + 10780, dest);
  m.S32(d + 10784, pitch);
  const uint32_t control = Rlwinm(kind, 20, 10, 11) | ((flags & 887) - 16);
  m.S32(d + 10776, control);
  m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | 15360);
  if (fmt == 61) {
    uint32_t w0 = 4369, w1 = 0x80808080u, w2 = 0xFFFFFFFFu;
    if (const uint32_t p = a.params) {
      const uint32_t r8 = Rlwinm(Rlwinm(Rlwimi(m.L32(p + 20), m.L32(p + 24), 4, 25, 27), 4, 21, 27), 0, 25, 23);
      w0 = Rlwinm(r8 | (m.L32(p + 16) & 7), 4, 0, 27) | (m.L32(p + 12) & 7);
      w1 = (uint32_t(m.L8(p + 28)) << 24) | (uint32_t(m.L8(p + 31)) << 16) | (uint32_t(m.L8(p + 30)) << 8) | m.L8(p + 29);
      w2 = (uint32_t(m.L8(p + 32)) << 24) | (uint32_t(m.L8(p + 35)) << 16) | (uint32_t(m.L8(p + 34)) << 8) | m.L8(p + 33);
    }
    m.S32(d + 10808, w0);
    m.S32(d + 10812, w1);
    m.S32(d + 10816, w2);
    m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | 60 | 30 | 15);
  }
  // The window scissor over the copied rect (8-aligned right / bottom) when the current one does not cover it.
  const int32_t rl = int32_t(left), rt = int32_t(top);
  const int32_t rr = int32_t((m.L32(rect + 8) + 7) & ~7u), rb = int32_t((m.L32(rect + 12) + 7) & ~7u);
  const Window win = ReadWindow(m, d);
  const bool moved = rl < win.x0 || rt < win.y0 || rr > win.x1 || rb > win.y1;
  if (moved) WindowScissorOps(c.ops, uint32_t(rl), uint32_t(rt), uint32_t(rr), uint32_t(rb));
  PartialFlush(c);
  if (m.L8(d + dev::kFlags70) & 0x08) {
    for (int i = 0; i < 3; ++i) c.ops.Type3(0xC0004600u, 0, {6});
    m.S8(d + dev::kFlags70, uint8_t(m.L8(d + dev::kFlags70) & 0xF7));
  }
  if (!keep_setup) {  // INVALIDATE_STATE, the copy shader (IM_LOAD_IMMEDIATE: vertex, 15 words), its program control
    c.ops.Type3(0xC0003B00u, 0, {0x100});
    c.ops.Type3Guest(0xC0102B00u, 0, 0, 15, m.Host(kCopyShader), 15);
    c.ops.Regs(0x2180, {0x00010002u, 0});
  }
  c.ops.Reg(0x2208, 6);
  c.ops.Reg(0x2200, 0);
  if (!keep_setup || (m.L8(d + dev::kFlags72) & 0x80)) Sub8254F6A8(c);
  Sub825555A0(c, 256, 0);
  if (!keep_setup) {
    c.ops.Regs(0x2100, {0xFFFF, 0, 0});
    c.ops.Reg(0x2293, 0);
    c.ops.Regs(0x2204, {0x10000, 0x10000, 0x300});
    c.ops.Reg(0x2312, 0xFFFF);
    c.ops.Reg(0x200D, 0);
  }
  if (!keep_dirty) {  // what the copy changed, re-sent with the next draw
    m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | (1ull << 41) | (1ull << 40) | (1ull << 39));
    m.S64(d + dev::kDirtyFetch, m.L64(d + dev::kDirtyFetch) | (1ull << 35));
    m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 128 | 64 | 32);
    m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | 0x80000);
    m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | (1ull << 44));
  }
  if (r14 == 3 && src != 4) {  // the color target's info register with its format bits cleared, marked dirty
    const uint32_t k = src == 0 ? 0 : src + 1;
    const uint32_t reg = m.L32(d + (k + 2593) * 4);
    if (Rlwinm(reg, 0, 12, 15) == 0x10000) {
      c.ops.Reg(0x2001 + k, Rlwinm(reg & 0x03FFFFFFu, 0, 20, 11));
      m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | ((1ull << 63) >> (k + 7)));
    }
  }
  // The rect's three vertices (x, y), from Direct3D's inline-data allocator.
  const uint32_t data = AllocInline(c.t, c.base, c.ppc, d, 6, 4, true);
  if (!data) return false;
  const float bias = std::bit_cast<float>(m.L32((m.L32(d + 10688) & 1) ? kBiasCenter : kBiasHalf));
  const float v0 = float(rl) - bias, v1 = float(rt) - bias;
  const float v2 = float(int32_t(uint32_t(rr) - uint32_t(rl))) + v0;
  const float v5 = float(int32_t(uint32_t(rb) - uint32_t(rt))) + v1;
  const float vtx[6] = {v0, v1, v2, v1, v2, v5};
  for (uint32_t k = 0; k < 6; ++k) m.S32(data + 4 * k, std::bit_cast<uint32_t>(vtx[k]));
  c.ops.Regs(0x4800, {Physical(data) | 3u, 0x1000001Au, 0, 0, 0, 0});
  // The draw (rect list, 3 vertices, auto index), the event, register 0x2007, the window back.
  c.ops.Type3(m.L32(d + 13092) | 0xC0003600u, 0, {Rlwimi(136u, 3u, 16, 0, 15)});
  c.ops.Type3(0xC0004600u, 0, {6});
  c.ops.Reg(0x2007, m.L32(d + dev::kReg2007));
  if (moved) WindowScissorOps(c.ops, uint32_t(win.x0), uint32_t(win.y0), uint32_t(win.x1), uint32_t(win.y1));
  m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 0x80000);
  m.S64(d + dev::kDirtyFetch, m.L64(d + dev::kDirtyFetch) | 0x80000000ull);
  m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 0x10000 | 0x8000);
  m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 8 | 2048 | 256);
  // The destination's extent, for the coherency wait.
  uint32_t fa, fb, fc;
  FetchDims(m, tex + 28, fa, fb, fc);
  const uint32_t r9 = Rlwinm(Rlwinm(t28, 10, 23, 31) * r28, 5, 0, 26);
  CallGuest(c.t, &__imp__sub_82547378, c.ppc, c.base,
            {fa, fb, fc, Rlwinm(t44, 26, 28, 31) + 1, t32 & 0x3F, Rlwinm(t48, 23, 30, 31), Rlwinm(t28, 1, 31, 31),
             Rlwinm(t48, 21, 31, 31)},
            {{84, Rlwinm(t40, 1, 31, 31)}, {92, uint32_t(int32_t(Rlwinm(t40, 13, 0, 18)) >> 26)}, {100, r9},
             {108, F(192)}, {116, F(160)}, {124, F(136)}});
  uint32_t size, phys;
  if (a.level != 0 && (t48 & 0xFFFFF000u)) {
    size = m.L32(F(136));
    phys = Rlwinm(Rlwinm(t48, 12, 20, 31) + 512, 0, 19, 19) + Rlwinm(t48, 0, 3, 19);
  } else {
    size = m.L32(F(160));
    const uint32_t b = t32 & 0xFFFFF000u;
    phys = Rlwinm(Rlwinm(b, 12, 20, 31) + 512, 0, 19, 19) + (b & 0x1FFFFFFFu);
  }
  if (m.L32(kGlobalCoherency) != 0) {
    if (!keep_dirty) {  // COHER_STATUS / SIZE / BASE, the wait for the copy's memory, WAIT_UNTIL
      c.ops.Reg(0x0A31, 0x03000000u);
      c.ops.Regs(0x0A2F, {(size + 8191) & 0xFFFFF000u, phys & 0xFFFFF000u});
      c.ops.Type3(0xC0043C00u, 0, {3, 0x0A31, 0, 0x80000000u, 8});
      c.ops.Reg(0x05C8, 0x00020000u);
    }
  } else if (!keep_dirty) {
    m.S8(d + dev::kFlags72, uint8_t(m.L8(d + dev::kFlags72) | 0xC0));
    m.S64(d + dev::kDirtyMisc, m.L64(d + dev::kDirtyMisc) | 0xC000000000000000ull);
  }
  m.S32(d + 10776, 0);
  m.S32(d + 10788, 0);
  c.ops.Reg(0x2318, 0);
  c.ops.Reg(0x231B, 0);
  return true;
}

bool ResolveEntry(PPCContext& ctx, uint8_t* base) {
  ThreadData* const tp = Enabled(g_api_resolves, ctx);
  if (!tp) return false;
  ThreadData& t = *tp;
  Direct m{base};
  Args a{};
  a.d = ctx.r3.u32;
  a.flags = ctx.r4.u32;
  a.rect = ctx.r5.u32;
  a.r6 = ctx.r6.u32;
  a.r7 = ctx.r7.u32;
  a.tex = ctx.r8.u32;
  a.point = ctx.r9.u32;
  a.level = ctx.r10.u32;
  const uint32_t sp = ctx.r1.u32;  // the stack arguments, in the caller's frame
  a.face = m.L32(sp + 84);
  a.color = m.L32(sp + 92);
  a.stencil = m.L32(sp + 108);
  a.params = m.L32(sp + 116);
  const uint32_t d = a.d;
  ResolveFallback fb = kRfCount;
  if ((m.L8(d + dev::kFlags68) & 0x30) || (m.L8(d + dev::kFlags71) & 0x20)) fb = kRfTiling;
  else if (a.r6 || a.r7) fb = kRfVariant;
  else if (a.flags & 0x300) fb = kRfClear;
  else if (m.L32(d + dev::kFenceNext) == 0) fb = kRfFenceList;
  if (fb != kRfCount) [[unlikely]] {
    Bump(g_resolve_fallbacks[fb]);
    return false;
  }
  Shadow shadow;
  if (sa2::enh::VerifyMode()) [[unlikely]]
    shadow = RunShadow(t, base, ctx, d, &__imp__sub_825645A0, {{a.tex, 52}});
  Ops ops = OpenOps(t);
  Ctx<Direct> c{m, ops, base, ctx, d, Scratch(), t};
  const uint32_t submissions = m.L32(d + 11088);
  const bool drawn = ResolveOps(c, a);
  const uint32_t w = MarkerSpace(t, base, ctx, d);
  SetPacketAddresses(ops, w + 16);
  uint32_t seq = 0;
  if (!Publish(seq, ops)) return true;
  rex::memory::store_and_swap<uint32_t>(HostOf(base, d + dev::kWptr), PutMarker(base, w, seq));
  if (drawn) {  // the destination waits for the submission that carries the copy
    const uint32_t fence = m.L32(d + dev::kFenceNext);
    m.S32(a.tex + 8, fence);
    m.S32(a.tex + 12, fence);
  }
  Bump(g_resolves);
  g_api_resolves.UseSampled();
  if (shadow.ok) {
    const bool submitted = m.L32(d + 11088) != submissions;
    // The destination's fence follows the native side's own submission (its marker's space), like the device's.
    if (submitted)
      for (Region& x : shadow.regions)
        if (x.a == a.tex && x.n >= 16) std::memcpy(x.original.data() + 8, HostOf(base, a.tex + 8), 8);
    CompareShadow(t, "Resolve", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + dev::kWptr)), submitted, {});
  }
  return true;
}

}  // namespace

bool Resolve(PPCContext& ctx, uint8_t* base) { return Timed([&] { return ResolveEntry(ctx, base); }); }

std::string ResolveReport() {
  std::string s = fmt::format("{} resolves", g_resolves.load(std::memory_order_relaxed));
  for (uint32_t i = 0; i < kRfCount; ++i)
    if (const uint64_t n = g_resolve_fallbacks[i].load(std::memory_order_relaxed))
      s += fmt::format(" ({} {} by the original)", n, kResolveFallbackNames[i]);
  return s;
}

}  // namespace sa2::api
