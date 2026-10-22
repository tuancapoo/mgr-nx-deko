// tools/float_loads.py: the AArch64 form of a single-precision guest load (the four big-endian bytes loaded into a
// vector register and reversed there, `ldr s; rev32`) against the integer form it replaces (the word loaded,
// byte-swapped and moved to the float register), for ALL 2^32 bit patterns. The NEON expression is the one the
// generated pch defines (RexFloatFromBigEndian), run here through simde's model of the instructions; both forms only
// move bytes, so every pattern (signalling NaNs and denormals included) must come out with the same bits.
//
//   clang++ -O2 -std=c++23 -march=x86-64-v2 -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           sdk/tests/float_loads_test.cpp -o out/tools/float_loads_test.exe
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <simde/arm/neon.h>

namespace {

// The pch's expression, with simde's names.
inline float FromBigEndian(const void* p) noexcept {
  float f;
  std::memcpy(&f, p, 4);
  const simde_float32x2_t v = simde_vset_lane_f32(f, simde_vdup_n_f32(0.0f), 0);
  return simde_vget_lane_f32(simde_vreinterpret_f32_u8(simde_vrev32_u8(simde_vreinterpret_u8_f32(v))), 0);
}

// The integer form (REX_LOAD_U32 and the union read of the generated code).
inline float IntegerForm(const void* p) noexcept {
  uint32_t w;
  std::memcpy(&w, p, 4);
  return std::bit_cast<float>(__builtin_bswap32(w));
}

}  // namespace

int main() {
  uint64_t differences = 0;
  alignas(8) unsigned char memory[8] = {};
  for (uint64_t i = 0; i <= 0xFFFFFFFFull; ++i) {
    const uint32_t guest = uint32_t(i);
    // Guest memory holds the value big-endian.
    memory[0] = uint8_t(guest >> 24);
    memory[1] = uint8_t(guest >> 16);
    memory[2] = uint8_t(guest >> 8);
    memory[3] = uint8_t(guest);
    void* p = memory;
    __asm__ __volatile__("" : "+r"(p) : : "memory");
    const uint32_t a = std::bit_cast<uint32_t>(FromBigEndian(p));
    const uint32_t b = std::bit_cast<uint32_t>(IntegerForm(p));
    if (a != b || b != guest) {
      if (differences++ < 12) printf("DIFFERENCE %08x: vector form %08x integer form %08x\n", guest, a, b);
    }
  }
  printf("float loads: all 2^32 patterns, vector form (simde's model) against the integer form\n");
  printf("RESULT: %s (%llu differences)\n", differences ? "FAILED" : "ok", (unsigned long long)differences);
  return differences ? 1 : 0;
}
