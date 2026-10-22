// The small VMX helper operations of rex/ppc/intrinsics.h against the NEON instruction each one stands for, on a PC:
// the header's x86 form on one side, a scalar statement of the NEON instruction's definition on the other.
//   simde_mm_cvtepu32_ps_  (vcfux)    UCVTF: unsigned 32-bit integer to single, round to nearest even
//                                     -> ALL 2^32 inputs
//   simde_mm_adds_epu32    (vadduws)  UQADD: unsigned saturating add
//   simde_mm_cmpgt_epu8/16 (vcmpgtub/uh) CMHI: unsigned greater-than
//   simde_mm_avg_epi8/16   (vavgsb/sh)   SRHADD: signed (a + b + 1) >> 1
//   VectorMaskL / VectorMaskR: every byte is 0..15 or has bit 7 set (then pshufb and TBL agree on it)
// sa2-nx (2026-10-12), with the NEON forms now in the header:
//   simde_mm_avg_epi32     (vavgsw)   SRHADD on words
//   simde_mm_sllv_epi16 / srlv_epi16 / srav_epi16 / sllv_epi8: USHL / SSHL with the count clamped. Each is compared
//                                     with a scalar statement of what the x86 form gives for EVERY count (0 or the
//                                     sign fill from the lane size on) and with the header's NEON expression run
//                                     through simde's model of the instruction.
// A helper is only given a NEON form in the header if this reports 0 differences for it.
//
//   clang++ -O2 -std=c++23 -march=x86-64-v2 -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           -Iout/pc/rexglue-sdk/include sdk/tests/vmx_small_helpers_test.cpp -o out/tools/vmx_small_helpers_test.exe
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <simde/arm/neon.h>

#include <rex/ppc/intrinsics.h>

namespace {

uint64_t g_state = 0x452821E638D01377ull;
uint64_t Next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

}  // namespace

