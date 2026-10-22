// Sonic Adventure 2: the game's Direct3D at its API boundary (the API-level renderer's game-thread side).
//
// The census (sa2_diag_d3d_census, a diagnostic): which Direct3D functions the game really calls, how often, and what
// command-buffer (PM4) packets each call writes. The census hooks themselves are not part of the normal build: they
// are generated for a census build only (scratchpad d3d_census_build.py: out/census/sa2_d3d_census_all.cpp, every
// function in the Direct3D address range that has no hook of its own, added through SA2_EXTRA_SOURCES and
// direct_calls.py --hooked-list), so the normal build's direct calls stay as they are. In the normal build only the
// renderer's own hooks (Draw*, sa2_me_hooks.cpp; the inline-vertex draw below) call the census, and do nothing more
// than a check of the cvar unless it is on.
//
// With the census on, each outermost call (no other census-hooked function below it on the thread) counts and decodes
// the PM4 it wrote: the device's write pointer (+48) before and after the call. When the call runs out of space, the
// command buffer's space function (hooked by the census build) closes the old segment (decoded up to its write
// pointer) and opens the new one. A report every 10 s lists every function called, its calls (outermost and nested), its words
// per call and its packet signatures; sa2_diag_d3d_census_dump names functions whose first calls are logged word by
// word. No callers: the optimised flavour elides LR stores, so ctx.lr is stale (the scratchpad's d3d_entries.py lists
// the static callers).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "cpu/sa2_hot_copies.h"
#include "me/sa2_api.h"
#include "me/sa2_d3d_api.h"

REXCVAR_DEFINE_BOOL(sa2_diag_d3d_census, false, "SA2",
                    "Diagnostic: count the Direct3D functions the game calls, with the PM4 packets each call writes "
                    "(a report every 10 s; needs a census build for more than the draw functions). Changes nothing");
REXCVAR_DEFINE_STRING(sa2_diag_d3d_census_dump, "", "SA2",
                      "Diagnostic (with sa2_diag_d3d_census): comma-separated function addresses (hex) whose first 6 "
                      "outermost calls are logged with every PM4 word they wrote");

namespace sa2::d3d {
namespace {

struct Stats {
  uint64_t calls = 0, nested = 0, words = 0, segments = 0;
  std::map<std::string, uint64_t> packets;  // signature -> count
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Stats> g_stats;
std::unordered_map<uint32_t, uint32_t> g_dumps_left;  // function -> calls still to dump
std::atomic<uint32_t> g_device{0};                    // the Direct3D device (r3 of a Draw* call)
auto g_last_report = std::chrono::steady_clock::now();

thread_local int t_depth = 0;
struct Frame {
  uint32_t address;
  uint32_t wptr, limit;  // the segment being decoded: from wptr on
  bool valid, dump;
  std::string words;  // the dump
};
thread_local Frame t_frame{};

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + (address >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}

std::string Signature(uint32_t h, const uint8_t* base, uint32_t p) {
  const uint32_t type = h >> 30;
  if (type == 0) {
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    const uint32_t reg = h & 0x7FFF;
    // Registers by group: constants and fetch constants by range, the rest by exact base.
    if (reg >= 0x4000 && reg < 0x4800) return fmt::format("t0 ALU consts {:04X}..+{}", reg & ~0x3Fu, count);
    if (reg >= 0x4800 && reg < 0x4900) return fmt::format("t0 fetch consts {:04X}..+{}", reg & ~0x5u, count);
    return fmt::format("t0 {:04X}+{}{}", reg, count, (h >> 15) & 1 ? " one-reg" : "");
  }
  if (type == 1) return fmt::format("t1 {:03X},{:03X}", h & 0x7FF, (h >> 11) & 0x7FF);
  if (type == 2) return "t2 filler";
  const uint32_t opcode = (h >> 8) & 0x7F;
  if (opcode == 0x10) {  // NOP: the renderer's own markers
    const uint32_t magic = Load32(base, p + 4);
    if (magic == 0x53413244u) return "t3 NOP marker SA2D";
    if (magic == 0x53413246u) return "t3 NOP placeholder SA2F";
    return fmt::format("t3 NOP ({} words)", ((h >> 16) & 0x3FFF) + 1);
  }
  return fmt::format("t3 {:02X}{} ({} words)", opcode, (h & 1) ? " predicated" : "", ((h >> 16) & 0x3FFF) + 1);
}

// The packets in [first, last] (addresses of words; first = the word after the old write pointer). Caller holds
// g_mutex.
void Decode(Stats& s, const uint8_t* base, uint32_t first, uint32_t last) {
  if (last < first) return;
  s.words += (last - first) / 4 + 1;
  if (t_frame.dump) {
    for (uint32_t p = first; p <= last; p += 4) t_frame.words += fmt::format(" {:08X}", Load32(base, p));
    t_frame.words += " |";
  }
  for (uint32_t p = first; p <= last;) {
    const uint32_t h = Load32(base, p);
    const uint32_t type = h >> 30;
    uint32_t words = 1;
    if (type == 0) words = h ? 1 + ((h >> 16) & 0x3FFF) + 1 : 1;
    else if (type == 1) words = 3;
    else if (type == 3) words = 1 + ((h >> 16) & 0x3FFF) + 1;
    if (h) ++s.packets[Signature(h, base, p)];
    p += 4 * words;
  }
}

// The device's flag bytes (+11068..+11072) at each draw: which of Direct3D's draw-time branches SA2 takes.
std::map<uint64_t, uint64_t> g_flags;

void Report() {
  {
    std::string f;
    for (const auto& [k, n] : g_flags) f += fmt::format(" [{:010X}] x{};", k, n);
    REXLOG_INFO("[d3d census] draw-time device flags (+11068..+11072):{}", f);
    g_flags.clear();
  }
  std::vector<std::pair<uint64_t, uint32_t>> order;
  for (const auto& [address, s] : g_stats)
    if (s.calls || s.nested) order.push_back({s.calls * 4 + s.nested, address});
  std::sort(order.rbegin(), order.rend());
  REXLOG_INFO("[d3d census] last 10 s: {} functions called", order.size());
  for (const auto& [rank, address] : order) {
    Stats& s = g_stats[address];
    std::vector<std::pair<uint64_t, std::string>> packets;
    for (const auto& [sig, n] : s.packets) packets.push_back({n, sig});
    std::sort(packets.rbegin(), packets.rend());
    std::string p;
    for (size_t k = 0; k < packets.size() && k < 16; ++k)
      p += fmt::format(" [{}] x{};", packets[k].second, packets[k].first);
    REXLOG_INFO("[d3d census] sub_{:08X}: {} calls ({} nested), {:.1f} PM4 words/call, {} segments; packets:{}",
                address, s.calls, s.nested, s.calls ? double(s.words) / double(s.calls) : 0.0, s.segments, p);
    s = Stats{};
  }
}

// The device before the first draw: Direct3D keeps it behind a pointer the title process reads (a check against the
// process type in its release path chooses this one).
uint32_t Device(const uint8_t* base) {
  if (const uint32_t d = g_device.load(std::memory_order_relaxed)) return d;
  const uint32_t holder = Load32(base, 0x8200070Cu);
  if (holder < 0x40000000u || holder >= 0xA0000000u) return 0;
  const uint32_t device = Load32(base, holder);
  return device >= 0x40000000u && device < 0xA0000000u ? device : 0;
}

void ParseDumps() {
  const std::string list = REXCVAR_GET(sa2_diag_d3d_census_dump);
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find_first_of(",; ", i);  // ';' too: the test scripts split their settings on ','
    if (j == std::string::npos) j = list.size();
    const std::string item = list.substr(i, j - i);
    if (!item.empty()) g_dumps_left[uint32_t(std::stoul(item, nullptr, 16))] = 6;
    i = j + 1;
  }
}

}  // namespace

void NoteDevice(uint32_t device) { g_device.store(device, std::memory_order_relaxed); }

void NoteDrawFlags(const uint8_t* base, uint32_t device) {
  if (!CensusOn() || !device) return;
  uint64_t k = 0;
  for (uint32_t i = 0; i < 5; ++i) k = (k << 8) | base[device + 11068 + i];
  std::lock_guard lock(g_mutex);
  ++g_flags[k];
}

bool CensusOn() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(sa2_diag_d3d_census);
    if (v) ParseDumps();
    return v;
  }();
  return on;
}

