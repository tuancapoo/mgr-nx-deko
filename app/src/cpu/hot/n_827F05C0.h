// sub_827F05C0 - XDK CRT memcpy: r3 = dst, r4 = src, r5 = n (32-bit), returns r3 = the ORIGINAL dst (all 64 bits).
// (masseffect-nx's n_82AC4AF0.h: the same CRT routine in Mass Effect.)
//
// The original saves r3 at -8(r1), copies FORWARD (byte head up to 8-byte dst alignment, then 128-byte blocks / dword /
// word / byte loops chosen by src alignment; dcbt/dcbtst are no-ops) and reloads r3 from -8(r1). Facts used here:
//   * every size / address use is the 32-bit part (cmplw, rlwinm, ea = u32 + u32); the upper halves of r4/r5 are
//     ignored, r3 is returned unchanged (so the native version leaves ctx.r3 alone);
//   * no loop reads past src + n or writes past dst + n;
//   * within each unit all loads precede its stores and units run upward, so for dst <= src the result is exactly
//     memmove, and for non-overlapping ranges a plain copy. When dst > src and dst - src < n the forward copy smears
//     already-copied bytes: the native version DECLINES (returns false, nothing modified) and the original runs.
//   * n == 0 is a no-op in the original (all loops are skipped).
// Ranges that wrap around 4 GB are declined for n > 64 (the original wraps on every access).
// Registers: the original leaves r4-r12, ctr, cr0/cr1/cr6/cr7 as scratch (an assembly routine: callers cannot rely
// on them); the native writes no register.
#pragma once

#include "../sa2_hot_common.h"

namespace sa2::hot::n_827F05C0 {

namespace detail {
struct B16 {
  uint8_t b[16];
};
inline B16 L16(const uint8_t* p) {
  B16 v;
  std::memcpy(v.b, p, 16);
  return v;
}
inline void S16(uint8_t* p, const B16& v) { std::memcpy(p, v.b, 16); }

// 1 <= n <= 64; all loads before all stores (memmove semantics for dst <= src overlap).
inline void CopySmall(uint8_t* d, const uint8_t* s, uint32_t n) {
  if (n >= 16) {
    if (n > 32) {
      const B16 a = L16(s), b = L16(s + 16), c = L16(s + n - 32), e = L16(s + n - 16);
      S16(d, a);
      S16(d + 16, b);
      S16(d + n - 32, c);
      S16(d + n - 16, e);
    } else {
      const B16 a = L16(s), e = L16(s + n - 16);
      S16(d, a);
      S16(d + n - 16, e);
    }
  } else if (n >= 8) {
    uint64_t a, e;
    std::memcpy(&a, s, 8);
    std::memcpy(&e, s + n - 8, 8);
    std::memcpy(d, &a, 8);
    std::memcpy(d + n - 8, &e, 8);
  } else if (n >= 4) {
    uint32_t a, e;
    std::memcpy(&a, s, 4);
    std::memcpy(&e, s + n - 4, 4);
    std::memcpy(d, &a, 4);
    std::memcpy(d + n - 4, &e, 4);
  } else if (n >= 2) {
    uint16_t a, e;
    std::memcpy(&a, s, 2);
    std::memcpy(&e, s + n - 2, 2);
    std::memcpy(d, &a, 2);
    std::memcpy(d + n - 2, &e, 2);
  } else {
    *d = *s;
  }
}
}  // namespace detail

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  if (uint32_t(dst - src - 1u) < n - 1u) return false;  // dst > src && dst - src < n: the original smears
  uint8_t* d = Raw(base, dst);
  const uint8_t* s = Raw(base, src);
  if (n <= 64) {
    detail::CopySmall(d, s, n);
    return true;
  }
  if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
  if (uint32_t(src - dst) < n)
    std::memmove(d, s, n);  // dst < src (or equal), overlapping: forward copy == memmove
  else
    std::memcpy(d, s, n);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, sa2::hot::Writes& w) {
  w.Add(ctx.r3.u32, ctx.r5.u32);  // > 1 MB sets overflow
}

}  // namespace sa2::hot::n_827F05C0
