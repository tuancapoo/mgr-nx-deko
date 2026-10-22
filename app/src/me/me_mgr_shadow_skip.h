#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace me::native {
// MGR's traced shadow pass: D24FS8 at tile 0x400, physical pitch
// 1040, 1024-square viewport. This is deliberately not SA2's >=1600 rule.
struct MgrShadowDraw {
  uint32_t pitch, msaa, depth_base, depth_format, mode, color_mask;
  uint32_t depth_control, primitive, count, scissor_tl, scissor_br, window_offset;
};
inline bool MgrShadowSurface(const MgrShadowDraw& d) {
  return d.depth_base == 0x400 && d.depth_format == 1 &&
         (d.mode == 4 || d.mode == 5) && d.msaa == 0 && d.pitch == 1040;
}
inline bool MgrShadowClearSurface(const MgrShadowDraw& d) {
  return (MgrShadowSurface(d) || (d.depth_base == 0x400 && d.depth_format == 1 &&
          d.mode == 5 && d.pitch == 520 && d.msaa == 2)) && d.primitive == 8;
}
inline bool MgrShadowGeometry(const MgrShadowDraw& d) {
  const bool triangles = d.primitive == 4 || d.primitive == 5 || d.primitive == 6 || d.primitive == 13;
  return MgrShadowSurface(d) && triangles && d.count && !d.color_mask &&
         (d.depth_control & 7) == 6 && ((d.depth_control >> 4) & 7) == 6 &&
         (d.scissor_tl & 0x3fff3fffu) == 0 && d.scissor_br == 0x04000400 && !d.window_offset;
}
class MgrShadowSkip {
 public:
  // Called only after the original clear successfully records/redirects.
  bool Cleared(uint64_t frame, const MgrShadowDraw& d,
               const std::optional<std::array<int32_t, 4>>& rect,
               std::optional<float> depth) {
    Start(frame);
    if (!MgrShadowClearSurface(d) || !depth || *depth != 0.0f || !rect) {
      Reset(); return false;
    }
    // A 4x depth tile is 40x8 guest pixels (80x16 physical samples).
    // Partial tiles need the full Xenos sample permutation; do not infer
    // their physical coverage from a scaled rectangle.
    if (d.msaa == 2 && (((*rect)[0] % 40) || ((*rect)[2] % 40) ||
                        ((*rect)[1] % 8) || ((*rect)[3] % 8))) {
      Reset(); return false;
    }
    // The game clears the 64-pixel right edge through the 1x view, and the
    // remaining 960 pixels through the 4x (2x2 sample) alias. Zero depth has
    // the same all-zero encoding in both. Track their UNION, in physical
    // sample coordinates. Only completely covered 16x16 cells are admitted.
    const int64_t scale = d.msaa == 2 ? 2 : 1;
    const auto clipped = [&](int i) { return std::clamp<int64_t>(int64_t((*rect)[i]) * scale, 0, 1024); };
    const int32_t x0 = int32_t((clipped(0) + 15) / 16), y0 = int32_t((clipped(1) + 15) / 16);
    const int32_t x1 = int32_t(clipped(2) / 16), y1 = int32_t(clipped(3) / 16);
    if (x1 > x0 && y1 > y0) {
      dirty_ = true;
      const uint64_t bits = (x1 == 64 ? UINT64_MAX : (uint64_t(1) << x1) - 1) &
                            ~((uint64_t(1) << x0) - 1);
      for (int32_t y = y0; y < y1; ++y) coverage_[y] |= bits;
    }
    ready_ = std::all_of(coverage_.begin(), coverage_.end(), [](uint64_t row) { return row == UINT64_MAX; });
    return ready_;
  }
  bool Skip(uint64_t frame, const MgrShadowDraw& d) {
    Start(frame);
    if (MgrShadowGeometry(d)) {
      if (ready_) return true;
      Reset();  // a geometry draw that executes invalidates partial zero coverage
      return false;
    }
    if (!MgrShadowClearSurface(d)) Reset();  // end of contiguous shadow pass
    return false;
  }
 private:
  void Reset() { ready_ = false; if (dirty_) coverage_.fill(0); dirty_ = false; }
  void Start(uint64_t frame) { if (frame != frame_) { frame_ = frame; Reset(); } }
  uint64_t frame_ = UINT64_MAX;
  bool ready_ = false;
  bool dirty_ = false;
  std::array<uint64_t, 64> coverage_{};
};
}  // namespace me::native
