// The any-mode helpers of the codegen option fp_compares_modeless (rex/ppc/diet_ops.h) against the plain operation:
// for every input, the reference is the plain expression evaluated with the host flush mode OFF (what the generated
// code did after switching to FPU mode), and the helper must give the same bits with the mode off AND with it on,
// and leave the mode as it found it. Also the three conversions the option stops switching for at all (fcfid,
// fctiwz, fctidz): their plain expressions must give the same bits in both modes.
//
// Inputs: every pair of a list of edge values (zeros, the smallest and largest denormal doubles, DBL_MIN, the single
// denormal range and FLT_MIN seen as doubles, ones, huge values, infinities, quiet and signalling NaNs), then random
// bit patterns with the exponent drawn from the edges half of the time.
//
//   clang++ -O2 -std=c++23 -ffp-contract=off -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde
//           sdk/tests/fp_any_mode_test.cpp -o out/tools/fp_any_mode_test.exe      (also builds for AArch64)
//   fp_any_mode_test [random pairs = 20000000] [control]
// "control" puts the plain operations in the helpers' place: the run must then report differences (it shows that the
// inputs reach the cases where the mode matters).
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <rex/ppc/diet_ops.h>
#include <simde/x86/sse2.h>

namespace {

using P = rex::platform::FPSCRPlatform;

// The flags of a CR field, as rex::ppc::CRRegister::compare(double, double) writes them.
struct Cr {
  bool lt, gt, eq, un;
  void compare(double left, double right) noexcept {
    un = __builtin_isunordered(left, right);
    lt = __builtin_isless(left, right);
    gt = __builtin_isgreater(left, right);
    eq = left == right;
  }
  uint32_t bits() const { return uint32_t(lt) | uint32_t(gt) << 1 | uint32_t(eq) << 2 | uint32_t(un) << 3; }
};

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

[[gnu::noinline]] uint32_t PlainCompare(double a, double b) {
  OPAQUE(a);
  OPAQUE(b);
  Cr cr;
  cr.compare(a, b);
  return cr.bits();
}
[[gnu::noinline]] uint32_t AnyCompare(double a, double b) {
  OPAQUE(a);
  OPAQUE(b);
  Cr cr;
  rex::ppc::CompareAnyMode(cr, a, b);
  return cr.bits();
}
[[gnu::noinline]] uint64_t PlainFrsp(double a) {
  OPAQUE(a);
  float f = float(a);
  OPAQUE(f);
  double d = double(f);
  OPAQUE(d);
  return std::bit_cast<uint64_t>(d);
}
[[gnu::noinline]] uint64_t AnyFrsp(double a) {
  OPAQUE(a);
  double d = rex::ppc::RoundToSingleAnyMode(a);
  OPAQUE(d);
  return std::bit_cast<uint64_t>(d);
}
[[gnu::noinline]] bool PlainGeZero(double a) {
  OPAQUE(a);
  return a >= 0.0;
}
[[gnu::noinline]] bool AnyGeZero(double a) {
  OPAQUE(a);
  return rex::ppc::GeZeroAnyMode(a);
}
// fadds / fsubs / fmuls / fdivs: the generator's plain expression and the any-mode helper (fp_mode_lookahead).
#define ARITHMETIC(NAME, OP, HELPER)                         \
  [[gnu::noinline]] uint64_t Plain##NAME(double a, double b) { \
    OPAQUE(a);                                               \
    OPAQUE(b);                                               \
    double d = a OP b;                                       \
    OPAQUE(d);                                               \
    float f = float(d);                                      \
    OPAQUE(f);                                               \
    double r = double(f);                                    \
    OPAQUE(r);                                               \
    return std::bit_cast<uint64_t>(r);                       \
  }                                                          \
  [[gnu::noinline]] uint64_t Any##NAME(double a, double b) {   \
    OPAQUE(a);                                               \
    OPAQUE(b);                                               \
    double r = rex::ppc::HELPER(a, b);                       \
    OPAQUE(r);                                               \
    return std::bit_cast<uint64_t>(r);                       \
  }
ARITHMETIC(Add, +, AddSingleAnyMode)
ARITHMETIC(Sub, -, SubSingleAnyMode)
ARITHMETIC(Mul, *, MulSingleAnyMode)
ARITHMETIC(Div, /, DivSingleAnyMode)
#undef ARITHMETIC

// The generator's expressions for the conversions that no longer switch.
[[gnu::noinline]] int64_t Fctiwz(double a) {
  OPAQUE(a);
  return rex::ppc::fctiwz(a);
}
[[gnu::noinline]] int64_t Fctidz(double a) {
  OPAQUE(a);
  return std::isnan(a) ? int64_t(0x8000000000000000ULL)
                       : (a > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&a));
}
[[gnu::noinline]] uint64_t Fcfid(int64_t v) {
  __asm__ __volatile__("" : "+r"(v));
  double d = double(v);
  OPAQUE(d);
  return std::bit_cast<uint64_t>(d);
}

