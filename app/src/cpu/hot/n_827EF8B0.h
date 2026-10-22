// sub_827EF8B0 - XDK CRT memmove: r3 = dst, r4 = src, r5 = n; returns r3 = dst (all 64 bits).
//
// The original compares the low words SIGNED (cmpw r3,r4): equal -> return; dst < src -> tail call to memcpy
// (sub_827F05C0, a forward copy); dst > src -> backward copy (byte tail down to a 4-aligned dst end, words - assembled
// from bytes when src is unaligned - then bytes), r3/r4 run from dst + n / src + n back down, so r3 ends equal to dst.
// The forward copy equals memmove when dst <= src (unsigned) or the ranges do not overlap; the backward copy when
// dst >= src or no overlap. The two orders only disagree for ranges on both sides of 0x80000000: an overlap there is
// declined (the original runs). Also declined: garbage in the upper half of r5 (ctr = r5 + 1 is 64-bit) and ranges
// that wrap around 4 GB.
// Registers: r0, r4, r5, r7-r10, ctr, cr0/cr6 are scratch of an assembly routine; r3 ends as it started; the native
// writes no register.
#pragma once

#include "../sa2_hot_common.h"

namespace sa2::hot::n_827EF8B0 {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  if (ctx.r5.u64 >> 32) return false;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (dst == src || n == 0) return true;  // cmpw equal: beqlr; n == 0: every loop is skipped
  if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
  const bool overlap = dst < src ? src - dst < n : dst - src < n;
  if (overlap) {
    const bool forward = int32_t(dst) < int32_t(src);
    if (forward != (dst < src)) return false;  // signed and unsigned order disagree
  }
  std::memmove(Raw(base, dst), Raw(base, src), n);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, sa2::hot::Writes& w) { w.Add(ctx.r3.u32, ctx.r5.u32); }

}  // namespace sa2::hot::n_827EF8B0
