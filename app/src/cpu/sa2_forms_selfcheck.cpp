// Sonic Adventure 2: the instruction forms that only exist on one processor, checked at every start
// (docs/enhancements.md, "The third CPU round of 2026-10-12"; the idea of a boot-time proof is banjo-nx's).
//
// Several helpers have an AArch64 form the PC build never compiles: the conversions to integer (`frinti` + `fcvtzs`),
// the divisions (`sdiv` / `udiv`), the small VMX helpers (one NEON instruction each), vperm and the two saturating
// float-to-integer conversions. Their unit tests run the NEON expressions through a model on the PC; the processor
// itself first sees them on the console. So each start runs every one of them on a fixed set of edge inputs
// against a reference written in plain C++ (no helper, no intrinsic) and logs one line:
//     [enh] instruction forms self-check: N of N results as expected
// or, per failure, a DIFFERENCE line with the helper, the input and both results (and, because the released
// configuration logs nothing, the same lines in sa2_selfcheck_failed.txt next to the program). On the PC the same
// table runs the x86 forms, which proves the expected values; on the console it proves the AArch64 forms. A few
// microseconds, once.
#include "sa2_enhancements.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/diet_ops.h>
#include <rex/ppc/intrinsics.h>

namespace {

// Keeps the compiler from working a result out at build time.
template <typename T>
T Opaque(T v) {
  volatile T x = v;
  return x;
}

struct Tally {
  int checks = 0;
  int wrong = 0;
  std::string report;  // the first failures, for the file a failed check leaves next to the program
  void Expect(const char* what, const std::string& input, uint64_t got, uint64_t want) {
    ++checks;
    if (got == want) return;
    if (wrong++ < 8) {
      REXLOG_ERROR("[enh] DIFFERENCE: instruction forms self-check: {}({}) gave {:016X}, expected {:016X}", what,
                   input, got, want);
      report += fmt::format("{}({}) gave {:016X}, expected {:016X}\n", what, input, got, want);
    }
  }
};

union Vec {
  simde__m128i v;
  simde__m128 f;
  uint8_t u8[16];
  int8_t s8[16];
  uint16_t u16[8];
  int16_t s16[8];
  uint32_t u32[4];
  int32_t s32[4];
  float f32[4];
};

Vec Load(const uint8_t* bytes) {
  Vec r;
  std::memcpy(r.u8, bytes, 16);
  r.v = Opaque(r.v);
  return r;
}

void ExpectVec(Tally& t, const char* what, int index, const Vec& got, const Vec& want) {
  uint64_t g[2], w[2];
  std::memcpy(g, got.u8, 16);
  std::memcpy(w, want.u8, 16);
  t.Expect(what, fmt::format("vector {} low", index), g[0], w[0]);
  t.Expect(what, fmt::format("vector {} high", index), g[1], w[1]);
}

void Conversions(Tally& t) {
  // Round to nearest even: the mode every thread starts in. fctiw's result is the register image: sign extended,
  // except a NaN (0x0000000080000000).
  struct Case {
    double v;
    int64_t fctid, fctiw, fctidz;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Case cases[] = {
      {0.5, 0, 0, 0},
      {1.5, 2, 2, 1},
      {2.5, 2, 2, 2},
      {3.5, 4, 4, 3},
      {-0.5, 0, 0, 0},
      {-1.5, -2, -2, -1},
      {-2.5, -2, -2, -2},
      {-3.5, -4, -4, -3},
      {2.4999, 2, 2, 2},
      {2.5001, 3, 3, 2},
      {1e10, 10000000000ll, INT32_MAX, 10000000000ll},
      {-1e10, -10000000000ll, INT32_MIN, -10000000000ll},
      {9223372036854775808.0, INT64_MAX, INT32_MAX, INT64_MAX},
      {-1e19, INT64_MIN, INT32_MIN, INT64_MIN},
      {nan, INT64_MIN, int64_t(0x80000000u), INT64_MIN},
  };
  for (const Case& c : cases) {
    const double v = Opaque(c.v);
    const std::string in = fmt::format("{}", c.v);
    t.Expect("fctid", in, uint64_t(rex::ppc::fctid(v)), uint64_t(c.fctid));
    t.Expect("fctiw", in, uint64_t(rex::ppc::fctiw(v)), uint64_t(c.fctiw));
    t.Expect("fctidz", in, uint64_t(rex::ppc::fctidz(v)), uint64_t(c.fctidz));
    // fctiwz (the first round's): truncation with the same saturation and NaN image.
    int64_t z;
    if (std::isnan(c.v)) {
      z = int64_t(0x80000000u);
    } else if (c.v >= 2147483647.0) {
      z = INT32_MAX;
    } else if (c.v <= -2147483648.0) {
      z = INT32_MIN;
    } else {
      z = int64_t(int32_t(c.v));
    }
    t.Expect("fctiwz", in, uint64_t(rex::ppc::fctiwz(v)), uint64_t(z));
  }
}

void Divisions(Tally& t) {
  struct Signed {
    int64_t n, d;
  };
  const Signed pairs[] = {{7, 0},   {-7, 0},       {0, 0},         {100, 7},      {-100, 7},      {100, -7},
                          {-7, 2},  {7, -2},       {INT32_MIN, -1}, {INT32_MIN, 1}, {INT32_MAX, -1}, {INT64_MIN, -1},
                          {INT64_MIN, 1}, {INT64_MAX, -1}, {-9, 2}, {1, INT64_MIN}};
  for (const Signed& p : pairs) {
    const std::string in = fmt::format("{}, {}", p.n, p.d);
    {
      const int32_t n = int32_t(p.n), d = int32_t(p.d);
      const uint32_t want = (d == 0 || (n == INT32_MIN && d == -1)) ? 0u : uint32_t(int64_t(n) / int64_t(d));
      t.Expect("divw", in, rex::ppc::divw(Opaque(n), Opaque(d)), want);
      const uint32_t un = uint32_t(n), ud = uint32_t(d);
      t.Expect("divwu", in, rex::ppc::divwu(Opaque(un), Opaque(ud)), ud == 0 ? 0u : un / ud);
    }
    {
      const int64_t n = p.n, d = p.d;
      int64_t want = 0;
      if (d != 0 && !(n == INT64_MIN && d == -1)) want = n / d;
      t.Expect("divd", in, uint64_t(rex::ppc::divd(Opaque(n), Opaque(d))), uint64_t(want));
      const uint64_t un = uint64_t(n), ud = uint64_t(d);
      t.Expect("divdu", in, rex::ppc::divdu(Opaque(un), Opaque(ud)), ud == 0 ? 0u : un / ud);
    }
  }
}

void FloatCopies(Tally& t) {
  // single bits -> the double a load gives with the flush mode off, and the bits a store of it writes.
  struct Case {
    uint32_t single;
    uint64_t as_double;
    uint32_t stored;
  };
  const Case cases[] = {
      {0x00000000u, 0x0000000000000000ull, 0x00000000u}, {0x80000000u, 0x8000000000000000ull, 0x80000000u},
      {0x3F800000u, 0x3FF0000000000000ull, 0x3F800000u}, {0x00000001u, 0x36A0000000000000ull, 0x00000001u},
      {0x807FFFFFu, 0xB80FFFFFC0000000ull, 0x807FFFFFu}, {0x00800000u, 0x3810000000000000ull, 0x00800000u},
      {0x7F800000u, 0x7FF0000000000000ull, 0x7F800000u}, {0x7FC00001u, 0x7FF8000020000000ull, 0x7FC00001u},
      {0x7F800001u, 0x7FF8000020000000ull, 0x7FC00001u}, {0xFFA00000u, 0xFFFC000000000000ull, 0xFFE00000u},
  };
  for (const Case& c : cases) {
    const std::string in = fmt::format("{:08X}", c.single);
    t.Expect("SingleToDoublePure", in, std::bit_cast<uint64_t>(rex::ppc::SingleToDoublePure(Opaque(c.single))),
             c.as_double);
    t.Expect("QuietSingleBits", in, rex::ppc::QuietSingleBits(Opaque(c.single)), c.stored);
  }
}

void VectorHelpers(Tally& t) {
  // Two operands with lanes at the edges of every width, and shift counts below, at and above the lane width.
  static const uint8_t kA[4][16] = {
      {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x55, 0xAA, 0x10, 0x20, 0x40, 0x08, 0x04, 0x02, 0xC3, 0x3C},
      {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0x00},
      {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21},
      {0x00, 0x80, 0xFF, 0x7F, 0x01, 0x80, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
  };
  static const uint8_t kB[4][16] = {
      {0x01, 0x01, 0x01, 0x80, 0x01, 0xFF, 0xAA, 0x55, 0x10, 0x1F, 0x40, 0x07, 0x04, 0x03, 0x3C, 0xC3},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
      {0x00, 0x00, 0x07, 0x00, 0x08, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11, 0x00, 0xFF, 0x00, 0x00, 0x01},
      {0x00, 0x01, 0x07, 0x08, 0x09, 0x0F, 0x10, 0x1F, 0x20, 0x7F, 0x80, 0xFF, 0x03, 0x05, 0x06, 0x02},
  };
  for (int k = 0; k < 4; ++k) {
    const Vec a = Load(kA[k]), b = Load(kB[k]);
    Vec want, got;
    for (int i = 0; i < 4; ++i) want.u32[i] = uint32_t(std::min<uint64_t>(uint64_t(a.u32[i]) + b.u32[i], 0xFFFFFFFFull));
    got.v = rex::ppc::simde_mm_adds_epu32(a.v, b.v);
    ExpectVec(t, "adds_epu32", k, got, want);
    for (int i = 0; i < 16; ++i) want.s8[i] = int8_t((int(a.s8[i]) + int(b.s8[i]) + 1) >> 1);
    got.v = rex::ppc::simde_mm_avg_epi8(a.v, b.v);
    ExpectVec(t, "avg_epi8", k, got, want);
    for (int i = 0; i < 8; ++i) want.s16[i] = int16_t((int(a.s16[i]) + int(b.s16[i]) + 1) >> 1);
    got.v = rex::ppc::simde_mm_avg_epi16(a.v, b.v);
    ExpectVec(t, "avg_epi16", k, got, want);
    for (int i = 0; i < 4; ++i) want.s32[i] = int32_t((int64_t(a.s32[i]) + int64_t(b.s32[i]) + 1) >> 1);
    got.v = rex::ppc::simde_mm_avg_epi32(a.v, b.v);
    ExpectVec(t, "avg_epi32", k, got, want);
    for (int i = 0; i < 4; ++i) want.f32[i] = float(a.u32[i]);
    got.f = rex::ppc::simde_mm_cvtepu32_ps_(a.v);
    ExpectVec(t, "cvtepu32_ps", k, got, want);
    for (int i = 0; i < 16; ++i) want.u8[i] = a.u8[i] > b.u8[i] ? 0xFF : 0;
    got.v = rex::ppc::simde_mm_cmpgt_epu8(a.v, b.v);
    ExpectVec(t, "cmpgt_epu8", k, got, want);
    for (int i = 0; i < 8; ++i) want.u16[i] = a.u16[i] > b.u16[i] ? 0xFFFF : 0;
    got.v = rex::ppc::simde_mm_cmpgt_epu16(a.v, b.v);
    ExpectVec(t, "cmpgt_epu16", k, got, want);
    for (int i = 0; i < 8; ++i) want.u16[i] = b.u16[i] >= 16 ? 0 : uint16_t(a.u16[i] << b.u16[i]);
    got.v = rex::ppc::simde_mm_sllv_epi16(a.v, b.v);
    ExpectVec(t, "sllv_epi16", k, got, want);
    for (int i = 0; i < 8; ++i) want.u16[i] = b.u16[i] >= 16 ? 0 : uint16_t(a.u16[i] >> b.u16[i]);
    got.v = rex::ppc::simde_mm_srlv_epi16(a.v, b.v);
    ExpectVec(t, "srlv_epi16", k, got, want);
    for (int i = 0; i < 8; ++i) want.s16[i] = int16_t(int32_t(a.s16[i]) >> (b.u16[i] >= 15 ? 15 : b.u16[i]));
    got.v = rex::ppc::simde_mm_srav_epi16(a.v, b.v);
    ExpectVec(t, "srav_epi16", k, got, want);
    for (int i = 0; i < 16; ++i) want.u8[i] = b.u8[i] >= 8 ? 0 : uint8_t(a.u8[i] << b.u8[i]);
    got.v = rex::ppc::simde_mm_sllv_epi8(a.v, b.v);
    ExpectVec(t, "sllv_epi8", k, got, want);
    // vperm: byte i comes from (bit 4 of the control ? b : a)[15 - (control & 15)]; the control here is kB of
    // the next row, which mixes both sources.
    const Vec c = Load(kB[(k + 1) & 3]);
    for (int i = 0; i < 16; ++i) want.u8[i] = ((c.u8[i] & 0x10) ? b : a).u8[15 - (c.u8[i] & 15)];
    got.v = rex::ppc::simde_mm_perm_epi8_(a.v, b.v, c.v);
    ExpectVec(t, "perm_epi8", k, got, want);
  }
  // The two saturating conversions: truncation; out of range saturates; a NaN gives 0.
  static const float kF[2][4] = {
      {0.75f, -0.75f, 2147483648.0f, -2147483904.0f},
      {4294967296.0f, 123456.7f, std::numeric_limits<float>::quiet_NaN(), -1.0f},
  };
  for (int k = 0; k < 2; ++k) {
    Vec a, want, got;
    std::memcpy(a.f32, kF[k], 16);
    a.v = Opaque(a.v);
    for (int i = 0; i < 4; ++i) {
      const float f = a.f32[i];
      want.s32[i] = std::isnan(f) ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : int32_t(f);
    }
    got.v = rex::ppc::simde_mm_vctsxs(a.f);
    ExpectVec(t, "vctsxs", k, got, want);
    for (int i = 0; i < 4; ++i) {
      const float f = a.f32[i];
      want.u32[i] = (std::isnan(f) || f <= 0.0f) ? 0u : f >= 4294967296.0f ? 0xFFFFFFFFu : uint32_t(f);
    }
    got.v = rex::ppc::simde_mm_vctuxs(a.f);
    ExpectVec(t, "vctuxs", k, got, want);
  }
}

}  // namespace

namespace sa2::enh {

void FormsSelfCheck() {
  Tally t;
  Conversions(t);
  Divisions(t);
  FloatCopies(t);
  VectorHelpers(t);
  if (t.wrong) {
    for (int i = 0; i < t.wrong; ++i) VerifyDifference();
    REXLOG_ERROR("[enh] DIFFERENCE: instruction forms self-check: {} of {} results are NOT as expected (see the lines "
                 "above); this build's helpers do not match their reference on this processor",
                 t.wrong, t.checks);
    // The released configuration logs nothing, and a wrong form must not go unseen on the console: the failures
    // also go to a file next to the program. Nothing is written when every result is as expected.
    const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "sa2_selfcheck_failed.txt";
    if (std::FILE* f = std::fopen(path.string().c_str(), "w")) {
      std::fprintf(f, "sa2-nx instruction forms self-check: %d of %d results are not as expected\n%s", t.wrong,
                   t.checks, t.report.c_str());
      std::fclose(f);
    }
  } else {
    REXLOG_INFO("[enh] instruction forms self-check: {} of {} results as expected ({})", t.checks, t.checks,
#if defined(__aarch64__)
                "the AArch64 forms"
#else
                "the x86 forms"
#endif
    );
  }
}

}  // namespace sa2::enh