uint64_t g_state = 0x9E3779B97F4A7C15ull;
uint64_t Next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}
double RandomDouble() {
  // Edges of the double and single ranges, and exponents whose products and quotients land on them (2^-511 squared
  // is near DBL_MIN, 2^-63 squared near FLT_MIN, 2^-74 squared near the smallest single denormal).
  static const uint64_t exponents[] = {0,     0,     1,     2,     3,     0x369, 0x36A, 0x380, 0x381, 0x382,
                                       0x3FF, 0x47E, 0x47F, 0x7FE, 0x7FF, 0x1FF, 0x200, 0x201, 0x3BF, 0x3C0,
                                       0x3C1, 0x3B4, 0x3B5, 0x43E, 0x43F, 0x5FE, 0x5FF};
  uint64_t r = Next();
  uint64_t e = (r >> 52) & 0x7FF;
  if (Next() & 1) e = exponents[Next() % (sizeof(exponents) / sizeof(exponents[0]))];
  uint64_t m = r & 0x000FFFFFFFFFFFFFull;
  switch (Next() & 7) {  // mantissas at the rounding edges too
    case 0: m = 0; break;
    case 1: m = 0x000FFFFFFFFFFFFFull; break;
    case 2: m &= 0x000FFFFFE0000000ull; break;  // exactly a single's mantissa
    case 3: m = (m & 0x000FFFFFE0000000ull) | 0x10000000ull; break;  // a tie when rounded to single
    default: break;
  }
  return std::bit_cast<double>((r & 0x8000000000000000ull) | (e << 52) | m);
}

uint64_t g_failures = 0, g_checks = 0, g_cold = 0;
uint64_t g_mode_matters[4] = {};  // per arithmetic operation: inputs whose plain result differs between the modes
bool g_control = false;  // negative control: the plain operations in the helpers' place must differ with the mode on
void Fail(const char* what, bool mode, double a, double b, uint64_t got, uint64_t want) {
  if (g_failures++ < 20) {
    std::printf("FAILED %s (flush mode %s): a %016llX b %016llX got %llX want %llX\n", what, mode ? "on" : "off",
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(a)),
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(b)), static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
  }
}

