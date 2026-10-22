// rex/ppc/intrinsics.h gives three VMX helper operations a NEON-native form on AArch64:
//   simde_mm_perm_epi8_ (vperm)   -> vqtbl2q_u8({a, b}, (c & 0x1F) ^ 0x0F)
//   simde_mm_vctsxs               -> vcvtq_s32_f32   (FCVTZS)
//   simde_mm_vctuxs               -> vcvtq_u32_f32   (FCVTZU)
// This runs on a PC: the left side is the header's x86 form (what the PC build executes), the right side the same
// expression as the NEON path written with simde's implementation of the NEON instruction (its model of the
// instruction's definition) for vperm, and for the two conversions a scalar statement of the Arm definition of
// FCVTZS / FCVTZU. Every result must be equal.
//
//   clang++ -O2 -std=c++23 -march=x86-64-v2 -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           -Iout/pc/rexglue-sdk/include sdk/tests/vmx_helpers_neon_test.cpp -o out/tools/vmx_helpers_neon_test.exe
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <simde/arm/neon.h>

#include <rex/ppc/intrinsics.h>

namespace {

uint64_t g_state = 0xA4093822299F31D0ull;
uint64_t Next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

uint64_t g_checked = 0, g_failures = 0;

// FCVTZS / FCVTZU of a single, as the Arm ARM defines them (FPToFixed with round-toward-zero: a NaN gives 0, a value
// beyond the destination's range saturates). simde's x86 implementation of vcvtq_u32_f32 is not used: it goes through
// the signed conversion and returns 0x80000000 for values from 2^31 up, which the instruction does not.
int32_t Fcvtzs(float x) {
  if (std::isnan(x)) return 0;
  if (x >= 2147483648.0f) return INT32_MAX;
  if (x <= -2147483648.0f) return INT32_MIN;
  return int32_t(x);
}
uint32_t Fcvtzu(float x) {
  if (std::isnan(x)) return 0;
  if (x >= 4294967296.0f) return UINT32_MAX;
  if (x <= 0.0f) return 0;
  return uint32_t(x);
}

void Report(const char* what, const void* in, const void* got, const void* want) {
  if (g_failures++ < 12) {
    uint32_t i[4], g[4], w[4];
    std::memcpy(i, in, 16);
    std::memcpy(g, got, 16);
    std::memcpy(w, want, 16);
    std::printf("FAILED %s: in %08X %08X %08X %08X  neon form %08X %08X %08X %08X  x86 form %08X %08X %08X %08X\n",
                what, i[0], i[1], i[2], i[3], g[0], g[1], g[2], g[3], w[0], w[1], w[2], w[3]);
  }
}

void CheckPerm(const uint8_t* a, const uint8_t* b, const uint8_t* c) {
  simde__m128i va, vb, vc;
  std::memcpy(&va, a, 16);
  std::memcpy(&vb, b, 16);
  std::memcpy(&vc, c, 16);
  const simde__m128i x86 = rex::ppc::simde_mm_perm_epi8_(va, vb, vc);
  const simde_uint8x16x2_t table = {{simde_vld1q_u8(a), simde_vld1q_u8(b)}};
  const simde_uint8x16_t index =
      simde_veorq_u8(simde_vandq_u8(simde_vld1q_u8(c), simde_vdupq_n_u8(0x1F)), simde_vdupq_n_u8(0x0F));
  uint8_t neon[16], want[16];
  simde_vst1q_u8(neon, simde_vqtbl2q_u8(table, index));
  std::memcpy(want, &x86, 16);
  ++g_checked;
  if (std::memcmp(neon, want, 16) != 0) Report("vperm", c, neon, want);
}

void CheckConversions(const float* f) {
  simde__m128 v;
  std::memcpy(&v, f, 16);
  const simde__m128i s86 = rex::ppc::simde_mm_vctsxs(v);
  const simde__m128i u86 = rex::ppc::simde_mm_vctuxs(v);
  int32_t sn[4];
  uint32_t un[4];
  for (int i = 0; i < 4; ++i) {
    sn[i] = Fcvtzs(f[i]);
    un[i] = Fcvtzu(f[i]);
  }
  g_checked += 2;
  if (std::memcmp(sn, &s86, 16) != 0) Report("vctsxs", f, sn, &s86);
  if (std::memcmp(un, &u86, 16) != 0) Report("vctuxs", f, un, &u86);
}

}  // namespace

int main() {
  uint8_t a[16], b[16], c[16];
  // vperm: marker tables (a holds 0..15, b holds 16..31), every control byte value in every position, then random.
  for (int i = 0; i < 16; ++i) {
    a[i] = uint8_t(i);
    b[i] = uint8_t(16 + i);
  }
  for (int position = 0; position < 16; ++position) {
    for (int value = 0; value < 256; ++value) {
      for (int i = 0; i < 16; ++i) c[i] = uint8_t(Next());
      c[position] = uint8_t(value);
      CheckPerm(a, b, c);
    }
  }
  for (int k = 0; k < 3000000; ++k) {
    uint64_t r[6];
    for (auto& x : r) x = Next();
    std::memcpy(a, &r[0], 16);
    std::memcpy(b, &r[2], 16);
    std::memcpy(c, &r[4], 16);
    CheckPerm(a, b, c);
  }
  // The conversions: edge values in every lane position, then random bit patterns (exponents near 2^31 and 2^32 half
  // of the time).
  const uint32_t edges[] = {
      0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x3F000000, 0xBF000000,  // 0, denormals, 0.5
      0x3F7FFFFF, 0x3F800000, 0xBF800000, 0x3FFFFFFF, 0x40000000, 0x4EFFFFFF, 0x4F000000, 0x4F000001,  // around 2^31
      0xCEFFFFFF, 0xCF000000, 0xCF000001, 0x4F7FFFFF, 0x4F800000, 0x4F800001, 0xCF800000, 0x5F000000,  // around 2^32
      0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFFBFFFFF,  // max, inf, NaNs
      0x4B000000, 0x4B000001, 0xCB000001, 0x4A7FFFFE};                                                 // integers with halves
  float f[4];
  for (const uint32_t e0 : edges) {
    for (const uint32_t e1 : edges) {
      const uint32_t bits[4] = {e0, e1, uint32_t(Next()), e0 ^ 0x80000000u};
      std::memcpy(f, bits, 16);
      CheckConversions(f);
    }
  }
  for (int k = 0; k < 8000000; ++k) {
    uint32_t bits[4];
    for (auto& x : bits) {
      x = uint32_t(Next());
      if (Next() & 1) x = (x & 0x807FFFFFu) | (uint32_t(0x9C + Next() % 8) << 23);  // exponents 2^29 .. 2^36
    }
    std::memcpy(f, bits, 16);
    CheckConversions(f);
  }
  std::printf("%llu comparisons of the NEON forms with the x86 forms (vperm, vctsxs, vctuxs)\n",
              static_cast<unsigned long long>(g_checked));
  std::printf(g_failures ? "RESULT: FAILED (%llu differences)\n" : "RESULT: ok (0 differences)\n",
              static_cast<unsigned long long>(g_failures));
  return g_failures ? 1 : 0;
}
