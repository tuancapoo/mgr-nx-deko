#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <simde/arm/neon.h>

namespace me::native {

// Compare the bytes a vertex upload would produce, without allocating a temporary
// or hashing the guest range first. All bytes participate, including vector tails.
inline bool VertexUploadEquals(const uint8_t* source, const uint8_t* uploaded,
                               size_t bytes, uint32_t order) {
  if (order > 3 || bytes % 4) return false;
  if (!order) return std::memcmp(source, uploaded, bytes) == 0;
  size_t i = 0;
  for (; bytes - i >= 16; i += 16) {
    auto v = simde_vld1q_u8(source + i);
    if (order == 1) v = simde_vrev16q_u8(v);
    else if (order == 2) v = simde_vrev32q_u8(v);
    else v = simde_vreinterpretq_u8_u16(simde_vrev32q_u16(simde_vreinterpretq_u16_u8(v)));
    const auto equal = simde_vceqq_u8(v, simde_vld1q_u8(uploaded + i));
    if (simde_vminvq_u8(equal) != 255) return false;
  }
  for (; i < bytes; i += 4) {
    for (uint32_t b = 0; b < 4; ++b) {
      const uint32_t lane = order == 1 ? b ^ 1 : order == 2 ? 3 - b : b ^ 2;
      if (source[i + lane] != uploaded[i + b]) return false;
    }
  }
  return true;
}

}  // namespace me::native
