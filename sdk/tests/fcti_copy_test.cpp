// The helpers of the codegen options fcti_inline and fp_copy_pairs (rex/ppc/diet_ops.h), sa2-nx's second CPU round.
//
// 1. fctid / fctiw / fctidz against a model of the PowerPC instruction written with integer arithmetic only (the
//    double is taken apart, rounded by the rule of the mode and saturated), in each of the four rounding modes:
//    edge values (zeros, halves, the int32 and int64 limits and their neighbours, denormals, infinities, NaNs) and
//    random bit patterns. The model is also the AArch64 form step by step: `frinti` (to an integral double in the
//    current mode) and the saturating `fcvtzs`, with the NaN select. The old generated expressions are run next to
//    them: on x86 they must agree everywhere except fctid / fctidz at exactly 2^63 (reported: once per mode).
// 2. rex::ppc::SingleToDoublePure and QuietSingleBits for ALL 2^32 single bit patterns:
//      - with the flush mode off: the same bits as `double(float)` and as `float(double(float))`, and as a
//        conversion written with integer arithmetic only;
//      - with the flush mode on: SingleToDoublePure still gives the integer conversion's bits (the plain conversion
//        does not: its differences are counted as the negative control and must not be 0).
//
//   clang++ -O2 -std=c++23 -ffp-contract=off -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           sdk/tests/fcti_copy_test.cpp -o out/tools/fcti_copy_test.exe
//   fcti_copy_test [random values per mode = 20000000]
#include <bit>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <rex/ppc/diet_ops.h>
#include <simde/x86/sse2.h>

namespace {

using P = rex::platform::FPSCRPlatform;

#if defined(__aarch64__)
#define OPAQUE(x) __asm__ __volatile__("" : "+w"(x))
#else
#define OPAQUE(x) __asm__ __volatile__("" : "+x"(x))
#endif

void SetFlush(bool on) {
  auto csr = P::getcsr();
  csr = on ? decltype(csr)(csr | decltype(csr)(P::FlushMask)) : decltype(csr)(csr & ~decltype(csr)(P::FlushMask));
  P::setcsr(csr);
}

// Guest rounding modes (FPSCR.RN): 0 nearest even, 1 toward zero, 2 up, 3 down.
void SetRounding(unsigned guest) {
  auto csr = P::getcsr();
  csr = decltype(csr)((csr & ~decltype(csr)(P::RoundMaskVal)) | decltype(csr)(P::GuestToHost[guest]));
  P::setcsr(csr);
}

// The double rounded to an integer in the given mode, saturated to [lo, hi]; integer arithmetic only.
// `nan` is the answer for a NaN.
int64_t ModelRound(double v, unsigned mode, int64_t lo, int64_t hi, int64_t nan) {
  const uint64_t bits = std::bit_cast<uint64_t>(v);
  const bool negative = bits >> 63;
  const int exponent = int((bits >> 52) & 0x7FF);
  uint64_t fraction = bits & 0x000FFFFFFFFFFFFFull;
  if (exponent == 0x7FF) {
    if (fraction != 0) return nan;
    return negative ? lo : hi;
  }
  // |v| = magnitude * 2^-shift exactly (as far as it matters), with the bits below the integer in `rest`.
  unsigned __int128 magnitude;
  bool half = false, below = false;  // the first bit below the integer, and any bit below that
  if (exponent == 0) {
    magnitude = 0;
    below = fraction != 0;  // a denormal: less than a half
  } else {
    fraction |= 0x0010000000000000ull;
    const int e = exponent - 1023 - 52;  // |v| = fraction * 2^e
    if (e >= 0) {
      if (e > 62) return negative ? lo : hi;  // at least 2^115
      magnitude = (unsigned __int128)fraction << e;
    } else if (e < -54) {
      magnitude = 0;
      below = true;
    } else {
      const int s = -e;  // 1..54
      magnitude = s >= 64 ? 0 : fraction >> s;
      half = (fraction >> (s - 1)) & 1;
      below = (fraction & ((1ull << (s - 1)) - 1)) != 0;
    }
  }
  bool up = false;  // away from zero
  switch (mode) {
    case 0: up = half && (below || (magnitude & 1)); break;
    case 1: up = false; break;
    case 2: up = !negative && (half || below); break;
    case 3: up = negative && (half || below); break;
  }
  if (up) ++magnitude;
  if (negative) {
    if (magnitude > (unsigned __int128)(-(__int128)lo)) return lo;
    return int64_t(-(__int128)magnitude);
  }
  if (magnitude > (unsigned __int128)hi) return hi;
  return int64_t(magnitude);
}

// The register images the instructions leave.
int64_t ModelFctid(double v, unsigned mode) { return ModelRound(v, mode, INT64_MIN, INT64_MAX, INT64_MIN); }
int64_t ModelFctidz(double v) { return ModelRound(v, 1, INT64_MIN, INT64_MAX, INT64_MIN); }
int64_t ModelFctiw(double v, unsigned mode) {
  return ModelRound(v, mode, INT32_MIN, INT32_MAX, int64_t(0x80000000u));
}

[[gnu::noinline]] int64_t NewFctid(double v) { OPAQUE(v); return rex::ppc::fctid(v); }
[[gnu::noinline]] int64_t NewFctidz(double v) { OPAQUE(v); return rex::ppc::fctidz(v); }
[[gnu::noinline]] int64_t NewFctiw(double v) { OPAQUE(v); return rex::ppc::fctiw(v); }
// The generator's expressions without the option (floating_point.cpp).
[[gnu::noinline]] int64_t OldFctid(double v) {
  OPAQUE(v);
  return __builtin_isnan(v) ? int64_t(0x8000000000000000ULL)
                            : (v > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvtsd_si64(simde_mm_load_sd(&v));
}
[[gnu::noinline]] int64_t OldFctidz(double v) {
  OPAQUE(v);
  return __builtin_isnan(v) ? int64_t(0x8000000000000000ULL)
                            : (v > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&v));
}
[[gnu::noinline]] int64_t OldFctiw(double v) {
  OPAQUE(v);
  return __builtin_isnan(v) ? int64_t(0x80000000U)
                            : (v >= double(INT_MAX)) ? INT_MAX : simde_mm_cvtsd_si32(simde_mm_load_sd(&v));
}

uint64_t g_seed = 0x9E3779B97F4A7C15ull;
uint64_t Next() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 7;
  g_seed ^= g_seed << 17;
  return g_seed;
}

