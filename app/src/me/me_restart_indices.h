#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <simde/arm/neon.h>

namespace me::native {
// A fixed 0xffff restart remains 0xffff in a Vulkan UINT16 index buffer.
// Restart entries must not enlarge the vertex upload range.
inline void DecodeRestart16(const uint8_t* data, uint32_t count, uint16_t* output,
                            bool swap, uint32_t& minimum, uint32_t& maximum) {
  auto mn = simde_vdupq_n_u16(0xffff);
  auto mx = simde_vdupq_n_u16(0);
  const auto sentinel = simde_vdupq_n_u16(0xffff);
  uint32_t i = 0;
  for (; count - i >= 8; i += 8) {
    auto bytes = simde_vld1q_u8(data + size_t(i) * 2);
    if (swap) bytes = simde_vrev16q_u8(bytes);
    const auto v = simde_vreinterpretq_u16_u8(bytes);
    simde_vst1q_u16(output + i, v);
    mn = simde_vminq_u16(mn, v);
    mx = simde_vmaxq_u16(mx, simde_vbicq_u16(v, simde_vceqq_u16(v, sentinel)));
  }
  minimum = simde_vminvq_u16(mn);
  maximum = simde_vmaxvq_u16(mx);
  for (; i < count; ++i) {
    uint16_t v;
    std::memcpy(&v, data + size_t(i) * 2, 2);
    if (swap) v = std::byteswap(v);
    output[i] = v;
    if (v != 0xffff) {
      minimum = std::min<uint32_t>(minimum, v);
      maximum = std::max<uint32_t>(maximum, v);
    }
  }
  if (minimum == 0xffff) minimum = UINT32_MAX;  // empty / restart-only draw, like the legacy scan
}
}  // namespace me::native