void CheckPair(double a, double b) {
  SetFlush(false);
  const uint32_t want_cmp = PlainCompare(a, b);
  const uint64_t want_frsp = PlainFrsp(a);
  const bool want_ge = PlainGeZero(a);
  const int64_t want_fctiwz = Fctiwz(a), want_fctidz = Fctidz(a);
  const int64_t integer = std::bit_cast<int64_t>(a);
  const uint64_t want_fcfid = Fcfid(integer);
  if (rex::ppc::IsDenormalDouble(a) || rex::ppc::IsDenormalDouble(b)) ++g_cold;
  const uint64_t want_arith[4] = {PlainAdd(a, b), PlainSub(a, b), PlainMul(a, b), PlainDiv(a, b)};
  // NaN payloads: the operations return one of the operands' payloads either way; compared as "is a NaN".
  const auto same = [](uint64_t x, uint64_t y) {
    const bool nx = (x & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
    const bool ny = (y & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
    return nx || ny ? nx == ny : x == y;
  };
  for (const bool mode : {false, true}) {
    SetFlush(mode);
    const auto before = P::getcsr();
    const uint32_t cmp = g_control ? PlainCompare(a, b) : AnyCompare(a, b);
    const uint64_t frsp = g_control ? PlainFrsp(a) : AnyFrsp(a);
    const bool ge = g_control ? PlainGeZero(a) : AnyGeZero(a);
    const int64_t fctiwz = Fctiwz(a), fctidz = Fctidz(a);
    const uint64_t fcfid = Fcfid(integer);
    const uint64_t arith[4] = {g_control ? PlainAdd(a, b) : AnyAdd(a, b), g_control ? PlainSub(a, b) : AnySub(a, b),
                               g_control ? PlainMul(a, b) : AnyMul(a, b), g_control ? PlainDiv(a, b) : AnyDiv(a, b)};
    const auto after = P::getcsr();
    static const char* const names[4] = {"AddSingleAnyMode", "SubSingleAnyMode", "MulSingleAnyMode",
                                         "DivSingleAnyMode"};
    for (int k = 0; k < 4; ++k) {
      if (!same(arith[k], want_arith[k])) Fail(names[k], mode, a, b, arith[k], want_arith[k]);
    }
    if (mode) {
      const uint64_t plain_on[4] = {PlainAdd(a, b), PlainSub(a, b), PlainMul(a, b), PlainDiv(a, b)};
      for (int k = 0; k < 4; ++k) g_mode_matters[k] += !same(plain_on[k], want_arith[k]);
    }
    g_checks += 10;
    if (cmp != want_cmp) Fail("CompareAnyMode", mode, a, b, cmp, want_cmp);
    if (frsp != want_frsp) Fail("RoundToSingleAnyMode", mode, a, b, frsp, want_frsp);
    if (ge != want_ge) Fail("GeZeroAnyMode", mode, a, b, ge, want_ge);
    if (fctiwz != want_fctiwz) Fail("fctiwz", mode, a, b, uint64_t(fctiwz), uint64_t(want_fctiwz));
    if (fctidz != want_fctidz) Fail("fctidz", mode, a, b, uint64_t(fctidz), uint64_t(want_fctidz));
    if (fcfid != want_fcfid) Fail("fcfid", mode, a, b, fcfid, want_fcfid);
    if ((before ^ after) & decltype(before)(P::FlushMask)) Fail("the mode was left changed", mode, a, b, after, before);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const uint64_t random_pairs = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000000ull;
  g_control = argc > 2 && argv[2][0] == 'c';
  const auto saved = P::getcsr();
  std::vector<double> edges;
  const uint64_t magnitudes[] = {
      0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000002ull, 0x0008000000000000ull,
      0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x0010000000000001ull,  // around DBL_MIN
      0x36A0000000000000ull, 0x369FFFFFFFFFFFFFull, 0x36A0000000000001ull,  // around the smallest single denormal
      0x3690000000000000ull, 0x3680000000000000ull,                          // half of it and below: rounds to 0 or it
      0x380FFFFFFFFFFFFFull, 0x3810000000000000ull, 0x3810000000000001ull,  // around FLT_MIN
      0x380FFFFFF0000000ull, 0x380FFFFFE0000000ull, 0x3800000000000000ull,  // single denormals as doubles
      0x3FF0000000000000ull, 0x3FF0000010000000ull, 0x3FEFFFFFFFFFFFFFull,  // around 1, a rounding tie
      0x41DFFFFFFFC00000ull, 0x41E0000000000000ull, 0x43E0000000000000ull,  // INT32_MAX, 2^31, 2^63
      0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x47F0000000000000ull,  // FLT_MAX and past it
      0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
      0x7FFFFFFFFFFFFFFFull};
  for (const uint64_t m : magnitudes) {
    edges.push_back(std::bit_cast<double>(m));
    edges.push_back(std::bit_cast<double>(m | 0x8000000000000000ull));
  }
  for (const double a : edges)
    for (const double b : edges) CheckPair(a, b);
  for (uint64_t i = 0; i < random_pairs; ++i) {
    const double a = RandomDouble();
    double b = (i & 7) == 0 ? edges[Next() % edges.size()] : RandomDouble();
    if ((i & 7) == 1) {
      // A near-equal partner: sums and differences that cancel down to a tiny result.
      const uint64_t bits = std::bit_cast<uint64_t>(a) ^ (Next() & 0xFFFF) ^ ((Next() & 1) << 63);
      b = std::bit_cast<double>(bits);
    }
    CheckPair(a, b);
  }
  P::setcsr(saved);
  std::printf("%llu edge pairs and %llu random pairs, %llu comparisons of a helper with the plain operation "
              "(%llu pairs with a denormal operand: the cold paths)\n",
              static_cast<unsigned long long>(edges.size() * edges.size()),
              static_cast<unsigned long long>(random_pairs), static_cast<unsigned long long>(g_checks),
              static_cast<unsigned long long>(g_cold));
  std::printf("pairs whose plain single-precision result differs between the modes: add %llu, sub %llu, mul %llu, "
              "div %llu\n",
              static_cast<unsigned long long>(g_mode_matters[0]), static_cast<unsigned long long>(g_mode_matters[1]),
              static_cast<unsigned long long>(g_mode_matters[2]), static_cast<unsigned long long>(g_mode_matters[3]));
  if (g_control) {
    std::printf("NEGATIVE CONTROL (plain operations with the mode on): %llu differences; the test can see the "
                "mode: %s\n",
                static_cast<unsigned long long>(g_failures), g_failures ? "yes" : "NO");
    return g_failures ? 0 : 1;
  }
  std::printf(g_failures ? "RESULT: FAILED (%llu differences)\n" : "RESULT: ok (0 differences)\n",
              static_cast<unsigned long long>(g_failures));
  return g_failures ? 1 : 0;
}
