// Native leaf for MGR sub_825B4070 (recursive float filter). Included by mgr_me_hooks.cpp.
// Arithmetic order, float rounding boundaries, four-sample load/store order and
// final PPC register images follow sa2_recomp.231.cpp exactly. In particular,
// fmadds remains double std::fma followed by float; replacing it by fmaf changes
// double-rounding/NaN behavior. No restrict promise is made for guest buffers.
#include <cmath>
#include <cstring>

namespace mgr::wwise {
inline uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t value;
#ifndef MGR_WWISE_DSP_TEST
  const auto offset = rex::memory::detail::PhysicalHostOffset(address);
#else
  constexpr size_t offset = 0;  // The standalone differential arena uses low virtual addresses.
#endif
  std::memcpy(&value, base + address + offset, sizeof(value));
  return __builtin_bswap32(value);
}
inline void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
#ifndef MGR_WWISE_DSP_TEST
  const auto offset = rex::memory::detail::PhysicalHostOffset(address);
#else
  constexpr size_t offset = 0;
#endif
  std::memcpy(base + address + offset, &value, sizeof(value));
}

// An integer-only upper bound on |float| * 2^32, rounded up. Its caller has
// already checked |float| <= 1. Avoid FP operations in the guard: it runs before
// the guest leaf changes flush mode and must not change host exception flags.
inline uint64_t FeedbackBound(uint32_t magnitude) {
  if (!magnitude) return 0;
  const unsigned exponent = magnitude >> 23;
  const uint64_t mantissa = (magnitude & 0x7fffff) | 0x800000;
  if (exponent >= 118) return mantissa << (exponent - 118);
  const unsigned shift = 118 - exponent;
  return shift >= 24 ? 1 : (mantissa + ((uint64_t(1) << shift) - 1)) >> shift;
}

// Guard before touching the PPC context or modifying guest memory. The finite
// domain prevents allocation-dependent NaN operand propagation. With |feedback|
// sum <= 1, |input/state| <= 2^64, feedforward coefficients <= 16 and <=4096
// samples, the recurrence stays far below float overflow even with directed
// rounding. Aliases retain the original four-sample read/store ordering.
inline bool CanFilter(const PPCContext& ctx, const uint8_t* base) {
  if (ctx.r6.u32 > 4096 || ((ctx.r3.u32 | ctx.r4.u32 | ctx.r5.u32) & 3u)) return false;
  if (ctx.r3.u32 > 0xfffffff0u || ctx.r4.u32 > 0xfffffff0u) return false;
  const uint32_t start = Read32(base, ctx.r5.u32);
  const uint64_t end = uint64_t(start) + uint64_t(ctx.r6.u32) * 4;
  if ((start & 3u) || end >= 0x80000000ull) return false;
  const uint32_t a = Read32(base, ctx.r4.u32 + 8) & 0x7fffffff;
  const uint32_t b = Read32(base, ctx.r4.u32 + 12) & 0x7fffffff;
  if (a > 0x3f800000 || b > 0x3f800000 ||
      FeedbackBound(a) + FeedbackBound(b) > (uint64_t(1) << 32)) return false;
  if ((Read32(base, ctx.r4.u32) & 0x7fffffff) > 0x41800000 ||
      (Read32(base, ctx.r4.u32 + 4) & 0x7fffffff) > 0x41800000) return false;
  for (unsigned i = 0; i < 4; ++i)
    if ((Read32(base, ctx.r3.u32 + i * 4) & 0x7fffffff) > 0x5f800000) return false;
  for (uint64_t at = start; at < end; at += 4)
    if ((Read32(base, uint32_t(at)) & 0x7fffffff) > 0x5f800000) return false;
  return true;
}

