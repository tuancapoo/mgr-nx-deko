/**
 * @file        ppc/diet_ops.h
 * @brief       Codegen "diet" helpers: cheaper but bit-identical lowerings of single PPC instructions.
 *
 * Included from context.h. Each helper reproduces exactly the value the previous generated expression produced
 * (including the upper word of the 64-bit register image), so it can be switched on per option.
 */

#pragma once

#include <bit>
#include <cstdint>

#include <rex/platform/fpscr.h>

#include <simde/x86/sse2.h>

// PC diagnostic (sa2-nx, 2026-10-12): how often the any-mode helpers below run, and how often they take a cold path.
// A flush-mode switch saved is paid for with a test in every one of these calls, so the two counts decide whether the
// generator's fp_mode_lookahead pays in a given game. Counted only when the build defines REX_DIET_CENSUS_ON (a PC
// app does, for its game code; the Switch build and the unit tests do not): plain lossy counters the app defines.
#if defined(REX_DIET_CENSUS_ON) && !defined(__SWITCH__)
// conversions, compares, arithmetic, cold paths, float copy loads, float copy stores
extern "C" volatile unsigned long long g_rex_diet_census[6];
#define REX_DIET_CENSUS(k) (g_rex_diet_census[k] = g_rex_diet_census[k] + 1)
#else
#define REX_DIET_CENSUS(k) ((void)0)
#endif