// single -> double with integer arithmetic only, signalling NaNs quieted (what the hardware conversion does).
uint64_t ModelSingleToDouble(uint32_t bits) {
  const uint64_t sign = uint64_t(bits >> 31) << 63;
  const uint32_t exponent = (bits >> 23) & 0xFF;
  uint32_t fraction = bits & 0x007FFFFFu;
  if (exponent == 0xFF) {
    return sign | 0x7FF0000000000000ull | (fraction ? (uint64_t(fraction | 0x00400000u) << 29) : 0);
  }
  if (exponent == 0) {
    if (fraction == 0) return sign;
    int e = 1;
    while (!(fraction & 0x00800000u)) {
      fraction <<= 1;
      --e;
    }
    return sign | (uint64_t(e - 127 + 1023) << 52) | (uint64_t(fraction & 0x007FFFFFu) << 29);
  }
  return sign | (uint64_t(exponent - 127 + 1023) << 52) | (uint64_t(fraction) << 29);
}

[[gnu::noinline]] uint64_t PlainLoad(uint32_t bits) {
  float f = std::bit_cast<float>(bits);
  OPAQUE(f);
  double d = double(f);
  OPAQUE(d);
  return std::bit_cast<uint64_t>(d);
}
[[gnu::noinline]] uint32_t PlainCopy(uint32_t bits) {
  float f = std::bit_cast<float>(bits);
  OPAQUE(f);
  double d = double(f);
  OPAQUE(d);
  float g = float(d);
  OPAQUE(g);
  return std::bit_cast<uint32_t>(g);
}

}  // namespace

