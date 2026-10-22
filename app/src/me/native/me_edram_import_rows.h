#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <tuple>
#include <vector>

namespace me::native {

// A linear shader range may enclose gaps between equal-width tile rows. Only
// the rectangle's columns are transferred; gaps remain owned by their old view.
struct EdramImportRows {
  uint32_t pitch = 0, width = 0;
  constexpr bool includes(uint32_t offset) const {
    return !width || (pitch && offset % pitch < width);
  }
  constexpr bool valid(uint32_t start, uint32_t count) const {
    if (!count || start >= 2048 || count > 2048 - start) return false;
    return !width || (pitch && width <= pitch && start % pitch + width <= pitch &&
                      count >= width && (count - width) % pitch == 0);
  }
};

// Caller proves that these transfers are independent: distinct source/dest
// images, no restores, and depth-only imports that leave source stencil records
// unchanged (deferred stencil is carried as metadata, never fetched here).
// Adjacent physical rows from the same source and relative offset can then be
// drawn once with a rectangular scissor. No shader or ownership entry may write
// the holes in the enclosing linear range.
template <typename Span, typename Eligible>
size_t CoalesceEdramImportRows(std::vector<Span>& spans, uint32_t pitch, Eligible eligible) {
  if (!pitch || spans.size() < 2) return 0;
  std::sort(spans.begin(), spans.end(), [pitch](const Span& a, const Span& b) {
    if (a.source != b.source) return std::less<decltype(a.source)>{}(a.source, b.source);
    return std::tuple(int32_t(a.tile_source) - int32_t(a.tile), a.tile % pitch, a.count, a.tile) <
           std::tuple(int32_t(b.tile_source) - int32_t(b.tile), b.tile % pitch, b.count, b.tile);
  });
  const size_t before = spans.size();
  size_t used = 0;
  for (const Span& next : spans) {
    bool merged = false;
    if (used && eligible(next) && eligible(spans[used - 1])) {
      Span& prev = spans[used - 1];
      const uint32_t width = prev.row_width ? prev.row_width : prev.count;
      if (prev.source == next.source && prev.native_msaa_import == next.native_msaa_import &&
          width && width <= pitch && next.count == width &&
          prev.tile % pitch + width <= pitch &&
          prev.tile_source % pitch + width <= pitch &&
          next.tile == prev.tile + prev.count - width + pitch &&
          next.tile_source == prev.tile_source + prev.count - width + pitch &&
          prev.count <= 2048 - pitch &&
          EdramImportRows{pitch, width}.valid(prev.tile, prev.count + pitch) &&
          EdramImportRows{pitch, width}.valid(prev.tile_source, prev.count + pitch)) {
        prev.row_width = width;
        prev.count += pitch;
        merged = true;
      }
    }
    if (!merged) spans[used++] = next;
  }
  spans.resize(used);
  return before - used;
}
}  // namespace me::native