inline void Filter(PPCContext& ctx, uint8_t* base) {
  // Context memory is large and aliases guest accesses from the compiler's point
  // of view. Keep this leaf's working register set local and publish it once.
  ctx.fpscr.disableFlushMode();
  double f0 = ctx.f0.f64;
  double f1 = ctx.f1.f64;
  double f2 = ctx.f2.f64;
  double f3 = ctx.f3.f64;
  double f4 = ctx.f4.f64;
  double f5 = ctx.f5.f64;
  double f6 = ctx.f6.f64;
  double f7 = ctx.f7.f64;
  double f8 = ctx.f8.f64;
  double f9 = ctx.f9.f64;
  double f10 = ctx.f10.f64;
  double f11 = ctx.f11.f64;
  double f12 = ctx.f12.f64;
  double f13 = ctx.f13.f64;
  PPCRegister r7 = ctx.r7;
  PPCRegister r8 = ctx.r8;
  PPCRegister r9 = ctx.r9;
  PPCRegister r10 = ctx.r10;
  PPCRegister r11 = ctx.r11;
  auto cr6 = ctx.cr6;
  auto xer = ctx.xer;
  auto ctr = ctx.ctr;

  PPCRegister temp{};
  r11.u64 = Read32(base, ctx.r5.u32 + 0);
  r10.u64 = __builtin_rotateleft64(ctx.r6.u32 | (ctx.r6.u64 << 32), 2) & 0xFFFFFFFC;
  temp.u32 = Read32(base, ctx.r3.u32 + 0);
  f0 = double(temp.f32);
  r10.u64 = r10.u64 + r11.u64;
  temp.u32 = Read32(base, ctx.r4.u32 + 0);
  f12 = double(temp.f32);
  temp.u32 = Read32(base, ctx.r4.u32 + 4);
  f11 = double(temp.f32);
  temp.u32 = Read32(base, ctx.r4.u32 + 8);
  f10 = double(temp.f32);
  cr6.compare<uint32_t>(r11.u32, r10.u32, xer);
  temp.u32 = Read32(base, ctx.r3.u32 + 4);
  f8 = double(temp.f32);
  temp.u32 = Read32(base, ctx.r3.u32 + 8);
  f13 = double(temp.f32);
  temp.u32 = Read32(base, ctx.r3.u32 + 12);
  f7 = double(temp.f32);
  temp.u32 = Read32(base, ctx.r4.u32 + 12);
  f9 = double(temp.f32);
  if (!cr6.lt) goto loc_825B4198;
  r9.u64 = r10.u64 - r11.u64;
  r9.s64 = r9.s64 + 3;
  xer.ca = (r9.s32 < 0) & ((r9.u32 & 0x3) != 0);
  r8.s64 = r9.s32 >> 2;
  temp.s64 = r8.s64 + xer.ca;
  xer.ca = temp.u32 < r8.u32;
  r7.s64 = temp.s64;
  cr6.compare<int32_t>(r7.s32, 4, xer);
  if (cr6.lt) goto loc_825B414C;
  r9.s64 = r10.s64 + -12;
loc_825B40C0:
  temp.u32 = Read32(base, r11.u32 + 0);
  f6 = double(temp.f32);
  f5 = double(float(f11 * f0));
  f4 = double(float(f6 + f8));
  temp.u32 = Read32(base, r11.u32 + 8);
  f8 = double(temp.f32);
  f3 = f0;
  temp.u32 = Read32(base, r11.u32 + 4);
  f0 = double(temp.f32);
  f2 = double(float(std::fma(f4, f12, f5)));
  f1 = double(float(f0 + f3));
  f5 = double(float(f11 * f6));
  f4 = double(float(f8 + f6));
  f6 = f0;
  f3 = double(float(std::fma(f9, f7, f2)));
  f2 = double(float(std::fma(f1, f12, f5)));
  f1 = double(float(f11 * f0));
  temp.u32 = Read32(base, r11.u32 + 12);
  f0 = double(temp.f32);
  f5 = double(float(f11 * f8));
  f7 = double(float(std::fma(f10, f13, f3)));
  temp.f32 = float(f7);
  Write32(base, r11.u32 + 0, temp.u32);
  f3 = double(float(std::fma(f9, f13, f2)));
  f2 = double(float(std::fma(f4, f12, f1)));
  f1 = double(float(f0 + f6));
  f13 = double(float(std::fma(f10, f7, f3)));
  temp.f32 = float(f13);
  Write32(base, r11.u32 + 4, temp.u32);
  f6 = double(float(std::fma(f1, f12, f5)));
  f5 = double(float(std::fma(f9, f7, f2)));
  f4 = f13;
  f13 = double(float(std::fma(f10, f13, f5)));
  temp.f32 = float(f13);
  Write32(base, r11.u32 + 8, temp.u32);
  f3 = double(float(std::fma(f9, f4, f6)));
  f7 = f13;
  f13 = double(float(std::fma(f10, f13, f3)));
  temp.f32 = float(f13);
  Write32(base, r11.u32 + 12, temp.u32);
  r11.s64 = r11.s64 + 16;
  cr6.compare<int32_t>(r11.s32, r9.s32, xer);
  if (cr6.lt) goto loc_825B40C0;
loc_825B414C:
  cr6.compare<uint32_t>(r11.u32, r10.u32, xer);
  if (!cr6.lt) goto loc_825B4198;
  r10.u64 = r10.u64 - r11.u64;
  r10.s64 = r10.s64 + -1;
  r10.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 30) & 0x3FFFFFFF;
  r10.s64 = r10.s64 + 1;
  ctr.u64 = r10.u64;
