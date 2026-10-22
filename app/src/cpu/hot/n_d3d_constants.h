// The XDK's two shader float-constant writers (Direct3D, called for every constant upload of every draw):
//   sub_8255D990: r3 = device, r4 = first constant, r5 = source (any alignment), r6 = count of 16-byte vectors,
//                 r7 = 64-bit dirty bits. Destination (r3 + (r4 + 376) * 16) & ~15; dirty mask at r3 + 8.
//   sub_8255D8B8: the same with (r4 + 120) and the dirty mask at r3 + 0.
// (Console profile 2026-10-06, cpu_on, gameplay at 1020 MHz: 1.3 % and 1.2 % of the game thread.)
//
// The originals copy four vectors per iteration and then one per iteration: lvlx128 + lvrx128 + vor128 is an
// unaligned 16-byte load at r11 (lvrx at r11 + 16 loads nothing when r11 is aligned), stvx128 an aligned store at
// r10 & ~15, r10 and r11 advance by 16 per vector; dcbt are hints. Then ld / or r7 / std on the dirty mask. Facts used:
//   * every address is the 32-bit part (ea = u32 + u32); the vector count is r6 (cmplwi, mtctr of r6 - 4k): a count
//     with nonzero upper 32 bits would make the 64-bit CTR huge, so the native DECLINES it;
//   * the destination of vector k is ((r3 + (r4 + first) * 16) & ~15) + 16 k, its source r5 + 16 k: for
//     non-overlapping ranges that is a plain copy of 16 * count bytes, in guest byte order; overlapping ranges are
//     DECLINED (the four-at-a-time loop loads before it stores), as are ranges that wrap around 4 GB or cross the
//     physical-view offset boundary;
//   * the dirty mask is updated after the copy (the same order here).
// Registers: the originals leave r4-r11, CTR, CR6 and v59-v63 as scratch (some only on some paths, so callers compiled
// with interprocedural allocation must treat them all as clobbered); the native writes no register at all.
#pragma once

#include "../sa2_hot_common.h"

namespace sa2::hot::d3d_constants {

struct Plan {
  uint32_t dst, src, mask;
  uint64_t bytes;
};

inline Plan PlanOf(const PPCContext& c, uint32_t first, uint32_t mask_offset) {
  return {(((c.r4.u32 + first) << 4) + c.r3.u32) & ~15u, c.r5.u32, c.r3.u32 + mask_offset, uint64_t(c.r6.u32) * 16};
}

inline bool Upload(PPCContext& ctx, uint8_t* base, uint32_t first, uint32_t mask_offset) {
  if (ctx.r6.u64 >> 32) return false;
  const Plan p = PlanOf(ctx, first, mask_offset);
  if (p.bytes) {
    if (uint64_t(p.dst) + p.bytes > 0x100000000ull || uint64_t(p.src) + p.bytes > 0x100000000ull) return false;
    if (PhysOff(p.dst) != PhysOff(uint32_t(p.dst + p.bytes - 1)) ||
        PhysOff(p.src) != PhysOff(uint32_t(p.src + p.bytes - 1)))
      return false;
    if (p.src < p.dst + p.bytes && p.dst < p.src + p.bytes) return false;
    std::memcpy(Raw(base, p.dst), Raw(base, p.src), size_t(p.bytes));
  }
  uint64_t m;
  std::memcpy(&m, Raw(base, p.mask), 8);
  m = __builtin_bswap64(__builtin_bswap64(m) | ctx.r7.u64);
  std::memcpy(Raw(base, p.mask), &m, 8);
  return true;
}

inline void WritesOf(const PPCContext& c, Writes& w, uint32_t first, uint32_t mask_offset) {
  const Plan p = PlanOf(c, first, mask_offset);
  if (p.bytes) {
    if (p.bytes > Writes::kMaxBytes) {
      w.overflow = true;
      return;
    }
    w.Add(p.dst, uint32_t(p.bytes));
  }
  w.Add(p.mask, 8);
}

}  // namespace sa2::hot::d3d_constants

namespace sa2::hot::n_8255D990 {
inline bool Native(PPCContext& ctx, uint8_t* base) { return d3d_constants::Upload(ctx, base, 376, 8); }
inline void Writes(const PPCContext& c, const uint8_t*, sa2::hot::Writes& w) {
  d3d_constants::WritesOf(c, w, 376, 8);
}
}  // namespace sa2::hot::n_8255D990

namespace sa2::hot::n_8255D8B8 {
inline bool Native(PPCContext& ctx, uint8_t* base) { return d3d_constants::Upload(ctx, base, 120, 0); }
inline void Writes(const PPCContext& c, const uint8_t*, sa2::hot::Writes& w) {
  d3d_constants::WritesOf(c, w, 120, 0);
}
}  // namespace sa2::hot::n_8255D8B8
