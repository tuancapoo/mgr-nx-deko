// tools/pch_neon_shuffles.py redefines, on AArch64, simde_mm_shuffle_epi32(v, imm) and simde_mm_permute_ps(v, imm)
// as SIMDE_SHUFFLE_VECTOR_ with the indices (imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3). sasrt-nx left
// this one without a test (the index formula is simde's own for targets without a native shuffle); sa2-nx checks it
// anyway: this compares that exact expression (the macro bodies below are the tool's, under other names) with the
// real pshufd / shufps of the PC for every one of the 256 immediates, on position-marking and random vectors.
//
//   clang++ -O2 -std=c++23 -march=x86-64-v2 -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           sdk/tests/neon_shuffles_test.cpp -o out/tools/neon_shuffles_test.exe
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <emmintrin.h>
#include <simde/x86/avx.h>

#if !defined(SIMDE_SHUFFLE_VECTOR_)
#error "SIMDE_SHUFFLE_VECTOR_ is not available with this compiler: the tool's block would not apply either"
#endif

// The tool's two definitions (tools/pch_neon_shuffles.py, BLOCK), renamed.
#define rex_test_shuffle_epi32(a, imm8)                                                        \
  (__extension__({                                                                             \
    const simde__m128i_private rex_shuffle_a_ = simde__m128i_to_private(a);                    \
    simde__m128i_from_private((simde__m128i_private){                                          \
        .i32 = SIMDE_SHUFFLE_VECTOR_(32, 16, (rex_shuffle_a_).i32, (rex_shuffle_a_).i32,       \
                                     ((imm8)) & 3, ((imm8) >> 2) & 3, ((imm8) >> 4) & 3,       \
                                     ((imm8) >> 6) & 3)});                                     \
  }))
#define rex_test_permute_ps(a, imm8)                                                           \
  (__extension__({                                                                             \
    const simde__m128_private rex_permute_a_ = simde__m128_to_private(a);                      \
    simde__m128_from_private((simde__m128_private){                                            \
        .f32 = SIMDE_SHUFFLE_VECTOR_(32, 16, (rex_permute_a_).f32, (rex_permute_a_).f32,       \
                                     ((imm8)) & 3, ((imm8) >> 2) & 3, ((imm8) >> 4) & 3,       \
                                     ((imm8) >> 6) & 3)});                                     \
  }))

namespace {

uint64_t g_state = 0x9E3779B97F4A7C15ull;
uint64_t Next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

uint64_t g_comparisons = 0, g_differences = 0;

void Compare(const __m128i& want, const simde__m128i& got, const char* what, int imm) {
  ++g_comparisons;
  if (std::memcmp(&want, &got, 16) != 0) {
    if (g_differences++ < 8) std::printf("DIFFERENCE %s imm %d\n", what, imm);
  }
}

// The immediate must be a constant expression: one instantiation per value.
template <int Imm>
void One(const uint32_t* words) {
  const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words));
  simde__m128i sv;
  std::memcpy(&sv, &v, 16);
  Compare(_mm_shuffle_epi32(v, Imm), rex_test_shuffle_epi32(sv, Imm), "shuffle_epi32", Imm);
  const __m128 f = _mm_castsi128_ps(v);
  simde__m128 sf;
  std::memcpy(&sf, &f, 16);
  // vpermilps with an immediate is shufps of the vector with itself.
  const __m128 want = _mm_shuffle_ps(f, f, Imm);
  const simde__m128 got = rex_test_permute_ps(sf, Imm);
  __m128i want_i;
  simde__m128i got_i;
  std::memcpy(&want_i, &want, 16);
  std::memcpy(&got_i, &got, 16);
  Compare(want_i, got_i, "permute_ps", Imm);
}

template <int... Imm>
void Row(const uint32_t* words, int base) {
  (void)base;
  (One<Imm>(words), ...);
}

#define R4(n) n, n + 1, n + 2, n + 3
#define R16(n) R4(n), R4(n + 4), R4(n + 8), R4(n + 12)
#define R64(n) R16(n), R16(n + 16), R16(n + 32), R16(n + 48)

void All(const uint32_t* words) {
  Row<R64(0)>(words, 0);
  Row<R64(64)>(words, 64);
  Row<R64(128)>(words, 128);
  Row<R64(192)>(words, 192);
}

}  // namespace

int main() {
  const uint32_t marks[4] = {0x03020100u, 0x13121110u, 0x23222120u, 0x33323130u};
  All(marks);
  for (int i = 0; i < 20000; ++i) {
    uint32_t words[4];
    for (uint32_t& w : words) w = uint32_t(Next() >> 16);
    if ((i & 7) == 0) words[i & 3] = 0x7FC00001u;  // a NaN payload must travel as bits
    if ((i & 7) == 1) words[i & 3] = 0x00000001u;  // and a denormal
    All(words);
  }
  std::printf("%llu comparisons of the constant-permutation forms with pshufd / shufps (256 immediates)\n",
              static_cast<unsigned long long>(g_comparisons));
  std::printf("RESULT: %s (%llu differences)\n", g_differences ? "FAILED" : "ok",
              static_cast<unsigned long long>(g_differences));
  return g_differences ? 1 : 0;
}