loc_825B4168:
  temp.u32 = Read32(base, r11.u32 + 0);
  f6 = double(temp.f32);
  f5 = double(float(f11 * f0));
  f4 = double(float(f6 + f8));
  f8 = f0;
  f0 = f6;
  f3 = double(float(std::fma(f4, f12, f5)));
  f2 = double(float(std::fma(f9, f7, f3)));
  f7 = f13;
  f13 = double(float(std::fma(f10, f13, f2)));
  temp.f32 = float(f13);
  Write32(base, r11.u32 + 0, temp.u32);
  r11.s64 = r11.s64 + 4;
  --ctr.u64;
  if (ctr.u32 != 0) goto loc_825B4168;
loc_825B4198:
  temp.f32 = float(f0);
  Write32(base, ctx.r3.u32 + 0, temp.u32);
  Write32(base, ctx.r5.u32 + 0, r11.u32);
  temp.f32 = float(f8);
  Write32(base, ctx.r3.u32 + 4, temp.u32);
  temp.f32 = float(f13);
  Write32(base, ctx.r3.u32 + 8, temp.u32);
  temp.f32 = float(f7);
  Write32(base, ctx.r3.u32 + 12, temp.u32);

  ctx.f0.f64 = f0;
  ctx.f1.f64 = f1;
  ctx.f2.f64 = f2;
  ctx.f3.f64 = f3;
  ctx.f4.f64 = f4;
  ctx.f5.f64 = f5;
  ctx.f6.f64 = f6;
  ctx.f7.f64 = f7;
  ctx.f8.f64 = f8;
  ctx.f9.f64 = f9;
  ctx.f10.f64 = f10;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.f13.f64 = f13;
  ctx.r7 = r7;
  ctx.r8 = r8;
  ctx.r9 = r9;
  ctx.r10 = r10;
  ctx.r11 = r11;
  ctx.cr6 = cr6;
  ctx.xer = xer;
  ctx.ctr = ctr;
}
}  // namespace mgr::wwise

#ifndef MGR_WWISE_DSP_TEST
REXCVAR_DEFINE_BOOL(mgr_wwise_native_dsp, true, "MGR",
                    "Native bounded Wwise recursive filter; unsupported inputs or false use the original guest leaf");
REX_EXTERN(__imp__sub_825B4070);
REX_HOOK_RAW(sub_825B4070) {
  if (REXCVAR_GET(mgr_wwise_native_dsp) && mgr::wwise::CanFilter(ctx, base)) {
    mgr::wwise::Filter(ctx, base);
    return;
  }
  __imp__sub_825B4070(ctx, base);
}
#endif
