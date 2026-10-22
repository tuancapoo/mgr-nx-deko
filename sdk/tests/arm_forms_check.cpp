// Compile-only check for the Switch compiler (it cannot run on the PC): the AArch64-only forms of the CPU round of
// 2026-10-12, one small function each, so tools/arm_forms_check.py can read the instructions GCC makes of them.
//   - rex/thread/atomic.h: the inline __atomic lock words (exclusive pair with acquire/release, no trailing `dmb`
//     outside the branch on g_rex_atomic_legacy_barrier), next to the legacy __sync forms they replace;
//   - rex/ppc/intrinsics.h: vperm and the two float-to-integer conversions as their NEON instruction, with
//     REX_VMX_HELPERS_X86_FORM the x86 emulation through simde;
//   - rex/ppc/diet_ops.h: the any-mode float helpers compile (their PC test is sdk/tests/fp_any_mode_test.cpp).
//
//   python tools/arm_forms_check.py
#include <cstdint>

#include <rex/ppc/diet_ops.h>
#include <rex/ppc/intrinsics.h>
#include <rex/thread/atomic.h>

#define CHECK_FN extern "C" __attribute__((noinline))

CHECK_FN int32_t check_atomic_inc(volatile int32_t* p) { return rex::thread::atomic_inc(p); }
CHECK_FN int32_t check_atomic_dec(volatile int32_t* p) { return rex::thread::atomic_dec(p); }
CHECK_FN bool check_atomic_cas32(volatile int32_t* p) { return rex::thread::atomic_cas(int32_t(-1), int32_t(0), p); }
CHECK_FN bool check_atomic_cas64(volatile int64_t* p, int64_t a, int64_t b) { return rex::thread::atomic_cas(a, b, p); }
CHECK_FN int32_t check_atomic_exchange_add(volatile int32_t* p, int32_t n) {
  return rex::thread::atomic_exchange_add(n, p);
}

// The legacy forms (what atomic_posix.cpp compiled before, and what the generator's stwcx. was).
CHECK_FN int32_t check_legacy_inc(volatile int32_t* p) { return __sync_add_and_fetch(p, 1); }
CHECK_FN bool check_legacy_cas32(volatile int32_t* p) { return __sync_bool_compare_and_swap(p, -1, 0); }

CHECK_FN simde__m128i check_vperm(simde__m128i a, simde__m128i b, simde__m128i c) {
  return rex::ppc::simde_mm_perm_epi8_(a, b, c);
}
CHECK_FN simde__m128i check_vctsxs(simde__m128 a) { return rex::ppc::simde_mm_vctsxs(a); }
CHECK_FN simde__m128i check_vctuxs(simde__m128 a) { return rex::ppc::simde_mm_vctuxs(a); }

CHECK_FN double check_add_single(double a, double b) { return rex::ppc::AddSingleAnyMode(a, b); }
CHECK_FN double check_round_single(double a) { return rex::ppc::RoundToSingleAnyMode(a); }
CHECK_FN bool check_ge_zero(double a) { return rex::ppc::GeZeroAnyMode(a); }

// The second CPU round of 2026-10-12 (sa2-nx).
//   - rex/ppc/diet_ops.h: fctid / fctiw as `frinti` + `fcvtzs`, fctidz as `fcvtzs` (codegen option fcti_inline; no
//     call of round()); SingleToDoublePure and QuietSingleBits (fp_copy_pairs) without a call and without `msr`.
//   - rex/ppc/intrinsics.h: the small VMX helpers as their NEON instruction.
CHECK_FN int64_t check_fctid(double v) { return rex::ppc::fctid(v); }
CHECK_FN int64_t check_fctiw(double v) { return rex::ppc::fctiw(v); }
CHECK_FN int64_t check_fctidz(double v) { return rex::ppc::fctidz(v); }
CHECK_FN double check_single_pure(uint32_t bits) { return rex::ppc::SingleToDoublePure(bits); }
CHECK_FN uint32_t check_quiet_single(uint32_t bits) { return rex::ppc::QuietSingleBits(bits); }
CHECK_FN simde__m128i check_adds_epu32(simde__m128i a, simde__m128i b) { return rex::ppc::simde_mm_adds_epu32(a, b); }
CHECK_FN simde__m128i check_avg_epi8(simde__m128i a, simde__m128i b) { return rex::ppc::simde_mm_avg_epi8(a, b); }
CHECK_FN simde__m128i check_avg_epi16(simde__m128i a, simde__m128i b) { return rex::ppc::simde_mm_avg_epi16(a, b); }
CHECK_FN simde__m128i check_avg_epi32(simde__m128i a, simde__m128i b) { return rex::ppc::simde_mm_avg_epi32(a, b); }
CHECK_FN simde__m128 check_cvtepu32_ps(simde__m128i a) { return rex::ppc::simde_mm_cvtepu32_ps_(a); }
CHECK_FN simde__m128i check_cmpgt_epu8(simde__m128i a, simde__m128i b) { return rex::ppc::simde_mm_cmpgt_epu8(a, b); }
CHECK_FN simde__m128i check_cmpgt_epu16(simde__m128i a, simde__m128i b) { return rex::ppc::simde_mm_cmpgt_epu16(a, b); }
CHECK_FN simde__m128i check_sllv_epi16(simde__m128i a, simde__m128i c) { return rex::ppc::simde_mm_sllv_epi16(a, c); }
CHECK_FN simde__m128i check_srlv_epi16(simde__m128i a, simde__m128i c) { return rex::ppc::simde_mm_srlv_epi16(a, c); }
CHECK_FN simde__m128i check_srav_epi16(simde__m128i a, simde__m128i c) { return rex::ppc::simde_mm_srav_epi16(a, c); }
CHECK_FN simde__m128i check_sllv_epi8(simde__m128i a, simde__m128i c) { return rex::ppc::simde_mm_sllv_epi8(a, c); }

// The third CPU round (codegen option div_inline): the divisions as the instruction, without a branch, next to the
// guarded C expressions they replace.
CHECK_FN uint32_t check_divwu(uint32_t n, uint32_t d) { return rex::ppc::divwu(n, d); }
CHECK_FN uint32_t check_divw(int32_t n, int32_t d) { return rex::ppc::divw(n, d); }
CHECK_FN uint64_t check_divdu(uint64_t n, uint64_t d) { return rex::ppc::divdu(n, d); }
CHECK_FN int64_t check_divd(int64_t n, int64_t d) { return rex::ppc::divd(n, d); }
CHECK_FN uint32_t check_old_divwu(uint32_t n, uint32_t d) { return d ? n / d : 0; }
CHECK_FN uint32_t check_old_divw(int32_t n, int32_t d) {
  return uint32_t((d && !(n == INT32_MIN && d == -1)) ? n / d : 0);
}
CHECK_FN uint32_t check_divw_by_10(int32_t n) { return rex::ppc::divw(n, 10); }
