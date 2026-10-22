// sa2-nx (2026-10-07): hand-written replacements of the Ninja chunk-model vertex loaders (2P race profile: their
// njCalcPoint calls are the game thread's largest single cost, 8.5 %, and every call went through the hook and the
// localized copies' register write-back and reload, two per vertex):
//
//   sub_8214CF10: positions AND normals (r3 = chunk list, r4 = vertex buffer, r5 = transform flag), 32-byte records:
//                 position at +0, normal at +12, word +24;
//   sub_8214D0F8: positions only (the records' first 12 bytes; type 35 also word +24).
//
// The originals walk the chunk list. A header word h (type = h & 0xFF, 255 ends the list; length = h >> 16 words after
// it) and a second word w (first record = w & 0xFFFF, count = w >> 16) are followed by the vertex data. M = the current
// matrix (pointer at 0x839C07DC, read once at the start; njCalcPoint reads its values at every call). Per type:
//   41 (vertex + normal, 24 bytes): with the flag set njCalcPoint(M, in, out, 0) for the position (and, in
//      sub_8214CF10, njCalcPoint(M, in + 12, out + 12, 1) for the normal), else the words copied; sub_8214CF10 stores
//      -1 at +24. Records follow each other from the first.
//   42 (41 + one float, 28 bytes): first ORs 2 into the flags word at 0x8389F71C (read once at the start, stored at
//      every type-42 or -35 chunk); as 41, sub_8214CF10 storing the float converted by fctidz at +24.
//   44 (skinned: vertex + normal + a weight word, 28 bytes): sub_8214EAF0 (normals) / sub_8214ED18 (positions), the
//      mode = (h >> 8) & 0x7F, the flag: the transformed vertex is scaled by (weight word >> 16) * the float at
//      0x820EABD0, then STORED (mode 0) or ADDED (modes 1 and 2; mode 2 also stores -1 at +24 in sub_8214EAF0) into
//      the record (weight word & 0xFFFF) after the chunk's first: the bones' contributions accumulate. Without the
//      flag the originals scale values left on their stack by earlier calls: DECLINED.
//   35 (vertex + diffuse word, 16 bytes): ORs 3 into the flags word, then sub_8214E9B8: the position as type 41 (or
//      copied), the word stored at +24; records follow each other.
//   Other types do nothing. Each function returns r3 = 0.
//
// Exactness:
//   * the transforms are njCalcPoint's own operations in its order (doubles, every multiply and FMA rounded to float
//     as the recompiled fmuls / fmadds), the weights the originals' fcfid / frsp / fmuls / fadds in their operand
//     order; the matrix is read once per call instead of once per vertex, so lists whose writes could overlap the
//     matrix, the flags word, the weight constant or the chunk list itself are DECLINED (an overlap would change what
//     the original reads later), as are ranges that wrap around 4 GB or cross the physical-view offset. Weighted
//     records may overlap each other: they are updated in the originals' order;
//   * fctidz as the recompiled code computes it (NaN -> 0x8000000000000000, saturated);
//   * the FPSCR flush mode is switched off when the originals execute a float instruction, so the caller sees the
//     same state;
//   * registers: only r3 (= 0). The originals leave r4-r12, f0-f13, CR6 and CTR as scratch (njCalcPoint's f1-f13
//     among them) and restore what they save (r23-r31, f31): callers compiled with interprocedural allocation treat
//     the scratch ones as clobbered (tools/call_site_liveness.py: their one call site each reads none). The guard's
//     kReturnInt compares r1, r3, the non-volatile registers, the FPSCR and the written memory. The stack frames the
//     originals build below r1 (saved registers, the 80-88 block, the callees' 88-112 scratch) are dead after return.
#pragma once

#include <atomic>
#include <climits>
#include <cmath>

#include <rex/logging.h>

#include "../sa2_hot_common.h"