int main(int argc, char** argv) {
  const uint64_t randoms = argc > 1 ? strtoull(argv[1], nullptr, 10) : 20000000ull;
  uint64_t differences = 0, checked = 0, old_differs = 0;

  // ---- 1. the conversions to integer ----
  std::vector<double> edges;
  const double base_values[] = {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 2.5, 3.5, 1e-310, 4.9406564584124654e-324,
                                2147483645.5, 2147483646.0, 2147483646.5, 2147483647.0, 2147483647.25,
                                2147483647.5, 2147483647.75, 2147483648.0, 2147483648.5, 2147483649.0, 4294967295.5,
                                4294967296.0, 4503599627370495.5, 4503599627370496.0, 4503599627370497.0,
                                9007199254740991.0, 9007199254740992.0, 9223372036854774784.0,
                                9223372036854775808.0, 9223372036854777856.0, 1.8446744073709552e19, 1e300};
  for (double v : base_values) {
    edges.push_back(v);
    edges.push_back(-v);
    // the doubles next to it
    const uint64_t b = std::bit_cast<uint64_t>(v);
    edges.push_back(std::bit_cast<double>(b + 1));
    edges.push_back(-std::bit_cast<double>(b + 1));
    if (b) {
      edges.push_back(std::bit_cast<double>(b - 1));
      edges.push_back(-std::bit_cast<double>(b - 1));
    }
  }
  for (uint64_t nan : {0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000001ull,
                       0x7FF0000000000001ull, 0xFFF4000000000000ull}) {
    edges.push_back(std::bit_cast<double>(nan));
  }
  SetFlush(false);
  for (unsigned mode = 0; mode < 4; ++mode) {
    SetRounding(mode);
    const auto one = [&](double v) {
      const int64_t d = NewFctid(v), dz = NewFctidz(v), w = NewFctiw(v);
      const int64_t md = ModelFctid(v, mode), mdz = ModelFctidz(v), mw = ModelFctiw(v, mode);
      checked += 3;
      if (d != md || dz != mdz || w != mw) {
        if (differences++ < 12) {
          printf("DIFFERENCE mode %u v=%a: fctid %llx (model %llx) fctidz %llx (model %llx) fctiw %llx (model %llx)\n",
                 mode, v, (unsigned long long)d, (unsigned long long)md, (unsigned long long)dz,
                 (unsigned long long)mdz, (unsigned long long)w, (unsigned long long)mw);
        }
      }
#if !defined(__aarch64__)
      // The old expressions on x86: equal, except fctid / fctidz at exactly 2^63.
      const bool at_limit = v == 9223372036854775808.0;
      if (OldFctid(v) != d || OldFctidz(v) != dz || OldFctiw(v) != w) {
        if (at_limit && OldFctiw(v) == w) {
          ++old_differs;
        } else if (differences++ < 12) {
          printf("DIFFERENCE from the old expression, mode %u v=%a\n", mode, v);
        }
      }
#endif
    };
    for (double v : edges) one(v);
    for (uint64_t i = 0; i < randoms; ++i) {
      uint64_t bits = Next();
      switch (i & 3) {
        case 0: break;                                                             // any double
        case 1: bits = (bits & 0x800FFFFFFFFFFFFFull) | (uint64_t(1023 + (Next() % 70)) << 52); break;  // 1 .. 2^70
        case 2: bits = std::bit_cast<uint64_t>(double(int64_t(Next() % 4000000) - 2000000) * 0.5); break;  // halves
        case 3: bits = std::bit_cast<uint64_t>(2147483648.0 * ((Next() & 1) ? 1 : -1) +
                                                 double(int64_t(Next() % 4096) - 2048) * 0.25); break;
      }
      one(std::bit_cast<double>(bits));
    }
  }
  SetRounding(0);
  printf("fcti: %llu results against the integer model in 4 rounding modes; old x86 expressions differ at 2^63 only: "
         "%llu times\n",
         (unsigned long long)checked, (unsigned long long)old_differs);

  // ---- 2. the float copy pair, all single bit patterns ----
  uint64_t control = 0;
  SetFlush(false);
  for (uint64_t i = 0; i <= 0xFFFFFFFFull; ++i) {
    const uint32_t bits = uint32_t(i);
    const uint64_t ref = PlainLoad(bits);
    const uint64_t pure = std::bit_cast<uint64_t>(rex::ppc::SingleToDoublePure(bits));
    if (pure != ref || ModelSingleToDouble(bits) != ref || rex::ppc::QuietSingleBits(bits) != PlainCopy(bits)) {
      if (differences++ < 12) {
        printf("DIFFERENCE (mode off) single %08x: pure %016llx model %016llx plain %016llx; quiet %08x plain %08x\n",
               bits, (unsigned long long)pure, (unsigned long long)ModelSingleToDouble(bits),
               (unsigned long long)ref, rex::ppc::QuietSingleBits(bits), PlainCopy(bits));
      }
    }
  }
  SetFlush(true);
  for (uint64_t i = 0; i <= 0xFFFFFFFFull; ++i) {
    const uint32_t bits = uint32_t(i);
    const uint64_t model = ModelSingleToDouble(bits);
    uint32_t opaque = bits;
    __asm__ __volatile__("" : "+r"(opaque));
    if (std::bit_cast<uint64_t>(rex::ppc::SingleToDoublePure(opaque)) != model) {
      if (differences++ < 12) printf("DIFFERENCE (mode on) single %08x\n", bits);
    }
    if (PlainLoad(bits) != model) ++control;
  }
  const bool still_on = P::getcsr() & P::FlushMask;
  SetFlush(false);
  checked += 3 * 0x100000000ull + 0x100000000ull;
  printf("copy pair: all 2^32 singles, mode off (pure, integer model, quieted bits) and mode on (pure); the plain "
         "conversion differs with the mode on for %llu singles (negative control, must not be 0); mode left %s\n",
         (unsigned long long)control, still_on ? "on (as set)" : "CHANGED");
  if (!still_on) ++differences;
  if (control == 0) {
    printf("FAILED: the negative control found nothing\n");
    return 2;
  }
  printf("%llu checks, %llu differences\n", (unsigned long long)checked, (unsigned long long)differences);
  return differences ? 1 : 0;
}
