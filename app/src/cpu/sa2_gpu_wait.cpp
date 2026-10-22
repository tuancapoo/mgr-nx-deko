// Sonic Adventure 2: the game's Direct3D GPU wait sleeps instead of spinning (CPU enhancement sa2_cpu_gpu_wait_sleep).
//
// WHAT THE GAME DOES
//   Direct3D's waits for the GPU (ring space: sub_8254D6E8; fences and the others, 7 call sites) all loop on one
//   iteration function, sub_8254F400(r3 = the wait record sub_8254F308 filled: [0] device, [8] the read pointer seen
//   last time, [12] the tick of its last change, [20] the time base at the start). One iteration is 32 db16cyc delays,
//   then: GPU already declared hung (bit 0x02 of [device+11069]) -> 0 (stop); the read pointer ([[device+11024]])
//   moved -> remember it and its tick; no movement for longer than the game's limit -> the hang handler; else 1 (keep
//   waiting). The caller re-checks its condition after each iteration.
//   On the PC profile the game thread spent about 70 % of its time in this loop; on the console the game thread showed
//   ~100 % of a core whatever the frame rate, on a machine with 3 cores for the game.
//
// WHAT THIS DOES
//   Before an iteration in which nothing can have changed - the same wait record as the previous iteration on this
//   thread, the read pointer still the one the record saw, and no ring progress since the previous iteration returned
//   (the progress counter of me_ring_wait.cpp, bumped by the ring thread after each ring segment, each fence write and
//   each interrupt) - it sleeps until the ring thread reports progress, at most sa2_gpu_wait_max_us. Then the original
//   iteration runs as always. The counter is read AFTER the original returns, i.e. before the caller re-checks its
//   condition, so a change the caller could have missed always shows as progress and the next iteration does not sleep.
//
// WHY IT IS EXACT: the game reads the same memory and takes the same decisions; it only reaches them later, by at most
//   one wake-up latency (or max_us when the wake-up comes from something the ring does not report).
// RISK: a wait whose condition changes without ring progress or an interrupt gains up to max_us per iteration.
// KEY: sa2_cpu_gpu_wait_sleep (with sa2_cpu_enhancements); sa2_gpu_wait_max_us.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/platform.h>
#include <rex/ppc.h>

#include "cpu/sa2_enhancements.h"
#include "me/me_native_system.h"

SA2_ENHANCEMENT(g_gpu_wait_sleep, sa2_cpu_gpu_wait_sleep, sa2::enh::kCpu, true,
                "The game's Direct3D GPU wait (sub_8254F400) sleeps until the ring thread makes progress instead of "
                "spinning");
REXCVAR_DEFINE_INT32(sa2_gpu_wait_max_us, 1000, "SA2 enhancements",
                     "sa2_cpu_gpu_wait_sleep: longest sleep per iteration of the game's GPU wait, in microseconds")
    .range(50, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kIteration = 0x8254F400;
constexpr uint32_t kIterationBytes = 224;
constexpr uint64_t kIterationHash = 0xDEA4A353D6DE7E41ull;

constexpr uint32_t kDeviceReadPointerWord = 11024;  // address of the word the GPU writes the read pointer back to
constexpr uint32_t kDeviceState = 11069;            // bit 0x02: GPU declared hung

inline uint32_t PhysOff(uint32_t a) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return a >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)a;
  return 0u;
#endif
}
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a + PhysOff(a), 4);
  return __builtin_bswap32(v);
}
inline uint8_t Ld8(const uint8_t* base, uint32_t a) { return base[a + PhysOff(a)]; }

// The previous iteration on this thread: its record, the record's start time base, and the progress counter when it
// returned.
thread_local uint32_t t_record = 0;
thread_local uint32_t t_record_start = 0;
thread_local uint32_t t_progress = 0;

std::atomic<int> g_state{0};  // 0 unknown, 1 on, -1 off (code check failed or key off); set by the first call

}  // namespace

REX_EXTERN(__imp__sub_8254F400);
REX_HOOK_RAW(sub_8254F400) {
  int state = g_state.load(std::memory_order_relaxed);
  if (state == 0) {
    state = (g_gpu_wait_sleep.active &&
             sa2::enh::GuestCodeMatches(base, kIteration, kIterationBytes, kIterationHash, "sa2_cpu_gpu_wait_sleep"))
                ? 1
                : -1;
    g_state.store(state, std::memory_order_relaxed);
  }
  if (state < 0) {
    __imp__sub_8254F400(ctx, base);
    return;
  }
  const uint32_t record = ctx.r3.u32;
  const uint32_t start = Ld32(base, record + 20);
  if (record == t_record && start == t_record_start) {
    const uint32_t device = Ld32(base, record);
    if (device && !(Ld8(base, device + kDeviceState) & 0x02)) {
      const uint32_t word = Ld32(base, device + kDeviceReadPointerWord);
      if (word && Ld32(base, word) == Ld32(base, record + 8) && me::native::RingProgress() == t_progress) {
        static const uint32_t max_us = uint32_t(REXCVAR_GET(sa2_gpu_wait_max_us));
        const auto t0 = std::chrono::steady_clock::now();
        me::native::WaitRingProgress(t_progress, max_us);
        g_gpu_wait_sleep.Use();
        g_gpu_wait_sleep.Add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count()));
      }
    }
  }
  __imp__sub_8254F400(ctx, base);
  t_record = record;
  t_record_start = start;
  t_progress = me::native::RingProgress();
}

namespace {
const bool g_unit = [] {
  g_gpu_wait_sleep.amount_unit = "us asleep";
  return true;
}();
}  // namespace
