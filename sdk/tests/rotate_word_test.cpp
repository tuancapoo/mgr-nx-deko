// The generator option rotate_word_narrow: rlwinm / rlwnm / rlwimi with a mask that does not wrap (mb <= me) written
// as a 32-bit rotate and a 32-bit mask. This compares that form with the form the generator wrote before (a 64-bit
// rotate of the word duplicated into both halves, masked with the 64-bit mask), bit for bit on the whole 64-bit
// register, for every shift 0..31 and every mask mb <= me, on edge values and random ones, with random upper halves
// in the source register (the old form reads `rS.u32 | (rS.u64 << 32)`, which drops them) and, for rlwimi, in the
// destination (whose upper half must be kept). The wrapping masks (mb > me) keep the old form in the generator; the
// test also shows that they need it (the narrow form differs there).
//
//   clang++ -O2 -std=c++20 sdk/tests/rotate_word_test.cpp -o out/tools/rotate_word_test.exe
#include <cstdint>
#include <cstdio>

namespace {

// sdk/src/codegen/builders/helpers.h, compute_mask (PowerPC bit numbering: bit 0 is the most significant).
uint64_t ComputeMask(uint32_t mstart, uint32_t mstop) {
  mstart &= 0x3F;
  mstop &= 0x3F;
  const uint64_t value = (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
  return mstart <= mstop ? value : ~value;
}

uint64_t g_state = 0x13198A2E03707344ull;
uint64_t Next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

uint64_t Rotl64(uint64_t x, unsigned n) { return n ? (x << n) | (x >> (64 - n)) : x; }
uint32_t Rotl32(uint32_t x, unsigned n) { return n ? (x << n) | (x >> (32 - n)) : x; }

// The old forms.
uint64_t OldRlwinm(uint64_t rs, unsigned sh, uint64_t mask) {
  return Rotl64(uint32_t(rs) | (rs << 32), sh) & mask;
}
uint64_t OldRlwimi(uint64_t ra, uint64_t rs, unsigned sh, uint64_t mask) {
  return (Rotl64(uint32_t(rs) | (rs << 32), sh) & mask) | (ra & ~mask);
}
// The narrow forms (what the generator writes with the option).
uint64_t NewRlwinm(uint64_t rs, unsigned sh, uint64_t mask) {
  return Rotl32(uint32_t(rs), sh) & uint32_t(mask);
}
uint64_t NewRlwimi(uint64_t ra, uint64_t rs, unsigned sh, uint64_t mask) {
  return (Rotl32(uint32_t(rs), sh) & uint32_t(mask)) | (ra & ~mask);
}

}  // namespace

int main() {
  const uint64_t edges[] = {0, 1, 0x80000000ull, 0xFFFFFFFFull, 0x7FFFFFFFull, 0x55555555ull, 0xAAAAAAAAull,
                            0x00010000ull, 0xFFFF0000ull, 0x0000FFFFull, 0x12345678ull, 0x80000001ull};
  uint64_t checked = 0, failures = 0, wrapping_differ = 0, wrapping = 0;
  for (unsigned sh = 0; sh < 32; ++sh) {
    for (unsigned mb = 0; mb < 32; ++mb) {
      for (unsigned me = 0; me < 32; ++me) {
        const uint64_t mask = ComputeMask(mb + 32, me + 32);
        for (int k = 0; k < 12 + 64; ++k) {
          const uint64_t upper = Next() << 32;
          const uint64_t rs = (k < 12 ? edges[k] : (Next() & 0xFFFFFFFFull)) | upper;
          const uint64_t ra = Next();
          if (mb <= me) {
            if ((mask >> 32) != 0) ++failures;  // the premise: a mask that does not wrap lies in the low word
            failures += OldRlwinm(rs, sh, mask) != NewRlwinm(rs, sh, mask);
            failures += OldRlwimi(ra, rs, sh, mask) != NewRlwimi(ra, rs, sh, mask);
            checked += 2;
          } else {
            ++wrapping;
            wrapping_differ += OldRlwinm(rs, sh, mask) != NewRlwinm(rs, sh, mask);
          }
        }
      }
    }
  }
  std::printf("%llu comparisons of the narrow forms with the 64-bit forms (every shift, every mask mb <= me)\n",
              static_cast<unsigned long long>(checked));
  std::printf("wrapping masks (kept in the 64-bit form by the generator): the narrow form would differ in %llu of "
              "%llu cases\n",
              static_cast<unsigned long long>(wrapping_differ), static_cast<unsigned long long>(wrapping));
  std::printf(failures ? "RESULT: FAILED (%llu differences)\n" : "RESULT: ok (0 differences)\n",
              static_cast<unsigned long long>(failures));
  return failures ? 1 : 0;
}