namespace sa2::hot::vertex_chunks {

constexpr uint32_t kMatrixPointer = 0x839C07DCu;  // lis r10,-31844; lwz r30,2012(r10)
constexpr uint32_t kFlagsWord = 0x8389F71Cu;      // lis r27,-31862; lwz r29,-2276(r27)
constexpr uint32_t kWeightScale = 0x820EABD0u;    // lis r11,-32241 (0x820F0000); addi r10,r11,-21952; lfs f31,400(r10)
constexpr uint32_t kMaxChunks = 1u << 16;          // a longer list is declined (the original would loop on garbage)

enum Reason : uint8_t { kOk, kNoTransformSkinned, kOverlap, kRange, kLong, kReasons };

struct Plan {
  Reason why = kRange;
  bool fp = false;          // a float instruction runs: FPSCR flush mode off
  bool flags = false;       // a type-42 or -35 chunk: the flags word is stored
  uint32_t lo = 0, hi = 0;  // the records written (byte bounds; hi > lo when any)
};

// The walk both originals make (no side effects): what the call would write, and whether this version can do it.
inline Plan Scan(const uint8_t* base, const PPCContext& c, bool normals) {
  Plan p;
  const uint32_t list = c.r3.u32, out_base = c.r4.u32;
  const bool transform = c.r5.u32 != 0;
  uint32_t at = list;
  uint64_t lo = UINT64_MAX, hi = 0, list_end = uint64_t(list) + 4;
  const auto writes = [&](uint64_t a, uint64_t b) {  // [a, b): checked range, else false
    if (b > 0x100000000ull || PhysOff(uint32_t(a)) != PhysOff(uint32_t(b - 1))) return false;
    lo = std::min(lo, a);
    hi = std::max(hi, b);
    return true;
  };
  bool skinned = false;
  for (uint32_t n = 0;; ++n) {
    if (n > kMaxChunks) {
      p.why = kLong;
      return p;
    }
    if (uint64_t(at) + 8 > 0x100000000ull || PhysOff(at) != PhysOff(at + 7)) return p;
    const uint32_t h = Ld32(base, at);
    if ((h & 0xFF) == 255) {
      list_end = std::max(list_end, uint64_t(at) + 4);
      break;
    }
    const uint32_t len = h >> 16, type = h & 0xFF;
    const uint32_t w = Ld32(base, at + 4);
    const uint32_t count = w >> 16;
    const uint32_t first = ((w & 0xFFFF) << 5) + out_base;  // rlwinm r10,r11,5,11,26; add r10,r10,r25
    const uint64_t next = uint64_t(at) + 4 + uint64_t(len) * 4;
    if (type == 41 || type == 42 || type == 44 || type == 35) {
      if (type == 42 || type == 35) p.flags = true;
      if (count) {
        const uint32_t stride = type == 41 ? 24 : type == 35 ? 16 : 28;
        const uint64_t in_end = uint64_t(at) + 8 + uint64_t(count) * stride;
        if (in_end > 0x100000000ull || PhysOff(at) != PhysOff(uint32_t(in_end - 1))) return p;
        list_end = std::max(list_end, in_end);
        if (type == 44) {
          if (!transform) {
            p.why = kNoTransformSkinned;
            return p;
          }
          skinned = true;
          p.fp = true;  // the loop's first lfs
          const uint32_t mode = (h >> 8) & 0x7F;
          const uint32_t bytes = normals ? (mode == 2 ? 28 : 24) : 12;
          if (mode <= 2) {
            for (uint32_t i = 0; i < count; ++i) {
              const uint32_t ww = Ld32(base, at + 8 + i * 28 + 24);
              const uint64_t rec = uint64_t(first) + uint64_t(ww & 0xFFFF) * 32;
              if (uint64_t(first) + 0 > 0xFFFFFFFFull || !writes(rec & 0xFFFFFFFFull, (rec & 0xFFFFFFFFull) + bytes))
                return p;
              if (rec > 0xFFFFFFFFull) return p;  // add r11,r11,r10 wraps: declined
            }
          }
        } else {
          const uint32_t bytes = (normals || type == 35) ? 28 : 12;
          if (!writes(first, uint64_t(first) + uint64_t(count - 1) * 32 + bytes)) return p;
          if (transform || (normals && type == 42)) p.fp = true;
        }
      }
    }
    if (next + 4 > 0x100000000ull) return p;
    list_end = std::max(list_end, next + 4);
    at = uint32_t(next);
  }
  if (hi > lo) {
    if (hi - lo > (1u << 24)) return p;
    const auto overlaps = [&](uint64_t a, uint64_t b) { return a < hi && lo < b; };
    if (overlaps(list, list_end) || overlaps(kFlagsWord, uint64_t(kFlagsWord) + 4) ||
        (skinned && overlaps(kWeightScale, uint64_t(kWeightScale) + 4)) ||
        overlaps(kMatrixPointer, uint64_t(kMatrixPointer) + 4)) {
      p.why = kOverlap;
      return p;
    }
    if (transform) {
      const uint32_t m = Ld32(base, kMatrixPointer);
      if (uint64_t(m) + 48 > 0x100000000ull || PhysOff(m) != PhysOff(m + 47)) return p;
      if (overlaps(m, uint64_t(m) + 48)) {
        p.why = kOverlap;
        return p;
      }
    }
    p.lo = uint32_t(lo);
    p.hi = uint32_t(hi);
  }
  p.why = kOk;
  return p;
}

// Big-endian float access through a host pointer (the scan checked the ranges).
inline double LdF(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return double(f);  // lfs
}
inline void StF(uint8_t* p, double d) {
  const float f = float(d);  // stfs
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline uint32_t Ld32p(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void St32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline double R(double d) { return double(float(d)); }  // the single-precision rounding of fmuls / fadds

// The current matrix as njCalcPoint reads it (12 floats, row-major 3x4).
struct Matrix {
  double m[12];
};
inline Matrix LoadMatrix(const uint8_t* base) {
  Matrix M;
  const uint8_t* p = Raw(base, Ld32(base, kMatrixPointer));
  for (int i = 0; i < 12; ++i) M.m[i] = LdF(p + i * 4);
  return M;
}

struct V3 {
  double x, y, z;
};

// njCalcPoint (sub_82146ED8) for one point: its operations in its order (n_ninja_matrix.h).
inline V3 CalcPoint(const Matrix& M, const uint8_t* in, bool point) {
  const double* m = M.m;
  const double z = LdF(in + 8), y = LdF(in + 4), x = LdF(in + 0);
  const double f8 = R(m[2] * z);                    // fmuls f8,f10,f12
  const double f6 = R(m[5] * y);                    // fmuls f6,f9,f11
  const double f5 = R(m[9] * y);                    // fmuls f5,f7,f11
  const double f9 = R(std::fma(m[1], y, f8));       // fmadds f9,f3,f11,f8
  const double f8b = R(std::fma(m[4], x, f6));      // fmadds f8,f2,f4,f6
  const double f7 = R(std::fma(m[8], x, f5));       // fmadds f7,f1,f4,f5
  double f0 = R(std::fma(m[0], x, f9));             // fmadds f0,f0,f4,f9
  double f13 = R(std::fma(m[6], z, f8b));           // fmadds f13,f13,f12,f8
  double f12 = R(std::fma(m[10], z, f7));           // fmadds f12,f10,f12,f7
  if (point) {
    f0 = R(m[3] + f0);     // fadds f0,f11,f0
    f13 = R(m[7] + f13);   // fadds f13,f10,f13
    f12 = R(m[11] + f12);  // fadds f12,f9,f12
  }
  return {f0, f13, f12};
}
inline void Store(uint8_t* p, const V3& v) {
  StF(p + 0, v.x);
  StF(p + 4, v.y);
  StF(p + 8, v.z);
}

// fctidz exactly as the recompiled code computes it (the same expression), then stfiwx (the low word).
inline uint32_t FctidzLow(double d) {
  const int64_t v = std::isnan(d) ? int64_t(0x8000000000000000ULL)
                    : (d > double(LLONG_MAX)) ? LLONG_MAX
                                              : simde_mm_cvttsd_si64(simde_mm_load_sd(&d));
  return uint32_t(uint64_t(v));
}

// How often each function declined, by reason, logged at 1, 1024, 65536 and 2^20 declines.
inline std::atomic<uint32_t> g_declined[2][kReasons];
inline std::atomic<uint32_t> g_handled[2];
inline void NoteDecline(bool normals, Reason why) {
  const uint32_t n = g_declined[normals][why].fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t total = 0;
  for (int r = 1; r < kReasons; ++r) total += g_declined[normals][r].load(std::memory_order_relaxed);
  if (total == 1 || total == 1024 || total == 65536 || total == (1u << 20)) [[unlikely]]
    REXLOG_INFO("[hot] {}: {} lists declined (skinned without the transform flag {}, overlaps {}, ranges {}, long {}), "
                "{} done natively",
                normals ? "sub_8214CF10" : "sub_8214D0F8", total,
                g_declined[normals][kNoTransformSkinned].load(std::memory_order_relaxed),
                g_declined[normals][kOverlap].load(std::memory_order_relaxed),
                g_declined[normals][kRange].load(std::memory_order_relaxed),
                g_declined[normals][kLong].load(std::memory_order_relaxed),
                g_handled[normals].load(std::memory_order_relaxed));
  (void)n;
}

// Both functions. normals: sub_8214CF10 (positions, normals and word 24), else sub_8214D0F8 (positions).
inline bool Run(PPCContext& ctx, uint8_t* base, bool normals) {
  const Plan p = Scan(base, ctx, normals);
  if (p.why != kOk) {
    NoteDecline(normals, p.why);
    return false;
  }
  g_handled[normals].store(g_handled[normals].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  const bool transform = ctx.r5.u32 != 0;
  Matrix M{};
  if (transform && p.hi > p.lo) M = LoadMatrix(base);
  double scale = 0;
  bool scale_read = false;
  uint32_t flags = 0;
  bool flags_read = false;
  if (p.fp) ctx.fpscr.disableFlushMode();  // the originals' first float instruction comes before any of their stores
  const uint32_t out_base = ctx.r4.u32;
  uint32_t at = ctx.r3.u32;
  for (;;) {
    const uint32_t h = Ld32(base, at);
    if ((h & 0xFF) == 255) break;
    const uint32_t len = h >> 16, type = h & 0xFF;
    const uint32_t w = Ld32(base, at + 4);
    const uint32_t count = w >> 16;
    const uint32_t first = ((w & 0xFFFF) << 5) + out_base;
    if (type == 42 || type == 35) {  // ori r29,r29,2 (3); stw r29,-2276(r27)
      if (!flags_read) {
        flags = Ld32(base, kFlagsWord);
        flags_read = true;
      }
      flags |= type == 42 ? 2 : 3;
      St32(Raw(base, kFlagsWord), flags);
    }
    const uint8_t* in = Raw(base, at + 8);
    if ((type == 41 || type == 42) && count) {
      const uint32_t stride = type == 41 ? 24 : 28;
      uint8_t* o = Raw(base, first);
      for (uint32_t i = 0; i < count; ++i, in += stride, o += 32) {
        if (transform) {
          Store(o, CalcPoint(M, in, true));
          if (normals) Store(o + 12, CalcPoint(M, in + 12, false));
        } else {
          std::memcpy(o, in, normals ? 24 : 12);  // lwz/stw word pairs in order (no overlap: scanned)
        }
        if (normals) St32(o + 24, type == 41 ? 0xFFFFFFFFu : FctidzLow(LdF(in + 24)));
      }
    } else if (type == 35 && count) {  // sub_8214E9B8
      uint8_t* o = Raw(base, first);
      for (uint32_t i = 0; i < count; ++i, in += 16, o += 32) {
        if (transform) {
          Store(o, CalcPoint(M, in, true));
        } else {
          std::memcpy(o, in, 12);
        }
        St32(o + 24, Ld32p(in + 12));
      }
    } else if (type == 44 && count) {  // sub_8214EAF0 / sub_8214ED18 (the scan declined it without the flag)
      if (!scale_read) {
        scale = LdF(Raw(base, kWeightScale));
        scale_read = true;
      }
      const uint32_t mode = (h >> 8) & 0x7F;
      for (uint32_t i = 0; i < count; ++i, in += 28) {
        const V3 v = CalcPoint(M, in, true);
        V3 n{};
        if (normals) n = CalcPoint(M, in + 12, false);
        const uint32_t ww = Ld32p(in + 24);
        const double wt = R(double(int64_t(ww >> 16)) * scale);  // fcfid; frsp; fmuls f5,f6,f31
        const V3 wv{R(v.x * wt), R(v.y * wt), R(v.z * wt)};      // fmuls f0,f0,f5 ...
        uint8_t* rec = Raw(base, first + (ww & 0xFFFF) * 32);
        if (mode == 0) {
          Store(rec, wv);
          if (normals) Store(rec + 12, {R(n.x * wt), R(n.y * wt), R(n.z * wt)});
        } else if (mode == 1 || mode == 2) {
          const double rx = LdF(rec), ry = LdF(rec + 4), rz = LdF(rec + 8);
          Store(rec, {R(rx + wv.x), R(ry + wv.y), R(rz + wv.z)});  // fadds f6,f8,f0 ...: the record first
          if (normals) {
            const V3 wn{R(n.x * wt), R(n.y * wt), R(n.z * wt)};
            const double nx = LdF(rec + 12), ny = LdF(rec + 16), nz = LdF(rec + 20);
            Store(rec + 12, {R(nx + wn.x), R(ny + wn.y), R(nz + wn.z)});
            if (mode == 2) St32(rec + 24, 0xFFFFFFFFu);
          }
        }
      }
    }
    at = at + 4 + len * 4;
  }
  ctx.r3.u64 = 0;
  return true;
}

inline void WritesOf(const PPCContext& c, const uint8_t* base, Writes& w, bool normals) {
  const Plan p = Scan(base, c, normals);
  if (p.why != kOk) return;  // declined: nothing written by this version
  if (p.hi > p.lo) w.Add(p.lo, p.hi - p.lo);
  if (p.flags) w.Add(kFlagsWord, 4);
}

}  // namespace sa2::hot::vertex_chunks

namespace sa2::hot::n_vn_points_normals {  // sub_8214CF10
inline bool Native(PPCContext& ctx, uint8_t* base) { return vertex_chunks::Run(ctx, base, true); }
inline void Writes(const PPCContext& c, const uint8_t* base, sa2::hot::Writes& w) {
  vertex_chunks::WritesOf(c, base, w, true);
}
}  // namespace sa2::hot::n_vn_points_normals

namespace sa2::hot::n_vn_points {  // sub_8214D0F8
inline bool Native(PPCContext& ctx, uint8_t* base) { return vertex_chunks::Run(ctx, base, false); }
inline void Writes(const PPCContext& c, const uint8_t* base, sa2::hot::Writes& w) {
  vertex_chunks::WritesOf(c, base, w, false);
}
}  // namespace sa2::hot::n_vn_points
