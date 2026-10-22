// sa2-nx round 13 (2026-10-07): hand-written njCalcPoint and njMultiMatrix that write only what callers can see.
//
// The localized copies (n_82146ED8.h, n_82146F60.h) are exact by construction but copy every register the original
// touches in and out of the PPCContext on every call: 17 for njCalcPoint (34 loads and stores around ~40 operations),
// 15 for njMultiMatrix. tools/call_site_liveness.py over all direct call sites (1,800 for njCalcPoint, 529 for
// njMultiMatrix): after either call the callers read only r3-r10 / r5, r8-r10, registers neither function writes, and
// no float register. So these versions write only the memory result (and njMultiMatrix's r3 / r4 where the original
// changes them), with the originals' operations in their order: doubles, each multiply and FMA rounded to float as the
// recompiled fmuls / fmadds. The guard compares them with kReturnInt (r1, r3, the non-volatile registers, the FPSCR,
// the written memory).
#pragma once

#include <cmath>

#include "../sa2_hot_common.h"

namespace sa2::hot::ninja {

inline double LdF(const uint8_t* base, uint32_t a) {  // lfs
  const uint32_t v = Ld32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return double(f);
}
inline void StF(uint8_t* base, uint32_t a, double d) {  // stfs
  const float f = float(d);
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
}
inline double R(double d) { return double(float(d)); }  // the single-precision rounding of fmuls / fmadds / fadds

}  // namespace sa2::hot::ninja

// sub_82146ED8 - njCalcPoint(r3 = 3x4 matrix, r4 = point, r5 = out, r6 & 0xFF: 0 = point (+ translation), else
// vector). Every load precedes the three stores, so overlapping ranges give the original's result too.
namespace sa2::hot::n_calc_point {

inline void Native(PPCContext& ctx, uint8_t* base) {
  using namespace sa2::hot::ninja;
  ctx.fpscr.disableFlushMode();  // the original's first instruction is an lfs
  const uint32_t m = ctx.r3.u32, p = ctx.r4.u32, o = ctx.r5.u32;
  const double z = LdF(base, p + 8), y = LdF(base, p + 4), x = LdF(base, p);
  const double f8 = R(LdF(base, m + 8) * z);                       // fmuls f8,f10,f12
  const double f6 = R(LdF(base, m + 20) * y);                      // fmuls f6,f9,f11
  const double f5 = R(LdF(base, m + 36) * y);                      // fmuls f5,f7,f11
  const double f9 = R(std::fma(LdF(base, m + 4), y, f8));          // fmadds f9,f3,f11,f8
  const double f8b = R(std::fma(LdF(base, m + 16), x, f6));        // fmadds f8,f2,f4,f6
  const double f7 = R(std::fma(LdF(base, m + 32), x, f5));         // fmadds f7,f1,f4,f5
  double f0 = R(std::fma(LdF(base, m + 0), x, f9));                // fmadds f0,f0,f4,f9
  double f13 = R(std::fma(LdF(base, m + 24), z, f8b));             // fmadds f13,f13,f12,f8
  double f12 = R(std::fma(LdF(base, m + 40), z, f7));              // fmadds f12,f10,f12,f7
  if ((ctx.r6.u32 & 0xFF) == 0) {
    f0 = R(LdF(base, m + 12) + f0);    // fadds f0,f11,f0
    f13 = R(LdF(base, m + 28) + f13);  // fadds f13,f10,f13
    f12 = R(LdF(base, m + 44) + f12);  // fadds f12,f9,f12
  }
  StF(base, o + 0, f0);
  StF(base, o + 4, f13);
  StF(base, o + 8, f12);
}

}  // namespace sa2::hot::n_calc_point

// sub_82146F60 - njMultiMatrix(r3 = A, r4 = B, r5 = out): out = A * B (3x4). Returns at once if any pointer is 0.
// The original stores each element as soon as it is computed and reloads its inputs for the next one; when out IS A
// (or B) it first copies that matrix to -48(r1) and reads the copy (r3 / r4 = r1 - 48). So: out equal to A or B reads
// the values from before the call (a snapshot); out partly overlapping A or B, or A / B overlapping the -48(r1)
// scratch the copy would use, is DECLINED (the original runs).
namespace sa2::hot::n_multi_matrix {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  using namespace sa2::hot::ninja;
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, o = ctx.r5.u32;
  if (!a || !b || !o) return true;  // beqlr: nothing written
  const auto overlap = [](uint32_t x, uint32_t y) { return uint64_t(x) < uint64_t(y) + 48 && uint64_t(y) < uint64_t(x) + 48; };
  if ((a != o && overlap(a, o)) || (b != o && overlap(b, o))) return false;
  const uint32_t scratch = ctx.r1.u32 - 48;
  if ((a == o || b == o) && (overlap(a, scratch) || overlap(b, scratch))) return false;
  ctx.fpscr.disableFlushMode();
  double A[12], B[12];
  for (int i = 0; i < 12; ++i) A[i] = LdF(base, a + 4 * i);
  for (int i = 0; i < 12; ++i) B[i] = LdF(base, b + 4 * i);
  if (a == o) ctx.r3.s64 = ctx.r1.s64 - 48;  // addi r3,r1,-48 (as the original leaves it)
  if (b == o) ctx.r4.s64 = ctx.r1.s64 - 48;  // addi r4,r1,-48
  // Each element as the original computes it (the register comments name its operands).
  StF(base, o + 0, R(std::fma(B[8], A[2], R(std::fma(A[0], B[0], R(B[4] * A[1]))))));
  StF(base, o + 4, R(std::fma(B[9], A[2], R(std::fma(A[1], B[5], R(B[1] * A[0]))))));
  StF(base, o + 8, R(std::fma(A[1], B[6], R(std::fma(A[0], B[2], R(A[2] * B[10]))))));
  StF(base, o + 12, R(R(std::fma(A[2], B[11], R(std::fma(A[1], B[7], R(A[0] * B[3]))))) + A[3]));
  StF(base, o + 16, R(std::fma(A[4], B[0], R(std::fma(B[4], A[5], R(B[8] * A[6]))))));
  StF(base, o + 20, R(std::fma(B[9], A[6], R(std::fma(A[4], B[1], R(A[5] * B[5]))))));
  StF(base, o + 24, R(std::fma(A[5], B[6], R(std::fma(A[6], B[10], R(A[4] * B[2]))))));
  StF(base, o + 28, R(R(std::fma(A[6], B[11], R(std::fma(A[4], B[3], R(A[5] * B[7]))))) + A[7]));
  StF(base, o + 32, R(std::fma(A[8], B[0], R(std::fma(B[4], A[9], R(B[8] * A[10]))))));
  StF(base, o + 36, R(std::fma(B[9], A[10], R(std::fma(A[8], B[1], R(A[9] * B[5]))))));
  StF(base, o + 40, R(std::fma(A[9], B[6], R(std::fma(A[10], B[10], R(A[8] * B[2]))))));
  StF(base, o + 44, R(R(std::fma(A[10], B[11], R(std::fma(A[8], B[3], R(A[9] * B[7]))))) + A[11]));
  return true;
}

}  // namespace sa2::hot::n_multi_matrix
