// tools/vec_byteswap.py replaces, on AArch64, the 16-byte reversal the generator writes as
//     simde_mm_shuffle_epi8(v, simde_mm_load_si128((simde__m128i*)VectorMaskL))
// with vextq_u8(r, r, 8), r = vrev64q_u8(v). This checks that the two are the same permutation, on a PC: the left
// side with the real pshufb and the SDK's own mask table, the right side with simde's implementation of the two NEON
// instructions (its model of their definition). Position-marking vectors (each byte its own index), single-byte
// vectors and random ones.
//
//   clang++ -O2 -std=c++23 -march=x86-64-v2 -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           -Iout/pc/rexglue-sdk/include sdk/tests/vec_byteswap_test.cpp -o out/tools/vec_byteswap_test.exe
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <simde/arm/neon.h>
#include <simde/x86/ssse3.h>

#include <rex/ppc/intrinsics.h>

namespace {

uint64_t g_state = 0x243F6A8885A308D3ull;
uint64_t Next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

bool Same(const uint8_t* bytes) {
  simde__m128i v;
  std::memcpy(&v, bytes, 16);
  const simde__m128i table =
      simde_mm_shuffle_epi8(v, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
  simde_uint8x16_t n = simde_vld1q_u8(bytes);
  const simde_uint8x16_t r = simde_vrev64q_u8(n);
  n = simde_vextq_u8(r, r, 8);
  uint8_t a[16], b[16];
  std::memcpy(a, &table, 16);
  simde_vst1q_u8(b, n);
  return std::memcmp(a, b, 16) == 0;
}

}  // namespace

int main() {
  uint64_t checked = 0, failures = 0;
  uint8_t bytes[16];
  for (int i = 0; i < 16; ++i) bytes[i] = uint8_t(i);
  failures += !Same(bytes);
  ++checked;
  // The expected permutation itself, spelled out once: byte i of the result is byte 15 - i of the input.
  {
    simde__m128i v;
    std::memcpy(&v, bytes, 16);
    const simde__m128i table =
        simde_mm_shuffle_epi8(v, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
    uint8_t out[16];
    std::memcpy(out, &table, 16);
    for (int i = 0; i < 16; ++i) failures += out[i] != uint8_t(15 - i);
  }
  for (int i = 0; i < 16; ++i) {
    for (int value = 1; value < 256; ++value) {
      std::memset(bytes, 0, 16);
      bytes[i] = uint8_t(value);
      failures += !Same(bytes);
      ++checked;
    }
  }
  for (int k = 0; k < 4000000; ++k) {
    const uint64_t lo = Next(), hi = Next();
    std::memcpy(bytes, &lo, 8);
    std::memcpy(bytes + 8, &hi, 8);
    failures += !Same(bytes);
    ++checked;
  }
  std::printf("%llu vectors: REV64 + EXT #8 against pshufb with VectorMaskL's first row\n",
              static_cast<unsigned long long>(checked));
  std::printf(failures ? "RESULT: FAILED (%llu differences)\n" : "RESULT: ok (0 differences)\n",
              static_cast<unsigned long long>(failures));
  return failures ? 1 : 0;
}
