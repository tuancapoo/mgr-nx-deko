// The runner extracts the current generated sub_825B4070 without rewriting its
// expressions. This test uses the real SDK PPCContext, not a reduced mock.
#include <rex/ppc/context.h>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

// Profiling census is outside the leaf's guest-visible state and is compiled
// out on Switch. Keep the standalone host test independent of SDK linking.
namespace rex::syscount { void CensusSite(unsigned) {} }

#define MGR_WWISE_DSP_TEST
#include "../mgr_src/mgr_wwise_dsp.inc"
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U32(address) mgr::wwise::Read32(base, address)
#define REX_STORE_U32(address, value) mgr::wwise::Write32(base, address, value)
#define REX_LOAD_D_U32(reg, displacement) REX_LOAD_U32(uint32_t((reg) + (displacement)))
#define REX_STORE_D_U32(reg, displacement, value) REX_STORE_U32(uint32_t((reg) + (displacement)), value)
#define REX_LOAD_D_F32(reg, displacement) std::bit_cast<float>(uint32_t(REX_LOAD_D_U32(reg, displacement)))
// Model the Switch's proven-elided checks, and verify their precondition rather
// than silently reinstating the original operation in the differential oracle.
#define REX_FLUSH_KNOWN_OFF() do { if (!ctx.fpscr.flushModeIsOff()) std::abort(); } while (false)
#define REX_LOOP_BARRIER() std::atomic_signal_fence(std::memory_order_seq_cst)
#define REX_LEAF_LOCAL(type, name, reg) type name = ctx.reg
#define REX_LEAF_STORE(name, reg) ctx.reg = name
#include "mgr_wwise_filter_oracle.inc"

namespace {
using Memory = std::array<uint8_t, 32768>;
std::mt19937_64 random(0x825B4070);
unsigned native_calls = 0;

void Store(Memory& memory, uint32_t address, uint32_t bits) {
  mgr::wwise::Write32(memory.data(), address, bits);
}

bool Compare(const PPCContext& initial, const Memory& memory, unsigned case_id) {
  PPCContext expected, actual;
  std::memcpy(&expected, &initial, sizeof(expected));
  std::memcpy(&actual, &initial, sizeof(actual));
  auto expected_memory = memory;
  auto actual_memory = memory;
  expected.fpscr.setcsr(initial.fpscr.csr);
  OriginalFilter(expected, expected_memory.data());
  const auto expected_mode = expected.fpscr.getcsr();
  actual.fpscr.setcsr(initial.fpscr.csr);
  if (mgr::wwise::CanFilter(actual, actual_memory.data())) {
    ++native_calls;
    mgr::wwise::Filter(actual, actual_memory.data());
#ifdef MGR_WWISE_TEST_NEGATIVE_CONTROL
    actual_memory[actual.r3.u32] ^= 1;
#endif
  } else
    OriginalFilter(actual, actual_memory.data());
  const auto actual_mode = actual.fpscr.getcsr();
  if (expected_mode != actual_mode || expected_memory != actual_memory ||
      std::memcmp(&expected, &actual, sizeof(expected))) {
    std::printf("FAIL case %u count %u state %x coeff %x cursor %x mode %x/%x\n",
                case_id, initial.r6.u32, initial.r3.u32, initial.r4.u32,
                initial.r5.u32, expected_mode, actual_mode);
    const auto* a = reinterpret_cast<const uint8_t*>(&expected);
    const auto* b = reinterpret_cast<const uint8_t*>(&actual);
    for (size_t i = 0; i < sizeof(expected); ++i)
      if (a[i] != b[i]) { std::printf("first context diff +%zu: %02x/%02x\n", i, a[i], b[i]); break; }
    for (size_t i = 0; i < memory.size(); ++i)
      if (expected_memory[i] != actual_memory[i]) { std::printf("first memory diff +%zu\n", i); break; }
    return false;
  }
  return true;
}

double Benchmark(bool native, bool rejected) {
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  Memory memory{};
  ctx.r3.u32 = 128; ctx.r4.u32 = 160; ctx.r5.u32 = 192; ctx.r6.u32 = 256;
  for (uint32_t at = 4096; at < 5120; at += 4)
    Store(memory, at, std::bit_cast<uint32_t>(0.25f));
  Store(memory, 160, std::bit_cast<uint32_t>(0.25f));
  Store(memory, 164, std::bit_cast<uint32_t>(0.25f));
  Store(memory, 168, std::bit_cast<uint32_t>(rejected ? 1.8f : 0.2f));
  Store(memory, 172, std::bit_cast<uint32_t>(rejected ? -0.9f : 0.05f));
  const auto begin = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 12000; ++i) {
    for (uint32_t at = 128; at < 144; at += 4) Store(memory, at, 0);
    Store(memory, 192, 4096);
    if (native && mgr::wwise::CanFilter(ctx, memory.data())) mgr::wwise::Filter(ctx, memory.data());
    else OriginalFilter(ctx, memory.data());
  }
  volatile uint64_t checksum = ctx.f13.u64;
  (void)checksum;
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 12000;
}
}  // namespace