int main() {
  // UCVTF, every input. The C conversion of a uint32_t to float is correctly rounded in the current mode (round to
  // nearest even here): cvtsi2ss from the zero-extended 64-bit value.
  uint64_t ucvtf_differences = 0;
  uint32_t first_bad = 0;
  for (uint64_t base = 0; base < 0x100000000ull; base += 4) {
    const uint32_t in[4] = {uint32_t(base), uint32_t(base + 1), uint32_t(base + 2), uint32_t(base + 3)};
    simde__m128i v;
    std::memcpy(&v, in, 16);
    const simde__m128 r = rex::ppc::simde_mm_cvtepu32_ps_(v);
    float out[4];
    std::memcpy(out, &r, 16);
    for (int i = 0; i < 4; ++i) {
      volatile uint32_t x = in[i];
      const float want = float(uint32_t(x));
      if (std::bit_cast<uint32_t>(out[i]) != std::bit_cast<uint32_t>(want)) {
        if (!ucvtf_differences) first_bad = in[i];
        ++ucvtf_differences;
      }
    }
  }
  std::printf("vcfux / UCVTF: all 4294967296 inputs, %llu differences%s\n",
              static_cast<unsigned long long>(ucvtf_differences), ucvtf_differences ? " (first below)" : "");
  if (ucvtf_differences) std::printf("  first differing input %08X\n", first_bad);

  uint64_t add = 0, gt8 = 0, gt16 = 0, avg8 = 0, avg16 = 0, checked = 0;
  for (int k = 0; k < 6000000; ++k) {
    uint8_t a[16], b[16];
    uint64_t r[4] = {Next(), Next(), Next(), Next()};
    std::memcpy(a, &r[0], 16);
    std::memcpy(b, &r[2], 16);
    if ((k & 7) == 0) std::memset(a, (k & 8) ? 0xFF : 0x80, 16);
    if ((k & 15) == 1) std::memcpy(b, a, 16);
    simde__m128i va, vb;
    std::memcpy(&va, a, 16);
    std::memcpy(&vb, b, 16);
    uint8_t out[16];
    simde__m128i x = rex::ppc::simde_mm_adds_epu32(va, vb);
    std::memcpy(out, &x, 16);
    for (int i = 0; i < 4; ++i) {
      uint32_t p, q, got;
      std::memcpy(&p, a + 4 * i, 4);
      std::memcpy(&q, b + 4 * i, 4);
      std::memcpy(&got, out + 4 * i, 4);
      const uint64_t sum = uint64_t(p) + q;
      add += got != (sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(sum));
    }
    x = rex::ppc::simde_mm_cmpgt_epu8(va, vb);
    std::memcpy(out, &x, 16);
    for (int i = 0; i < 16; ++i) gt8 += out[i] != (a[i] > b[i] ? 0xFF : 0x00);
    x = rex::ppc::simde_mm_cmpgt_epu16(va, vb);
    std::memcpy(out, &x, 16);
    for (int i = 0; i < 8; ++i) {
      uint16_t p, q, got;
      std::memcpy(&p, a + 2 * i, 2);
      std::memcpy(&q, b + 2 * i, 2);
      std::memcpy(&got, out + 2 * i, 2);
      gt16 += got != (p > q ? 0xFFFF : 0x0000);
    }
    x = rex::ppc::simde_mm_avg_epi8(va, vb);
    std::memcpy(out, &x, 16);
    for (int i = 0; i < 16; ++i) avg8 += int8_t(out[i]) != int8_t((int(int8_t(a[i])) + int(int8_t(b[i])) + 1) >> 1);
    x = rex::ppc::simde_mm_avg_epi16(va, vb);
    std::memcpy(out, &x, 16);
    for (int i = 0; i < 8; ++i) {
      int16_t p, q, got;
      std::memcpy(&p, a + 2 * i, 2);
      std::memcpy(&q, b + 2 * i, 2);
      std::memcpy(&got, out + 2 * i, 2);
      avg16 += got != int16_t((int(p) + int(q) + 1) >> 1);
    }
    ++checked;
  }
  std::printf("%llu random vector pairs: UQADD (vadduws) %llu differences, CMHI bytes %llu, CMHI halfwords %llu, "
              "SRHADD bytes %llu, SRHADD halfwords %llu\n",
              static_cast<unsigned long long>(checked), static_cast<unsigned long long>(add),
              static_cast<unsigned long long>(gt8), static_cast<unsigned long long>(gt16),
              static_cast<unsigned long long>(avg8), static_cast<unsigned long long>(avg16));

  // The signed word average and the per-lane shifts. Counts: every small value, the lane sizes and their
  // neighbours, values whose low byte alone would mislead USHL (256, 0x8000, 0xFF10), and random ones.
  uint64_t avg32 = 0, sl16 = 0, sr16 = 0, sa16 = 0, sl8 = 0, shift_model = 0, shifts = 0;
  static const uint16_t kCounts[] = {0, 1, 2, 7, 8, 9, 14, 15, 16, 17, 31, 32, 33, 127, 128, 129, 255, 256, 257,
                                     0x7FFF, 0x8000, 0x8001, 0xFF00, 0xFF10, 0xFFFF};
  for (int k = 0; k < 3000000; ++k) {
    uint8_t a[16], c[16];
    uint64_t r[4] = {Next(), Next(), Next(), Next()};
    std::memcpy(a, &r[0], 16);
    std::memcpy(c, &r[2], 16);
    if ((k & 3) != 3) {
      // Mostly edge counts: random ones are almost always "too large".
      for (int i = 0; i < 8; ++i) {
        const uint16_t n = kCounts[(Next() >> 20) % (sizeof(kCounts) / sizeof(kCounts[0]))];
        std::memcpy(c + 2 * i, &n, 2);
      }
      if ((k & 3) == 1) {
        for (int i = 0; i < 16; ++i) c[i] = uint8_t(kCounts[(Next() >> 20) % 21]);  // byte counts 0..255
      }
    }
    simde__m128i va, vc;
    std::memcpy(&va, a, 16);
    std::memcpy(&vc, c, 16);
    uint8_t out[16], model[16];
    simde__m128i x = rex::ppc::simde_mm_avg_epi32(va, vc);
    std::memcpy(out, &x, 16);
    for (int i = 0; i < 4; ++i) {
      int32_t p, q, got;
      std::memcpy(&p, a + 4 * i, 4);
      std::memcpy(&q, c + 4 * i, 4);
      std::memcpy(&got, out + 4 * i, 4);
      avg32 += got != int32_t((int64_t(p) + int64_t(q) + 1) >> 1);
    }
    const simde_uint16x8_t na = simde_vreinterpretq_u16_u8(simde_vld1q_u8(a));
    const simde_uint16x8_t nc = simde_vreinterpretq_u16_u8(simde_vld1q_u8(c));
    // sllv_epi16
    x = rex::ppc::simde_mm_sllv_epi16(va, vc);
    std::memcpy(out, &x, 16);
    simde_vst1q_u8(model, simde_vreinterpretq_u8_u16(simde_vshlq_u16(
                              na, simde_vreinterpretq_s16_u16(simde_vminq_u16(nc, simde_vdupq_n_u16(16))))));
    shift_model += std::memcmp(out, model, 16) != 0;
    for (int i = 0; i < 8; ++i) {
      uint16_t p, n, got;
      std::memcpy(&p, a + 2 * i, 2);
      std::memcpy(&n, c + 2 * i, 2);
      std::memcpy(&got, out + 2 * i, 2);
      sl16 += got != (n >= 16 ? 0 : uint16_t(p << n));
    }
    // srlv_epi16
    x = rex::ppc::simde_mm_srlv_epi16(va, vc);
    std::memcpy(out, &x, 16);
    simde_vst1q_u8(model, simde_vreinterpretq_u8_u16(simde_vshlq_u16(
                              na, simde_vnegq_s16(simde_vreinterpretq_s16_u16(
                                      simde_vminq_u16(nc, simde_vdupq_n_u16(16)))))));
    shift_model += std::memcmp(out, model, 16) != 0;
    for (int i = 0; i < 8; ++i) {
      uint16_t p, n, got;
      std::memcpy(&p, a + 2 * i, 2);
      std::memcpy(&n, c + 2 * i, 2);
      std::memcpy(&got, out + 2 * i, 2);
      sr16 += got != (n >= 16 ? 0 : uint16_t(p >> n));
    }
    // srav_epi16
    x = rex::ppc::simde_mm_srav_epi16(va, vc);
    std::memcpy(out, &x, 16);
    simde_vst1q_u8(model, simde_vreinterpretq_u8_s16(simde_vshlq_s16(
                              simde_vreinterpretq_s16_u16(na),
                              simde_vnegq_s16(simde_vreinterpretq_s16_u16(
                                  simde_vminq_u16(nc, simde_vdupq_n_u16(15)))))));
    shift_model += std::memcmp(out, model, 16) != 0;
    for (int i = 0; i < 8; ++i) {
      int16_t p, got;
      uint16_t n;
      std::memcpy(&p, a + 2 * i, 2);
      std::memcpy(&n, c + 2 * i, 2);
      std::memcpy(&got, out + 2 * i, 2);
      sa16 += got != int16_t(p >> (n >= 15 ? 15 : n));
    }
    // sllv_epi8
    x = rex::ppc::simde_mm_sllv_epi8(va, vc);
    std::memcpy(out, &x, 16);
    simde_vst1q_u8(model, simde_vshlq_u8(simde_vld1q_u8(a),
                                         simde_vreinterpretq_s8_u8(simde_vminq_u8(simde_vld1q_u8(c),
                                                                                  simde_vdupq_n_u8(8)))));
    shift_model += std::memcmp(out, model, 16) != 0;
    for (int i = 0; i < 16; ++i) sl8 += out[i] != (c[i] >= 8 ? 0 : uint8_t(a[i] << c[i]));
    ++shifts;
  }
  std::printf("%llu random vector pairs: SRHADD words (vavgsw) %llu differences; shifts against the scalar statement: "
              "sllv16 %llu, srlv16 %llu, srav16 %llu, sllv8 %llu; against the NEON expression (simde's model): %llu\n",
              static_cast<unsigned long long>(shifts), static_cast<unsigned long long>(avg32),
              static_cast<unsigned long long>(sl16), static_cast<unsigned long long>(sr16),
              static_cast<unsigned long long>(sa16), static_cast<unsigned long long>(sl8),
              static_cast<unsigned long long>(shift_model));

  uint64_t mask_bytes = 0, mask_bad = 0;
  for (const uint8_t byte : rex::ppc::VectorMaskL) {
    ++mask_bytes;
    mask_bad += !(byte <= 15 || (byte & 0x80));
  }
  for (const uint8_t byte : rex::ppc::VectorMaskR) {
    ++mask_bytes;
    mask_bad += !(byte <= 15 || (byte & 0x80));
  }
  std::printf("VectorMaskL / VectorMaskR: %llu bytes, %llu outside {0..15, bit 7 set}\n",
              static_cast<unsigned long long>(mask_bytes), static_cast<unsigned long long>(mask_bad));
  const bool ok = !ucvtf_differences && !add && !gt8 && !gt16 && !avg8 && !avg16 && !mask_bad && !avg32 && !sl16 &&
                  !sr16 && !sa16 && !sl8 && !shift_model;
  std::printf(ok ? "RESULT: ok (0 differences)\n" : "RESULT: differences found (see above)\n");
  return ok ? 0 : 1;
}