void CensusEnter(uint32_t address, const uint8_t* base) {
  if (!CensusOn()) return;
  if (t_depth++ > 0) {
    std::lock_guard lock(g_mutex);
    ++g_stats[address].nested;
    return;
  }
  const uint32_t device = Device(base);
  t_frame.address = address;
  t_frame.valid = device != 0;
  t_frame.words.clear();
  if (t_frame.valid) {
    t_frame.wptr = Load32(base, device + 48);
    t_frame.limit = Load32(base, device + 52);
  }
  std::lock_guard lock(g_mutex);
  ++g_stats[address].calls;
  auto dump = g_dumps_left.find(address);
  t_frame.dump = dump != g_dumps_left.end() && dump->second > 0;
  if (t_frame.dump) --dump->second;
}

void CensusLeave(const uint8_t* base) {
  if (!CensusOn()) return;
  if (--t_depth > 0) return;
  const uint32_t device = Device(base);
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[t_frame.address];
  if (t_frame.valid && device) {
    const uint32_t wptr = Load32(base, device + 48), limit = Load32(base, device + 52);
    if (limit == t_frame.limit && wptr >= t_frame.wptr) {
      Decode(s, base, t_frame.wptr + 4, wptr);
    } else {
      ++s.segments;  // a switch the space function did not see (not hooked in this build)
    }
  }
  if (t_frame.dump)
    REXLOG_INFO("[d3d census] dump sub_{:08X}:{}", t_frame.address, t_frame.words.empty() ? " (no words)" : t_frame.words);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_report >= std::chrono::seconds(10)) {
    g_last_report = now;
    Report();
  }
}

void CensusSegmentBefore(const uint8_t* base) {
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = Device(base);
  if (!device) return;
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[t_frame.address];
  const uint32_t wptr = Load32(base, device + 48);
  if (Load32(base, device + 52) == t_frame.limit && wptr >= t_frame.wptr) Decode(s, base, t_frame.wptr + 4, wptr);
  ++s.segments;
}

void CensusSegmentAfter(const uint8_t* base) {
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = Device(base);
  if (!device) return;
  t_frame.wptr = Load32(base, device + 48);
  t_frame.limit = Load32(base, device + 52);
}

}  // namespace sa2::d3d

// The draw path with inline vertices (a localized copy, SA2_COPY_FOR_HOOK in cpu/sa2_hot_guest.cpp).
REX_EXTERN(__imp__sub_82565758);
REX_HOOK_RAW(sub_82565758) {
  if (!sa2::d3d::CensusOn()) [[likely]] {
    if (sa2::api::BeginVertices(ctx, base)) return;  // sa2_api_draws
    sa2::hot::Copy_82565758(ctx, base);
    return;
  }
  sa2::d3d::NoteDevice(ctx.r3.u32);
  sa2::d3d::CensusEnter(0x82565758u, base);
  sa2::hot::Copy_82565758(ctx, base);
  sa2::d3d::CensusLeave(base);
}
