// Exact conversion coverage for fp_moves_modeless. Reference: the same host
// conversion with flush disabled. Helpers must match every output bit in both
// modes, including the NaN payload/quieting produced by that same conversion.
// Usage: fp_moves_modeless_test [random input pairs = 1000000] [control]
// Control substitutes plain conversions: it passes only if both directions
// expose flush-dependent differences, while the flush-off reference still agrees.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/ppc/diet_ops.h>

namespace {
using P = rex::platform::FPSCRPlatform;
#if defined(__aarch64__) || defined(_M_ARM64)
#define OPAQUE_FP(x) __asm__ __volatile__("" : "+w"(x))
#else
#define OPAQUE_FP(x) __asm__ __volatile__("" : "+x"(x))
#endif

void SetFlush(bool enabled) {
  auto csr = P::getcsr();
  csr = enabled ? decltype(csr)(csr | decltype(csr)(P::FlushMask))
                : decltype(csr)(csr & ~decltype(csr)(P::FlushMask));
  P::setcsr(csr);
}

[[gnu::noinline]] uint64_t PlainLoad(uint32_t bits) {
  float f = std::bit_cast<float>(bits);
  OPAQUE_FP(f);
  double d = double(f);
  OPAQUE_FP(d);
  return std::bit_cast<uint64_t>(d);
}
[[gnu::noinline]] uint64_t AnyLoad(uint32_t bits) {
  __asm__ __volatile__("" : "+r"(bits));
  double d = rex::ppc::SingleToDoubleAnyMode(bits);
  OPAQUE_FP(d);
  return std::bit_cast<uint64_t>(d);
}
[[gnu::noinline]] uint32_t PlainStore(uint64_t bits) {
  double d = std::bit_cast<double>(bits);
  OPAQUE_FP(d);
  float f = float(d);
  OPAQUE_FP(f);
  return std::bit_cast<uint32_t>(f);
}
[[gnu::noinline]] uint32_t AnyStore(uint64_t bits) {
  double d = std::bit_cast<double>(bits);
  OPAQUE_FP(d);
  float f = rex::ppc::DoubleToSingleAnyMode(d);
  OPAQUE_FP(f);
  return std::bit_cast<uint32_t>(f);
}
#undef OPAQUE_FP

struct Results {
  uint64_t load[2]{}, store[2]{}, flush_errors = 0, checks = 0, printed = 0;
} results;
bool control = false;

void Difference(const char* operation, bool flush, uint64_t input, uint64_t got, uint64_t want) {
  if (control || results.printed++ >= 8) return;
  std::printf("DIFFERENCE %s flush=%u input=%016llX got=%016llX want=%016llX\n",
              operation, unsigned(flush), (unsigned long long)input,
              (unsigned long long)got, (unsigned long long)want);
}

void CheckLoad(uint32_t bits) {
  SetFlush(false);
  const uint64_t want = PlainLoad(bits);
  for (unsigned mode = 0; mode < 2; ++mode) {
    SetFlush(mode != 0);
    const auto before = P::getcsr();
    const uint64_t got = control ? PlainLoad(bits) : AnyLoad(bits);
    const auto after = P::getcsr();
    if (got != want) {
      ++results.load[mode];
      Difference("SingleToDouble", mode != 0, bits, got, want);
    }
    results.flush_errors += (before & P::FlushMask) != (after & P::FlushMask);
    ++results.checks;
  }
}

void CheckStore(uint64_t bits) {
  SetFlush(false);
  const uint32_t want = PlainStore(bits);
  for (unsigned mode = 0; mode < 2; ++mode) {
    SetFlush(mode != 0);
    const auto before = P::getcsr();
    const uint32_t got = control ? PlainStore(bits) : AnyStore(bits);
    const auto after = P::getcsr();
    if (got != want) {
      ++results.store[mode];
      Difference("DoubleToSingle", mode != 0, bits, got, want);
    }
    results.flush_errors += (before & P::FlushMask) != (after & P::FlushMask);
    ++results.checks;
  }
}

uint64_t random_state = 0x9E3779B97F4A7C15ull;
uint64_t Next() {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 7;
  random_state ^= random_state << 17;
  return random_state;
}
}  // namespace

int main(int argc, char** argv) {
  uint64_t pairs = 1000000;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "control") == 0) control = true;
    else {
      char* end = nullptr;
      pairs = std::strtoull(argv[i], &end, 10);
      if (!*argv[i] || !end || *end || *argv[i] == '-') {
        std::fprintf(stderr, "usage: %s [random input pairs] [control]\n", argv[0]);
        return 2;
      }
    }
  }
  const auto saved = P::getcsr();
  constexpr uint32_t singles[]{
      0, 1, 2, 0x003FFFFF, 0x00400000, 0x007FFFFE, 0x007FFFFF,
      0x00800000, 0x00800001, 0x3F000000, 0x3F800000, 0x7F7FFFFF,
      0x7F800000, 0x7F800001, 0x7FA12345, 0x7FC00000, 0x7FC12345, 0x7FFFFFFF};
  constexpr uint64_t doubles[]{
      0, 1, 2, 0x000FFFFFFFFFFFFEull, 0x000FFFFFFFFFFFFFull,
      0x0010000000000000ull, 0x0010000000000001ull,
      // Smallest single subnormal and its half-way rounding boundary.
      0x368FFFFFFFFFFFFFull, 0x3690000000000000ull, 0x3690000000000001ull,
      0x36A0000000000000ull, 0x36A0000000000001ull,
      // Largest single subnormal, rounding boundary, smallest single normal.
      0x380FFFFFC0000000ull, 0x380FFFFFDFFFFFFFull, 0x380FFFFFE0000000ull,
      0x380FFFFFE0000001ull, 0x380FFFFFFFFFFFFFull,
      0x3810000000000000ull, 0x3810000000000001ull,
      0x3FE0000000000000ull, 0x3FF0000000000000ull,
      // Single overflow boundary, double maximum, infinity and NaN payloads.
      0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x47F0000000000000ull,
      0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
      0x7FF0000000000001ull, 0x7FF123456789ABCDull,
      0x7FF8000000000000ull, 0x7FF8123456789ABCull, 0x7FFFFFFFFFFFFFFFull};
  for (uint32_t bits : singles) {
    CheckLoad(bits);
    CheckLoad(bits ^ 0x80000000u);
  }
  for (uint64_t bits : doubles) {
    CheckStore(bits);
    CheckStore(bits ^ 0x8000000000000000ull);
  }
  for (uint64_t i = 0; i < pairs; ++i) {
    CheckLoad(uint32_t(Next()));
    CheckStore(Next());
  }
  P::setcsr(saved);
  const bool passed = control
      ? !results.load[0] && !results.store[0] && results.load[1] && results.store[1] && !results.flush_errors
      : !results.load[0] && !results.load[1] && !results.store[0] && !results.store[1] && !results.flush_errors;
  std::printf("%s%s: %llu checks; load differences off/on=%llu/%llu, store=%llu/%llu, flush-mask changes=%llu\n",
              passed ? "PASS" : "FAIL", control ? " negative control" : " exact any-mode conversions",
              (unsigned long long)results.checks, (unsigned long long)results.load[0],
              (unsigned long long)results.load[1], (unsigned long long)results.store[0],
              (unsigned long long)results.store[1], (unsigned long long)results.flush_errors);
  return passed ? 0 : 1;
}
