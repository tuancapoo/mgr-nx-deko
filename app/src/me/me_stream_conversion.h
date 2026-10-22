#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <simde/arm/neon.h>

namespace me::native {
inline uint32_t Repack1011(uint32_t v, uint32_t format, bool sign) {
  return ((v >> (format == 16 ? 1 : 0)) & 0x3ffu) |
         (((v >> (format == 16 ? 12 : 11)) & 0x3ffu) << 10) |
         (((v >> 22) & 0x3ffu) << 20) | ((sign ? 1u : 3u) << 30);
}

// UINT32 output preserves the legacy draw/base-vertex contract, including a
// custom restart and 24-bit wrapping displacement. The source is read exactly
// count*2 bytes for orders 0/1. Paired orders 2/3 use the legacy i^1 mapping.
template<unsigned Order>
inline void DecodeIndices16Wide(const uint8_t* data, uint32_t count, uint32_t* output,
                                uint32_t displacement, bool restart, uint32_t reset,
                                uint32_t& minimum, uint32_t& maximum) {
  auto mn = simde_vdupq_n_u32(UINT32_MAX), mx = simde_vdupq_n_u32(0);
  const auto bias = simde_vdupq_n_u32(displacement);
  const auto mask = simde_vdupq_n_u32(0xffffff);
  const auto sentinel = simde_vdupq_n_u32(reset);
  const auto ones = simde_vdupq_n_u32(UINT32_MAX), zero = simde_vdupq_n_u32(0);
  uint32_t i = 0;
  for (; count - i >= 8; i += 8) {
    auto bytes = simde_vld1q_u8(data + size_t(i) * 2);
    if constexpr (Order == 1) bytes = simde_vrev16q_u8(bytes);
    if constexpr (Order == 2) bytes = simde_vrev32q_u8(bytes);
    if constexpr (Order == 3)
      bytes = simde_vreinterpretq_u8_u16(simde_vrev32q_u16(simde_vreinterpretq_u16_u8(bytes)));
    const auto half = simde_vreinterpretq_u16_u8(bytes);
    auto write = [&](auto raw, uint32_t* dst) {
      const auto v = simde_vandq_u32(simde_vaddq_u32(raw, bias), mask);
      const auto reset_mask = restart ? simde_vceqq_u32(raw, sentinel) : zero;
      const auto result = simde_vbslq_u32(reset_mask, ones, v);
      simde_vst1q_u32(dst, result);
      mn = simde_vminq_u32(mn, result);
      mx = simde_vmaxq_u32(mx, simde_vbicq_u32(v, reset_mask));
    };
    write(simde_vmovl_u16(simde_vget_low_u16(half)), output + i);
    write(simde_vmovl_u16(simde_vget_high_u16(half)), output + i + 4);
  }
  minimum = simde_vminvq_u32(mn); maximum = simde_vmaxvq_u32(mx);
  for (; i < count; ++i) {
    uint16_t raw;
    std::memcpy(&raw, data + size_t(Order >= 2 ? (i ^ 1u) : i) * 2, 2);
    if constexpr (Order == 1 || Order == 2) raw = std::byteswap(raw);
    if (restart && raw == reset) { output[i] = UINT32_MAX; continue; }
    const uint32_t v = (uint32_t(raw) + displacement) & 0xffffff;
    output[i] = v;
    minimum = std::min(minimum, v); maximum = std::max(maximum, v);
  }
}

// A short, per-binding plan. Every word is loaded, endian-converted, optionally
// repacked and stored once. Unsupported/overlapping layouts retain the old path.
struct RepackStreamPlan {
  static constexpr uint32_t MaxWords = 64;
  uint32_t words = 0;
  std::array<uint32_t, MaxWords> shift{}, w{}, active{};
  std::array<uint32_t, MaxWords / 4> chunks{};
  bool Init(uint32_t stride) {
    if (!stride || stride % 16 || stride / 4 > MaxWords) return false;
    words = stride / 4;
    return true;
  }
  bool Add(uint32_t offset, uint32_t format, bool sign) {
    if (offset % 4 || offset / 4 >= words || (format != 16 && format != 17) || active[offset / 4])
      return false;
    const uint32_t at = offset / 4;
    active[at] = UINT32_MAX; shift[at] = format == 16 ? UINT32_MAX : 0;
    w[at] = (sign ? 1u : 3u) << 30;
    chunks[at / 4] = 1;
    return true;
  }
};

template<unsigned Order>
inline simde_uint8x16_t LoadRepackedBlock(const uint8_t* source, const RepackStreamPlan& plan, uint32_t phase) {
  const auto mask10 = simde_vdupq_n_u32(0x3ff);
    __builtin_prefetch(source + 640);
    auto bytes = simde_vld1q_u8(source);
    if constexpr (Order == 1) bytes = simde_vrev16q_u8(bytes);
    if constexpr (Order == 2) bytes = simde_vrev32q_u8(bytes);
    if constexpr (Order == 3)
      bytes = simde_vreinterpretq_u8_u16(simde_vrev32q_u16(simde_vreinterpretq_u16_u8(bytes)));
    const auto v = simde_vreinterpretq_u32_u8(bytes);
    if (!plan.chunks[phase / 4]) {
      return bytes;
    }
    const auto shift = simde_vld1q_u32(plan.shift.data() + phase);
    const auto right1 = simde_vshrq_n_u32(v, 1), right2 = simde_vshrq_n_u32(v, 2);
    const auto lo = simde_vandq_u32(simde_vbslq_u32(shift, right1, v), mask10);
    const auto mid = simde_vandq_u32(simde_vbslq_u32(shift, right2, right1), simde_vdupq_n_u32(0xffc00));
    const auto hi = simde_vandq_u32(right2, simde_vdupq_n_u32(0x3ff00000));
    const auto packed = simde_vorrq_u32(simde_vorrq_u32(lo, mid),
        simde_vorrq_u32(hi, simde_vld1q_u32(plan.w.data() + phase)));
    return simde_vreinterpretq_u8_u32(
        simde_vbslq_u32(simde_vld1q_u32(plan.active.data() + phase), packed, v));
}

template<unsigned Order>
inline void RepackBlock(const uint8_t* source, uint8_t* target, const RepackStreamPlan& plan, uint32_t phase) {
  simde_vst1q_u8(target, LoadRepackedBlock<Order>(source, plan, phase));
}

// Compare the exact converted output, not a hash or an assumption about guest
// writes. This also rejects a cached upload made with a different repack layout.
template<unsigned Order>
inline bool RepackedUploadEquals(const uint8_t* source, const uint8_t* uploaded, uint32_t words,
                                 const RepackStreamPlan& plan) {
  if (!plan.words || plan.words > RepackStreamPlan::MaxWords || plan.words % 4) return false;
  uint32_t i = 0, phase = 0;
  for (; words - i >= 4; i += 4) {
    const auto expected = LoadRepackedBlock<Order>(source + size_t(i) * 4, plan, phase);
    const auto equal = simde_vceqq_u8(expected, simde_vld1q_u8(uploaded + size_t(i) * 4));
    if (simde_vminvq_u8(equal) != 255) return false;
    phase += 4;
    if (phase == plan.words) phase = 0;
  }
  for (; i < words; ++i) {
    uint32_t value, actual;
    std::memcpy(&value, source + size_t(i) * 4, 4);
    std::memcpy(&actual, uploaded + size_t(i) * 4, 4);
    if constexpr (Order == 1) value = ((value & 0xff00ffu) << 8) | ((value >> 8) & 0xff00ffu);
    if constexpr (Order == 2) value = std::byteswap(value);
    if constexpr (Order == 3) value = (value << 16) | (value >> 16);
    if (plan.active[phase]) value = Repack1011(value, plan.shift[phase] ? 16 : 17, plan.w[phase] == (1u << 30));
    if (value != actual) return false;
    if (++phase == plan.words) phase = 0;
  }
  return true;
}

template<unsigned Order, unsigned StrideWords>
inline uint32_t CopyRepackedFixed(const uint8_t* source, uint8_t* target, uint32_t words, const RepackStreamPlan& plan) {
  uint32_t i = 0;
  for (; words - i >= StrideWords; i += StrideWords) {
#pragma unroll
    for (uint32_t phase = 0; phase < StrideWords; phase += 4)
      RepackBlock<Order>(source + size_t(i + phase) * 4, target + size_t(i + phase) * 4, plan, phase);
  }
  return i;
}

template<unsigned Order>
inline void CopyRepacked(const uint8_t* source, uint8_t* target, uint32_t words,
                         const RepackStreamPlan& plan) {
  uint32_t i = 0, phase = 0;
  switch (plan.words) {
    case 4: i = CopyRepackedFixed<Order, 4>(source, target, words, plan); break;
    case 8: i = CopyRepackedFixed<Order, 8>(source, target, words, plan); break;
    case 12: i = CopyRepackedFixed<Order, 12>(source, target, words, plan); break;
    case 16: i = CopyRepackedFixed<Order, 16>(source, target, words, plan); break;
  }
  for (; words - i >= 4; i += 4) {
    RepackBlock<Order>(source + size_t(i) * 4, target + size_t(i) * 4, plan, phase);
    phase += 4; if (phase == plan.words) phase = 0;
  }
  for (; i < words; ++i) {
    uint32_t v; std::memcpy(&v, source + size_t(i) * 4, 4);
    if constexpr (Order == 1) v = ((v & 0xff00ffu) << 8) | ((v >> 8) & 0xff00ffu);
    if constexpr (Order == 2) v = std::byteswap(v);
    if constexpr (Order == 3) v = (v << 16) | (v >> 16);
    if (plan.active[phase]) v = Repack1011(v, plan.shift[phase] ? 16 : 17, plan.w[phase] == (1u << 30));
    std::memcpy(target + size_t(i) * 4, &v, 4);
    if (++phase == plan.words) phase = 0;
  }
}
}  // namespace me::native
