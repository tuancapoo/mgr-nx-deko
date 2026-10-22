// sub_827EFEA0 - XDK CRT memset: r3 = dst, r4 = value (its low byte), r5 = n; returns r3 = dst unchanged.
//
// The original (ori r6,r3,0; ctr = n + 1): stores single bytes until r6 is 4-aligned or n is used up, replicates the
// low byte of r4 into a word (rlwimi x2), then 16-byte blocks of four stw, then up to three stw, then up to three stb.
// Every store writes the low byte of r4 (or that byte replicated), each byte of [dst, dst + n) exactly once, nothing
// outside it, and nothing is read. So the effect is memset(dst, r4 & 0xFF, n).
// Facts used here:
//   * ctr = r5 + 1 is 64-bit: with garbage in the upper half of r5 the head loop could run past n and the 32-bit
//     counts would go wrong. Such a call is declined (the original runs);
//   * ranges that wrap around 4 GB are declined.
// Registers: r0, r4, r5, r6, ctr, cr0 are scratch of an assembly routine; the native writes no register.
#pragma once

#include "../sa2_hot_common.h"

namespace sa2::hot::n_827EFEA0 {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  if (ctx.r5.u64 >> 32) return false;
  const uint32_t dst = ctx.r3.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  if (uint64_t(dst) + n > 0x100000000ull) return false;
  std::memset(Raw(base, dst), int(ctx.r4.u8), n);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, sa2::hot::Writes& w) { w.Add(ctx.r3.u32, ctx.r5.u32); }

}  // namespace sa2::hot::n_827EFEA0
