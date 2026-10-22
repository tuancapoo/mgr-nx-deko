#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace me::native {

// Source and destination must not overlap. Each expanded host vertex carries
// the same three guest corners; copy the triple once, then replicate it.
template <class IndexAt>
bool ExpandRectangleVertexBytes(const uint8_t* source, size_t source_bytes,
                                size_t stride, uint32_t vmin, uint32_t rectangles,
                                IndexAt index_at, uint8_t* target, size_t target_bytes) {
  if (!stride || stride > std::numeric_limits<size_t>::max() / 18 ||
      rectangles > target_bytes / (stride * 18)) return false;
  for (uint32_t q = 0; q < rectangles; ++q) {
    size_t offsets[3];
    for (uint32_t c = 0; c < 3; ++c) {
      const uint32_t index = index_at(q * 3 + c);
      if (index < vmin || source_bytes < stride ||
          size_t(index - vmin) > (source_bytes - stride) / stride) return false;
      offsets[c] = size_t(index - vmin) * stride;
    }
    uint8_t* out = target + size_t(q) * 18 * stride;
    for (uint32_t c = 0; c < 3; ++c)
      std::memcpy(out + c * stride, source + offsets[c], stride);
    for (uint32_t v = 1; v < 6; ++v)
      std::memcpy(out + size_t(v) * 3 * stride, out, 3 * stride);
  }
  return true;
}

}  // namespace me::native
