// Stage 2, phase A of the native renderer: confirm the map of the game's Direct3D before replacing anything.
//
// SA2 carries the Xbox 360 XDK's Direct3D statically linked. Reading the recompiled code gives candidates
// (docs/sa2-notes.md: the functions that write the GPU's MMIO registers); this file checks them at run time. It
// wraps each candidate, always calls the original, and with sa2_d3d_trace=true logs every 5 seconds how often each
// one ran and from where (its most frequent return addresses). With the cvar off it only adds one branch per call.
//
// How the wrapping works: each recompiled function sub_XXXXXXXX is a weak alias of __imp__sub_XXXXXXXX. The strong
// definitions here win for every caller (direct, and indirect through the function table), and __imp__ is still the
// original.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "sa2_capture.h"

REXCVAR_DEFINE_BOOL(sa2_d3d_trace, false, "SA2",
                    "Stage 2 tracing: count the calls to the game's Direct3D ring/present candidates "
                    "(docs/sa2-notes.md) and log them every 5 s with their most frequent callers");

namespace {

struct Traced {
  uint32_t address;
  const char* what;
  std::atomic<uint64_t> calls{0};
  std::mutex mu;
  std::unordered_map<uint32_t, uint64_t> callers;  // return address -> calls
};

Traced g_traced[] = {
    {0x8254D340, "kick-off (CP_RB_WPTR)"},
    {0x8254DC00, "kick-off 2 (CP_RB_WPTR)"},
    {0x8254D420, "ring/interrupt setup"},
    {0x82557FC0, "present (D1GRPH_PRIMARY_SURFACE_ADDRESS)"},
    {0x825580F0, "display mode / present setup"},
    {0x8255AE60, "MMIO 0x112C"},
    {0x82568B98, "MMIO 0x112C/0xE5C"},
    {0x8255C0F0, "MMIO 0x268/0x3214/0x3408"},
    {0x825666F0, "MMIO 0x0/0x6034"},
};

std::atomic<int64_t> g_next_report_ms{0};

void Report() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  int64_t due = g_next_report_ms.load(std::memory_order_relaxed);
  if (now < due || !g_next_report_ms.compare_exchange_strong(due, now + 5000)) {
    return;
  }
  REXLOG_INFO("[d3d-trace] calls since the last report:");
  for (auto& t : g_traced) {
    const uint64_t calls = t.calls.exchange(0, std::memory_order_relaxed);
    if (!calls) {
      continue;
    }
    std::vector<std::pair<uint32_t, uint64_t>> top;
    {
      std::lock_guard lock(t.mu);
      top.assign(t.callers.begin(), t.callers.end());
      t.callers.clear();
    }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string from;
    for (size_t i = 0; i < top.size() && i < 4; ++i) {
      from += fmt::format(" {:08X}x{}", top[i].first, top[i].second);
    }
    REXLOG_INFO("[d3d-trace]   sub_{:08X} {:<42} {:>8} calls, from{}", t.address, t.what, calls, from);
  }
}

inline void Record(Traced& t, const PPCContext& ctx) {
  t.calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t lr = uint32_t(ctx.lr);
  std::lock_guard lock(t.mu);
  ++t.callers[lr];
}

}  // namespace

// One wrapper per candidate: record (when tracing), report now and then, call the original.
#define SA2_TRACE_HOOK(index, addr)                    \
  extern "C" REX_FUNC(__imp__sub_##addr);              \
  REX_HOOK_RAW(sub_##addr) {                           \
    if (REXCVAR_GET(sa2_d3d_trace)) {                  \
      Record(g_traced[index], ctx);                    \
      Report();                                        \
    }                                                  \
    __imp__sub_##addr(ctx, base);                      \
  }

SA2_TRACE_HOOK(0, 8254D340)
SA2_TRACE_HOOK(1, 8254DC00)
SA2_TRACE_HOOK(2, 8254D420)
SA2_TRACE_HOOK(4, 825580F0)

// The flip (once per presented frame, from the vblank interrupt) also counts the game's frames, always: the video
// recorder (sa2_capture.cpp) captures one image per flip.
extern "C" REX_FUNC(__imp__sub_82557FC0);
REX_HOOK_RAW(sub_82557FC0) {
  if (REXCVAR_GET(sa2_d3d_trace)) {
    Record(g_traced[3], ctx);
    Report();
  }
  sa2::capture::BeforeFlip();  // sa2_video_lockstep: the recorder takes every frame
  __imp__sub_82557FC0(ctx, base);
  g_sa2_game_frames.fetch_add(1, std::memory_order_relaxed);
}
SA2_TRACE_HOOK(5, 8255AE60)
SA2_TRACE_HOOK(6, 82568B98)
SA2_TRACE_HOOK(7, 8255C0F0)
SA2_TRACE_HOOK(8, 825666F0)