int main() {
  PPCFPSCRRegister host{};
  const uint32_t saved_mode = host.getcsr();
  constexpr uint32_t special[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff,
    0x00800000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000,
    0x7fc12345, 0xffc54321, 0x7f800001, 0xff800001, 0x3f800000, 0xbf800000};
  unsigned case_id = 0;
  for (unsigned mode = 0; mode < 8; ++mode) {
    for (unsigned test = 0; test < 1800; ++test) {
      PPCContext ctx;
      auto* bytes = reinterpret_cast<uint8_t*>(&ctx);
      for (size_t i = 0; i < sizeof(ctx); ++i) bytes[i] = uint8_t(random());
      Memory memory;
      for (size_t i = 0; i < memory.size(); i += 4) {
        uint32_t value = test < 256 ? special[(i / 4 + test) % std::size(special)] : uint32_t(random());
        // Half the randomized cases use ordinary, stable audio-range floats.
        if (test >= 256 && (test & 1))
          value = std::bit_cast<uint32_t>(float(int32_t(random() % 20001) - 10000) / 20000.0f);
        Store(memory, uint32_t(i), value);
      }
      const unsigned counts[] = {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 32, 63, 64, 255, 256, 1023, 1024, 4095, 4096};
      ctx.r6.u64 = (random() & 0xffffffff00000000ull) | counts[test % std::size(counts)];
      ctx.r3.u64 = 128;
      ctx.r4.u64 = 160;
      ctx.r5.u64 = 192;
      const uint32_t buffer = 4096;
      // Overlap state, coefficients, cursor and sample buffer in both directions.
      switch ((test / std::size(counts)) % 9) {
        case 1: ctx.r3.u64 = buffer; break;
        case 2: ctx.r4.u64 = buffer + 4; break;
        case 3: ctx.r3.u64 = ctx.r4.u64; break;
        case 4: ctx.r3.u64 = buffer + 12; ctx.r4.u64 = buffer + 8; break;
        case 5: ctx.r5.u64 = buffer + 8; break;
        case 6: ctx.r3.u64 = ctx.r5.u64; break;
        case 7: ctx.r4.u64 = ctx.r5.u64; break;
        case 8: ctx.r3.u64 = buffer - 4; ctx.r4.u64 = buffer - 8; break;
      }
      Store(memory, ctx.r5.u32, buffer);
      ctx.xer.so &= 1; ctx.xer.ov &= 1; ctx.xer.ca &= 1;
      ctx.fpscr.csr = (saved_mode & ~(PPCFPSCRRegister::RoundMaskVal | PPCFPSCRRegister::FlushMask)) |
        rex::platform::FPSCRPlatform::GuestToHost[mode & 3] |
        ((mode & 4) ? uint32_t(PPCFPSCRRegister::FlushMask) : 0u);
      rex::platform::FPSCRPlatform::InitHostExceptions(ctx.fpscr.csr);
      if (!Compare(ctx, memory, case_id++)) { host.setcsr(saved_mode); return 1; }
    }
    PPCContext boundary{};
    Memory memory{};
    boundary.r3.u32 = 128; boundary.r4.u32 = 160;
    boundary.r5.u32 = 192; boundary.r6.u32 = 4096;
    boundary.fpscr.csr = (saved_mode & ~(PPCFPSCRRegister::RoundMaskVal | PPCFPSCRRegister::FlushMask)) |
      rex::platform::FPSCRPlatform::GuestToHost[mode & 3] |
      ((mode & 4) ? uint32_t(PPCFPSCRRegister::FlushMask) : 0u);
    rex::platform::FPSCRPlatform::InitHostExceptions(boundary.fpscr.csr);
    for (uint32_t at = 128; at < 144; at += 4) Store(memory, at, 0x5f800000u | ((at & 4) << 29));
    Store(memory, 160, 0x41800000); Store(memory, 164, 0xc1800000);
    Store(memory, 168, 0x3f400000); Store(memory, 172, 0xbe800000);
    Store(memory, 192, 4096);
    for (uint32_t at = 4096; at < 20480; at += 4) Store(memory, at, 0x5f800000u | ((at & 4) << 29));
    if (!mgr::wwise::CanFilter(boundary, memory.data()) || !Compare(boundary, memory, case_id++)) return 4;
  }
  // Rejected ranges must leave both context and guest memory untouched.
  PPCContext rejected{};
  Memory memory{};
  rejected.r3.u32 = 128; rejected.r4.u32 = 160; rejected.r5.u32 = 192;
  for (auto count : {4097u, 0xffffffffu}) {
    rejected.r6.u32 = count;
    if (mgr::wwise::CanFilter(rejected, memory.data())) return 2;
  }
  rejected.r6.u32 = 4;
  Store(memory, 192, 0x7ffffffcu);
  if (mgr::wwise::CanFilter(rejected, memory.data())) return 3;
  rejected.r3.u32 = 0xfffffffcu;
  if (mgr::wwise::CanFilter(rejected, memory.data())) return 5;
  rejected.r3.u32 = 128; rejected.r4.u32 = 0xfffffffcu;
  if (mgr::wwise::CanFilter(rejected, memory.data())) return 6;
  host.setcsr(saved_mode);
  std::printf("PASS %u bitwise whole-context/memory/host-mode cases (%u native, others exact fallback); aliases, all rounding/flush modes, denormal/NaN/Inf/signed-zero and guard boundaries\n", case_id, native_calls);
  for (bool rejected : {false, true}) {
    const double original = Benchmark(false, rejected), candidate = Benchmark(true, rejected);
    std::printf("BENCH x64 count256 %s: original %.3fus guarded candidate %.3fus ratio %.3f\n",
                rejected ? "feedback1.8/-0.9(fallback)" : "feedback0.2/0.05(native)",
                original, candidate, candidate / original);
  }
  host.setcsr(saved_mode);
}