namespace rex::ppc {

/// fctiwz: truncate toward zero to int32 with PPC saturation, NaN -> 0x80000000. Returned as the s64 register image
/// that the old expression produced: NaN -> 0x0000000080000000 (zero extended), everything else sign extended.
/// The old expression was `isnan ? 0x80000000U : v >= INT_MAX ? INT_MAX : simde_mm_cvttsd_si32(v)`, where simde's
/// portable version returns INT32_MIN for v <= INT32_MIN: that is exactly the saturating AArch64 fcvtzs (positive
/// overflow -> INT32_MAX, negative overflow -> INT32_MIN); only NaN (fcvtzs gives 0) needs the select.
inline int64_t fctiwz(double v) noexcept {
#if defined(__aarch64__)
  int32_t r;
  asm("fcvtzs %w0, %d1" : "=r"(r) : "w"(v));
  return __builtin_isnan(v) ? int64_t(0x80000000U) : int64_t(r);
#else
  if (__builtin_isnan(v))
    return int64_t(0x80000000U);
  if (v >= 2147483647.0)
    return INT32_MAX;
  return (v > -2147483648.0) ? int64_t(int32_t(v)) : int64_t(INT32_MIN);
#endif
}

/// sa2-nx, second CPU round (codegen option fcti_inline): the other three conversions to integer the same way.
///
/// fctidz: truncate toward zero to int64 with PPC saturation, NaN -> INT64_MIN. The old expression was
/// `isnan ? INT64_MIN : v > 2^63 ? INT64_MAX : simde_mm_cvttsd_si64(v)`. On AArch64 simde's conversion is a C cast,
/// which the compiler writes as the saturating `fcvtzs`: with the NaN select that is every value the old expression
/// gave there. On x86 `cvttsd2si` answers INT64_MIN for everything out of range, so the old expression gave
/// INT64_MIN for exactly v == 2^63 (its test is `>`); the Xbox 360 and the AArch64 build give INT64_MAX there, and
/// so does this helper: the one input where it differs from the old x86 code.
inline int64_t fctidz(double v) noexcept {
#if defined(__aarch64__)
  int64_t r;
  asm("fcvtzs %x0, %d1" : "=r"(r) : "w"(v));
  return __builtin_isnan(v) ? INT64_MIN : r;
#else
  if (__builtin_isnan(v))
    return INT64_MIN;
  if (v >= 9223372036854775808.0)
    return INT64_MAX;
  return (v >= -9223372036854775808.0) ? int64_t(v) : INT64_MIN;
#endif
}

/// fctid: round to int64 in the current rounding mode (the guest's: FPSCR.RN is mirrored into the host's) with PPC
/// saturation, NaN -> INT64_MIN. The old expression was
/// `isnan ? INT64_MIN : v > 2^63 ? INT64_MAX : simde_mm_cvtsd_si64(v)`. On x86 that is `cvtsd2si` (current mode,
/// INT64_MIN out of range) and so is this helper; it differs only, as fctidz, at v == 2^63 (INT64_MAX, as on the
/// 360). On AArch64 simde's portable version is `(int64_t)round(v)`: halves AWAY from zero (0.5 -> 1, 2.5 -> 3) in
/// every mode, where the 360 and x86 give 0 and 2. `frinti` (to an integral double in the current mode, no inexact
/// flag) and the saturating `fcvtzs` round as the 360 does: the helper CORRECTS that build.
inline int64_t fctid(double v) noexcept {
  if (__builtin_isnan(v)) [[unlikely]]
    return INT64_MIN;
#if defined(__aarch64__)
  double integral;
  asm("frinti %d0, %d1" : "=w"(integral) : "w"(v));
  int64_t r;
  asm("fcvtzs %x0, %d1" : "=r"(r) : "w"(integral));
  return r;
#else
  if (v >= 9223372036854775808.0)
    return INT64_MAX;
  return simde_mm_cvtsd_si64(simde_mm_set_sd(v));
#endif
}

/// fctiw: the same to int32, as the s64 register image the old expression produced (NaN -> 0x0000000080000000,
/// everything else sign extended). Old: `isnan ? 0x80000000U : v >= INT_MAX ? INT_MAX : simde_mm_cvtsd_si32(v)`;
/// on x86 this is that expression, on AArch64 the halves are corrected as for fctid.
inline int64_t fctiw(double v) noexcept {
  if (__builtin_isnan(v)) [[unlikely]]
    return int64_t(0x80000000U);
#if defined(__aarch64__)
  double integral;
  asm("frinti %d0, %d1" : "=w"(integral) : "w"(v));
  int32_t r;
  asm("fcvtzs %w0, %d1" : "=r"(r) : "w"(integral));
  return int64_t(r);
#else
  if (v >= 2147483647.0)
    return INT32_MAX;
  return int64_t(simde_mm_cvtsd_si32(simde_mm_set_sd(v)));
#endif
}

/// sa2-nx, third CPU round (codegen option div_inline): the integer divisions with the results the generated
/// expressions gave: 0 for a zero divisor, 0 for the signed overflow (INT_MIN / -1), the C quotient otherwise.
/// On AArch64 `udiv` / `sdiv` are defined for a zero divisor (they answer 0) and never trap, so the unsigned ones
/// are the instruction alone and the signed ones add a select for the overflow, which `sdiv` answers with INT_MIN.
/// The C expression needs its guards (division by zero is undefined there) and the compiler keeps them as a branch.
/// A divisor known at compile time keeps the C expression: the compiler makes a multiplication of it.
inline uint32_t divwu(uint32_t n, uint32_t d) noexcept {
#if defined(__aarch64__)
  if (!__builtin_constant_p(d)) {
    uint32_t q;
    asm("udiv %w0, %w1, %w2" : "=r"(q) : "r"(n), "r"(d));
    return q;
  }
#endif
  return d ? n / d : 0;
}
inline uint64_t divdu(uint64_t n, uint64_t d) noexcept {
#if defined(__aarch64__)
  if (!__builtin_constant_p(d)) {
    uint64_t q;
    asm("udiv %x0, %x1, %x2" : "=r"(q) : "r"(n), "r"(d));
    return q;
  }
#endif
  return d ? n / d : 0;
}
/// The result as the zero-extended word the old expression assigned to the 64-bit register.
inline uint32_t divw(int32_t n, int32_t d) noexcept {
#if defined(__aarch64__)
  if (!__builtin_constant_p(d)) {
    int32_t q;
    asm("sdiv %w0, %w1, %w2" : "=r"(q) : "r"(n), "r"(d));
    return uint32_t((n == INT32_MIN && d == -1) ? 0 : q);
  }
#endif
  return uint32_t((d && !(n == INT32_MIN && d == -1)) ? n / d : 0);
}
inline int64_t divd(int64_t n, int64_t d) noexcept {
#if defined(__aarch64__)
  if (!__builtin_constant_p(d)) {
    int64_t q;
    asm("sdiv %x0, %x1, %x2" : "=r"(q) : "r"(n), "r"(d));
    return (n == INT64_MIN && d == -1) ? 0 : q;
  }
#endif
  return (d && !(n == INT64_MIN && d == -1)) ? n / d : 0;
}

/// Codegen option fp_copy_pairs: lfs directly followed by stfs of the same register.
///
/// lfs without a mode: the value `double(float)` gives with the flush mode off, in either mode and without touching
/// the mode. Only a denormal single reads differently with the mode on; it is converted with integer arithmetic
/// (normalise the fraction, rebuild the double), so the function is pure and the compiler removes it when the
/// register is not read again.
inline double SingleToDoublePure(uint32_t bits) noexcept {
  REX_DIET_CENSUS(4);
  const uint32_t fraction = bits & 0x007FFFFFu;
  if ((bits & 0x7F800000u) == 0 && fraction != 0) [[unlikely]] {
    const int shift = __builtin_clz(fraction) - 8;  // 1..23: brings the leading one to bit 23
    const uint64_t exponent = uint64_t(1023 - 126 - shift);
    const uint64_t mantissa = uint64_t((fraction << shift) & 0x007FFFFFu) << 29;
    return std::bit_cast<double>((uint64_t(bits >> 31) << 63) | (exponent << 52) | mantissa);
  }
  return double(std::bit_cast<float>(bits));
}

/// stfs of the value such an lfs loaded: `float(double(single))` with the mode off is the single again, except
/// that the two conversions quiet a signalling NaN (they set the top fraction bit and keep the rest).
inline uint32_t QuietSingleBits(uint32_t bits) noexcept {
  REX_DIET_CENSUS(5);
  return (bits & 0x7FFFFFFFu) > 0x7F800000u ? (bits | 0x00400000u) : bits;
}

/// sa2-nx round 7 (codegen option fp_moves_modeless): the lfs/stfs conversions with the IEEE result, i.e. the one
/// the conversion gives with the host's flush mode off, whatever the mode is. VMX code runs with the mode on (the
/// 360's VMX flushes denormals); the scalar float loads and stores between its instructions used to switch it off and
/// on again around each of them, and on the A57 every switch is an `msr fpcr` that costs as much as dozens of
/// instructions (SA2's VMX-heavy game thread spent 20-26 % of its time right after one).
/// The flush mode can change a single <-> double conversion only for a denormal single input (lfs), or a result in
/// the single denormal range or a denormal double input (stfs). Those convert in the cold helpers below with the mode
/// cleared for that one instruction (the volatile asm barriers keep the conversion between the two mode writes).
namespace detail {
#if defined(__aarch64__) || defined(_M_ARM64)
#define REX_DIET_FP_BARRIER(x) __asm__ __volatile__("" : "+w"(x))
#elif defined(__x86_64__) || defined(_M_X64)
#define REX_DIET_FP_BARRIER(x) __asm__ __volatile__("" : "+x"(x))
#else
#define REX_DIET_FP_BARRIER(x) ((void)0)
#endif

[[gnu::noinline, gnu::cold]] inline double SingleToDoubleSlow(uint32_t bits) noexcept {
  using P = rex::platform::FPSCRPlatform;
  const auto saved = P::getcsr();
  P::setcsr(decltype(saved)(saved & ~decltype(saved)(P::FlushMask)));
  float f = std::bit_cast<float>(bits);
  REX_DIET_FP_BARRIER(f);
  double d = double(f);
  REX_DIET_FP_BARRIER(d);
  P::setcsr(saved);
  return d;
}

[[gnu::noinline, gnu::cold]] inline float DoubleToSingleSlow(double d) noexcept {
  using P = rex::platform::FPSCRPlatform;
  const auto saved = P::getcsr();
  P::setcsr(decltype(saved)(saved & ~decltype(saved)(P::FlushMask)));
  REX_DIET_FP_BARRIER(d);
  float f = float(d);
  REX_DIET_FP_BARRIER(f);
  P::setcsr(saved);
  return f;
}
// The plain single-precision operations with the mode cleared (the cold paths of the *SingleAnyMode helpers).
#define REX_DIET_SINGLE_SLOW(NAME, EXPR)                                        \
  [[gnu::noinline, gnu::cold]] inline double NAME(double a, double b) noexcept { \
    using P = rex::platform::FPSCRPlatform;                                     \
    const auto saved = P::getcsr();                                             \
    P::setcsr(decltype(saved)(saved & ~decltype(saved)(P::FlushMask)));         \
    REX_DIET_FP_BARRIER(a);                                                     \
    REX_DIET_FP_BARRIER(b);                                                     \
    double r = double(float(EXPR));                                             \
    REX_DIET_FP_BARRIER(r);                                                     \
    P::setcsr(saved);                                                           \
    return r;                                                                   \
  }
REX_DIET_SINGLE_SLOW(AddSingleSlow, a + b)
REX_DIET_SINGLE_SLOW(SubSingleSlow, a - b)
REX_DIET_SINGLE_SLOW(MulSingleSlow, a * b)
REX_DIET_SINGLE_SLOW(DivSingleSlow, a / b)
#undef REX_DIET_SINGLE_SLOW

template <typename CR>
[[gnu::noinline, gnu::cold]] inline void CompareSlow(CR& cr, double a, double b) noexcept {
  using P = rex::platform::FPSCRPlatform;
  const auto saved = P::getcsr();
  P::setcsr(decltype(saved)(saved & ~decltype(saved)(P::FlushMask)));
  REX_DIET_FP_BARRIER(a);
  REX_DIET_FP_BARRIER(b);
  cr.compare(a, b);
  P::setcsr(saved);
}
#undef REX_DIET_FP_BARRIER
}  // namespace detail

/// lfs: the single's bits to the double register value. A denormal single (exponent 0, fraction not 0) takes the
/// slow path; zero, normal, infinite and NaN inputs convert the same in either mode.
inline double SingleToDoubleAnyMode(uint32_t bits) noexcept {
  REX_DIET_CENSUS(0);
  if (uint32_t((bits & 0x7FFFFFFFu) - 1u) < 0x007FFFFFu) [[unlikely]] {
    REX_DIET_CENSUS(3);
    return detail::SingleToDoubleSlow(bits);
  }
  return double(std::bit_cast<float>(bits));
}

/// stfs: the double register value to the stored single. 0 < |d| < 2^-126 (FLT_MIN; this includes every denormal
/// double) takes the slow path: only there can the result be tiny. From FLT_MIN up, and for zero, infinity and NaN,
/// neither an input flush nor an output flush can apply, on AArch64 (tininess before rounding) or x86 (after).
inline float DoubleToSingleAnyMode(double d) noexcept {
  REX_DIET_CENSUS(0);
  const uint64_t a = std::bit_cast<uint64_t>(d) & 0x7FFFFFFFFFFFFFFFull;
  if (a - 1u < 0x3810000000000000ull - 1u) [[unlikely]] {
    REX_DIET_CENSUS(3);
    return detail::DoubleToSingleSlow(d);
  }
  return float(d);
}

/// sasrt-nx (codegen option fp_compares_modeless, 2026-10-12): the same idea for the scalar float instructions the
/// generator still switched the mode for between VMX instructions.
///
/// 0 < |d| < 2^-1022: a denormal double, the only kind of operand the flush mode reads differently (as zero).
inline bool IsDenormalDouble(double d) noexcept {
  const uint64_t a = std::bit_cast<uint64_t>(d) & 0x7FFFFFFFFFFFFFFFull;
  return a - 1u < 0x000FFFFFFFFFFFFFull;
}

/// fcmpu / fcmpo: a compare writes no float result, so the mode can only change it through an operand: unless one is
/// a denormal double the answer is the same in either mode. Those compare in the cold helper with the mode cleared.
template <typename CR>
inline void CompareAnyMode(CR& cr, double a, double b) noexcept {
  REX_DIET_CENSUS(1);
  if (IsDenormalDouble(a) || IsDenormalDouble(b)) [[unlikely]] {
    REX_DIET_CENSUS(3);
    detail::CompareSlow(cr, a, b);
    return;
  }
  cr.compare(a, b);
}

/// frsp: the stfs conversion followed by the lfs one, each exact in either mode (above).
inline double RoundToSingleAnyMode(double d) noexcept {
  return SingleToDoubleAnyMode(std::bit_cast<uint32_t>(DoubleToSingleAnyMode(d)));
}

/// sasrt-nx (codegen option fp_mode_lookahead, 2026-10-12): fadds / fsubs / fmuls / fdivs between VMX instructions,
/// with the result the instruction gives with the flush mode off, whatever the mode is: one or two scalar operations
/// in the middle of vector code used to cost a switch to FPU mode and one back (two `msr fpcr` on the A57).
/// The flush mode can change `double(float(a op b))` in three places only, and each is ruled out or sent to the cold
/// helper (the plain expression with the mode cleared):
///   - an operand that is a denormal double (the mode reads it as zero): tested first;
///   - the double result of the operation being tiny: the mode then returns a zero (AArch64 flushes on tininess
///     before rounding, x86 on a tiny inexact result), so a result that is not zero was not flushed and is the
///     same in either mode; a zero result is accepted only where it is exact (x + (-x), x - x, a zero factor, a zero
///     dividend) and recomputed otherwise;
///   - the rounding to single and the widening back: RoundToSingleAnyMode (above).
inline double AddSingleAnyMode(double a, double b) noexcept {
  REX_DIET_CENSUS(2);
  if (IsDenormalDouble(a) || IsDenormalDouble(b)) [[unlikely]] return detail::AddSingleSlow(a, b);
  const double d = a + b;
  if (d == 0.0 && a != -b) [[unlikely]] return detail::AddSingleSlow(a, b);
  return RoundToSingleAnyMode(d);
}
inline double SubSingleAnyMode(double a, double b) noexcept {
  REX_DIET_CENSUS(2);
  if (IsDenormalDouble(a) || IsDenormalDouble(b)) [[unlikely]] return detail::SubSingleSlow(a, b);
  const double d = a - b;
  if (d == 0.0 && a != b) [[unlikely]] return detail::SubSingleSlow(a, b);
  return RoundToSingleAnyMode(d);
}
inline double MulSingleAnyMode(double a, double b) noexcept {
  REX_DIET_CENSUS(2);
  if (IsDenormalDouble(a) || IsDenormalDouble(b)) [[unlikely]] return detail::MulSingleSlow(a, b);
  const double d = a * b;
  if (d == 0.0 && a != 0.0 && b != 0.0) [[unlikely]] return detail::MulSingleSlow(a, b);
  return RoundToSingleAnyMode(d);
}
inline double DivSingleAnyMode(double a, double b) noexcept {
  REX_DIET_CENSUS(2);
  if (IsDenormalDouble(a) || IsDenormalDouble(b)) [[unlikely]] return detail::DivSingleSlow(a, b);
  const double d = a / b;
  if (d == 0.0 && a != 0.0) [[unlikely]] return detail::DivSingleSlow(a, b);
  return RoundToSingleAnyMode(d);
}

/// fsel's test, FRA >= 0.0, on the bits: true for both zeros and every positive number and +infinity, false for every
/// NaN and every negative number, denormals included (which the flush mode would read as -0: true). The same truth
/// table as the comparison with the flush mode off, in either mode and without a float compare.
inline bool GeZeroAnyMode(double a) noexcept {
  const uint64_t b = std::bit_cast<uint64_t>(a);
  return b <= 0x7FF0000000000000ull || b == 0x8000000000000000ull;
}

}  // namespace rex::ppc
