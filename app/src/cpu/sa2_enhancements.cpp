// Sonic Adventure 2: the switchable enhancements (see sa2_enhancements.h).
#include "sa2_enhancements.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/sys_counters.h>

#ifndef SA2_BUILD_FLAVOR
#define SA2_BUILD_FLAVOR "classic codegen"
#endif

REXCVAR_DEFINE_BOOL(sa2_cpu_enhancements, true, "SA2 enhancements",
                    "Master switch of every CPU enhancement (each also has its own key)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_noncpu_enhancements, true, "SA2 enhancements",
                    "Master switch of every non-CPU enhancement (each also has its own key)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Read by the SDK's Switch profiler (sdk/src/ui/switch_perf.cpp) besides logs/rex/stacks_profile.flag: the CPU
// sampler (1 ms, every thread; logs/rex/rex_profile.log). On in the A/B and profile configs, off in the default.
REXCVAR_DEFINE_BOOL(switch_profile_stacks, false, "SA2 enhancements",
                    "Switch: CPU stack sampling every 1 ms (rex_profile.log), as logs/rex/stacks_profile.flag does")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-08: the profiler's 10-second report (logs/rex/rex_profile.log) at all. Off in the release's built-in
// settings (no logs/ folder); the test configs turn it on. The overlay's FPS does not depend on it.
REXCVAR_DEFINE_BOOL(switch_profile_report, true, "SA2 enhancements",
                    "Switch: the profiler writes its 10-second report to logs/rex/rex_profile.log (false = no file)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_enhancements_report_s, 10, "SA2 enhancements",
                     "Seconds between two '[enh] uses' log lines (0 = no periodic line)")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Diagnostic, from sasrt-nx's CPU round of 2026-10-12 (PC builds: the SDK's sys_counters.h counts nothing of this on
// the Switch): how often the guest switches the host's float flush mode (an `msr fpcr` on the A57) and reaches the
// runtime's lock-and-syscall paths, per second, with each '[enh] uses' report. It tells what is worth making cheaper
// on the console, which a PC profile cannot show. tools/census_sites.py turns the sites line into functions.
REXCVAR_DEFINE_BOOL(sa2_diag_runtime_census, false, "SA2 enhancements",
                    "Diagnostic (PC): flush-mode switches and calls of the host runtime's clock, critical section, "
                    "event, semaphore, wait and handle-lookup paths per second, in '[census]' log lines with each "
                    "'[enh] uses' report")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if !REX_PLATFORM_SWITCH && defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* name);
static uint64_t CensusImageBase() { return reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)); }
#else
[[maybe_unused]] static uint64_t CensusImageBase() { return 0; }
#endif

// sasrt-nx's CPU round (2026-10-12): build-time work with a run-time side (docs/enhancements.md).
SA2_ENHANCEMENT(g_guest_cas_no_barrier, sa2_cpu_guest_cas_no_barrier, sa2::enh::kCpu, true,
                "Compare-and-swap (the game's interlocked operations, tools/guest_cas.py, and the SDK's lock words, "
                "rex/thread/atomic.h) without the legacy builtin's trailing `dmb ish` on the A57; off puts the barrier "
                "back at run time");
#if defined(__aarch64__)
extern "C" unsigned char g_rex_atomic_legacy_barrier;  // sdk/src/core/atomic_posix.cpp
#endif

#if !REX_PLATFORM_SWITCH
// tools/flush_mode_flow.py: off the Switch every flush-mode check the control flow proved redundant keeps its test
// (REX_FLUSH_KNOWN_ON / _OFF in the generated pch), counts itself here and calls RexFlushFlowMismatch when the proof
// was wrong, before the mode is switched after all. So every PC run checks the proofs it executes; the Switch build
// has no code at these places.
extern "C" {
volatile unsigned long long g_rex_flush_flow_checked = 0;  // a plain lossy count: "at least this many"
// rex/ppc/diet_ops.h, REX_DIET_CENSUS_ON: executions of the any-mode float helpers in the game code (conversions,
// compares, arithmetic, cold paths). Plain lossy counts too.
volatile unsigned long long g_rex_diet_census[6] = {};
}
namespace {
std::atomic<uint64_t> g_flush_flow_mismatches{0};
}
extern "C" void RexFlushFlowMismatch(int on) noexcept {
  sa2::enh::VerifyDifference();
  if (g_flush_flow_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enh] DIFFERENCE: flush-mode flow: a check proven redundant had to switch the mode {} (return "
                 "address {})",
                 on ? "on" : "off", static_cast<const void*>(__builtin_return_address(0)));
  }
}
#endif

namespace sa2::enh {
namespace {

std::vector<Enhancement*>& List() {
  static std::vector<Enhancement*> list;
  return list;
}

std::atomic<uint64_t> g_verify_checks{0};
std::atomic<uint64_t> g_verify_differences{0};

// Enhancements defined elsewhere as plain cvars: the renderer's ring-thread fast paths (masseffect-nx's measured
// set, each self-checked against the old path), the renderer's GPU optimisations (docs/gpu-optimisation-candidates.md)
// and the nfsmw NVK driver paths. Per row: key, group, the value that turns it on, the value that selects the previous
// path. With the group master on, a key the configuration does not set (still at its compiled default: an old sa2.toml
// on the card lacks the newer keys) is turned on; a key the toml, the environment or the command line sets is respected.
// With the master off every row is set to its previous-path value. on == nullptr: explicit only (never turned on here).
struct External {
  const char* key;
  Group group;
  const char* on;
  const char* off;
};
constexpr External kExternal[] = {
    // CPU: the ring thread (PC lockstep capture with all of these on: every gameplay frame identical).
    {"sa2_native_flat_identity", kCpu, "true", "false"},
    {"sa2_native_fast_pair", kCpu, "true", "false"},
    {"sa2_native_raw_microcode", kCpu, "true", "false"},
    {"sa2_native_pm4_fast", kCpu, "true", "false"},
    {"sa2_native_compare_fast", kCpu, "true", "false"},
    {"sa2_native_fast_copy", kCpu, "true", "false"},
    // Round 6: 2 = the hardware CRC32 pair on the Switch (+crc); a build without it falls back to 1 by itself.
    {"sa2_native_fingerprint_mode", kCpu, "2", "0"},
    {"sa2_native_dedupe_max_fingerprint", kCpu, "16384", "0"},
    {"sa2_native_indices_cache", kCpu, "true", "false"},
    {"sa2_native_pipelines_prewarm", kCpu, "true", "false"},
    // Round 3: the upload buffer (every draw's vertices and indices) in cached memory, published with
    // vkFlushMappedMemoryRanges before each submit. The console measured 5,251 MB/s of CPU writes there against
    // 2,506 MB/s in the uncached type the SDK picks; the shared constants already use the cached type this way.
    {"sa2_native_upload_memory", kCpu, "1", "0"},
    // Round 5: deferred Vulkan recording and its two refinements, on (PC lockstep captures pixel-identical; the
    // 100+ ms PC frames it once gave were Windows' timer period, fixed). configs/sa2_test_deferred_off.toml: the A/B.
    {"sa2_native_deferred_recording", kCpu, "true", "false"},
    {"sa2_deferred_native_fast", kCpu, "true", "false"},
    {"sa2_deferred_native_update", kCpu, "true", "false"},
    // sasrt-nx's CPU round (2026-10-12), the SDK's key: no condition-variable notify (a syscall on Horizon) for an
    // event or semaphore nobody is blocked on. Read once, at the first signal (threading_posix.cpp, which only the
    // Switch build compiles: the key does not exist on the PC).
#if REX_PLATFORM_SWITCH
    {"thread_signal_skip_idle", kCpu, "true", "false"},
#endif
    // Non-CPU: the renderer's GPU optimisations.
    {"sa2_native_edram4_clear_color0", kNonCpu, "true", "false"},
    {"sa2_native_constants_dedupe", kNonCpu, "true", "false"},
    {"sa2_native_pass_narrow_end", kNonCpu, "true", "false"},
    {"sa2_native_zcull_1x_depth", kNonCpu, nullptr, "false"},  // needs a long play test (GPU loss in Unleashed)
    // Non-CPU: the nfsmw NVK driver's paths (constants by differences, per-draw emission).
    {"sa2_native_set4_differences", kNonCpu, "true", "false"},
    {"sa2_native_nvk_emission", kNonCpu, "true", "false"},
    {"sa2_native_nvk_cbufs", kNonCpu, "true", "false"},
    {"sa2_native_nvk_dynamic", kNonCpu, "true", "false"},
    {"sa2_native_nvk_preload", kNonCpu, "true", "false"},
};

bool g_cpu_master = false;
bool g_noncpu_master = false;
bool g_resolved = false;

}  // namespace

bool Register(Enhancement* e) {
  List().push_back(e);
  return true;
}

void VerifyCheck(uint64_t n) { g_verify_checks.fetch_add(n, std::memory_order_relaxed); }
void VerifyDifference() { g_verify_differences.fetch_add(1, std::memory_order_relaxed); }
// Read by name once Resolve has run (a by-name lookup is a string map search: hot paths may call this).
bool g_verify_mode = false;
bool VerifyMode() { return g_resolved ? g_verify_mode : rex::cvar::GetFlagByName("sa2_cpu_verify") == "true"; }

bool CpuMaster() { return g_cpu_master; }
bool NonCpuMaster() { return g_noncpu_master; }

void Resolve() {
  if (g_resolved) return;
  g_resolved = true;
  g_cpu_master = REXCVAR_GET(sa2_cpu_enhancements);
  g_noncpu_master = REXCVAR_GET(sa2_noncpu_enhancements);
  g_verify_mode = rex::cvar::GetFlagByName("sa2_cpu_verify") == "true";
  CodegenChecksInit();
  if (VerifyMode()) {
    // The renderer's own self-checks (old and new path compared, DIFFERENCE/MISMATCH logged) check every use.
    rex::cvar::SetFlagByName("sa2_native_verify_n", "2147483647");
    rex::cvar::SetFlagByName("sa2_native_fast_copy_verify", "2147483647");
    REXLOG_INFO("[verify] sa2_cpu_verify: every native replacement call and every renderer fast path is checked "
                "against the original path; '[verify]' lines report the checks and differences (must stay 0)");
  }
  std::string line;
  for (Enhancement* e : List()) {
    const bool own = e->own();
    const bool master = e->group == kCpu ? g_cpu_master : g_noncpu_master;
    e->active = own && master;
#if defined(__aarch64__)
    // sa2_cpu_guest_cas_no_barrier: off puts the legacy `dmb ish` back after every compare-and-swap.
    if (e == &g_guest_cas_no_barrier) g_rex_atomic_legacy_barrier = e->active ? 0 : 1;
#endif
    line += fmt::format(" {}={}", e->key, e->active ? "on" : (own ? "on-but-master-off" : "off"));
  }
  int defaulted_on = 0;
  for (const External& x : kExternal) {
    const bool master = x.group == kCpu ? g_cpu_master : g_noncpu_master;
    if (master && x.on && rex::cvar::GetFlagSource(x.key) == rex::cvar::Source::kDefault &&
        rex::cvar::GetFlagByName(x.key) != x.on) {
      rex::cvar::SetFlagByName(x.key, x.on);  // not in the configuration: on
      ++defaulted_on;
    }
    const bool own = rex::cvar::GetFlagByName(x.key) != x.off;
    if (own && !master) rex::cvar::SetFlagByName(x.key, x.off);
    line += fmt::format(" {}={}", x.key, own && master ? "on" : (own ? "on-but-master-off" : "off"));
  }
  REXLOG_INFO("[enh] sa2-nx build {} {} ({}); sa2_cpu_enhancements={} sa2_noncpu_enhancements={} ({} not in the "
              "configuration, turned on);{}",
              __DATE__, __TIME__, SA2_BUILD_FLAVOR, g_cpu_master ? "on" : "off", g_noncpu_master ? "on" : "off",
              defaulted_on, line);
  FormsSelfCheck();
}

bool GuestCodeMatches(const uint8_t* base, uint32_t address, uint32_t bytes, uint64_t fnv, const char* what) {
  uint64_t h = 0xCBF29CE484222325ull;
  const uint8_t* p = base + address;
  for (uint32_t i = 0; i < bytes; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  if (h != fnv) {
    REXLOG_WARN("[enh] {}: the game code at {:08X} is not the code it was written from (hash {:016X}, expected "
                "{:016X}); it stays off",
                what, address, h, fnv);
    return false;
  }
  return true;
}

void StartReport() {
  const int seconds = REXCVAR_GET(sa2_enhancements_report_s);
  if (seconds <= 0) return;
  // Never joined and never destroyed (libnx cannot detach a thread; see the SDK's threading_posix.cpp).
  new std::thread([seconds] {
    std::vector<uint64_t> last_uses(List().size(), 0), last_amount(List().size(), 0);
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      std::string line;
      for (size_t i = 0; i < List().size(); ++i) {
        Enhancement* e = List()[i];
        if (!e->active) continue;
        const uint64_t uses = e->uses.load(std::memory_order_relaxed);
        const uint64_t amount = e->amount.load(std::memory_order_relaxed);
        const uint64_t du = uses - last_uses[i], da = amount - last_amount[i];
        last_uses[i] = uses;
        last_amount[i] = amount;
        if (!du && !da) continue;
        line += fmt::format(" {} {}", e->key, du);
        if (e->amount_unit) line += fmt::format(" ({} {})", da, e->amount_unit);
      }
      if (!line.empty()) REXLOG_INFO("[enh] uses in the last {} s:{}", seconds, line);
      const uint64_t checks = g_verify_checks.load(std::memory_order_relaxed);
      const uint64_t differences = g_verify_differences.load(std::memory_order_relaxed);
      if (checks || differences)
        REXLOG_INFO("[verify] checks done: native replacements {} | differences (native replacements + renderer "
                    "self-checks): {}",
                    checks, differences);
#if !REX_PLATFORM_SWITCH
      // tools/flush_mode_flow.py's self-check: the proven checks this run executed, and those that were wrong.
      if (const uint64_t proven = g_rex_flush_flow_checked) {
        const uint64_t wrong = g_flush_flow_mismatches.load(std::memory_order_relaxed);
        if (wrong) {
          REXLOG_ERROR("[enh] DIFFERENCE: flush-mode flow: {} of {} checks proven redundant had to switch the mode "
                       "(cumulative)",
                       wrong, proven);
        } else {
          REXLOG_INFO("[verify] flush-mode flow: {} checks proven redundant executed so far, 0 had to switch the mode",
                      proven);
        }
      }
      CodegenChecksReport(seconds);
      if (REXCVAR_GET(sa2_diag_runtime_census)) {
        // sa2_diag_runtime_census: the SDK's counters (rex/sys_counters.h), as calls per second.
        using namespace rex::syscount;
        static uint64_t last[kCensusCount] = {};
        const auto per_s = [&](unsigned id) {
          const uint64_t now = CensusRead(id);
          const uint64_t d = now - last[id];
          last[id] = now;
          return double(d) / double(seconds);
        };
        const double clock = per_s(kClockCall), clock_blocked = per_s(kClockBlocked);
        const double enter = per_s(kCensusCritSecEnter), recursive = per_s(kCensusCritSecRecursive);
        const double try_enter = per_s(kCensusTryEnter), cs_wait = per_s(kCritSecWait);
        const double set = per_s(kCensusEventSet), sem = per_s(kCensusSemRelease), wait = per_s(kCensusWait);
        const double lookup = per_s(kObjLookup), lookup_blocked = per_s(kObjBlocked);
        const double native = per_s(kCensusNativeObject), stamp = per_s(kTimestampTick);
        const double flush = per_s(kCensusFlushWrite), flush_same = per_s(kCensusFlushSame);
        REXLOG_INFO("[census] guest code per second: {:.0f} flush-mode switches (each an msr fpcr on the console; "
                    "{:.0f} of them unconditional writes that left the mode as it was)",
                    flush, flush_same);
        {
          // What the switches saved by the generator's any-mode forms cost: a test in each of these calls.
          static uint64_t last_diet[6] = {}, last_proven = 0;
          double diet[6];
          for (int i = 0; i < 6; ++i) {
            const uint64_t now = g_rex_diet_census[i];
            diet[i] = double(now - last_diet[i]) / double(seconds);
            last_diet[i] = now;
          }
          const uint64_t proven_now = g_rex_flush_flow_checked;
          const double proven_s = double(proven_now - last_proven) / double(seconds);
          last_proven = proven_now;
          REXLOG_INFO("[census] any-mode float helpers per second: conversions {:.0f}, compares {:.0f}, arithmetic "
                      "{:.0f}, cold paths {:.0f}; proven flush checks executed {:.0f}",
                      diet[0], diet[1], diet[2], diet[3], proven_s);
          REXLOG_INFO("[census] float copy forms per second (fp_copy_pairs): loads {:.0f}, stores {:.0f}", diet[4],
                      diet[5]);
        }
        // Where the mode is switched: the most counted code addresses so far, as offsets into the executable
        // (tools/census_sites.py turns them into functions and generated source lines).
        CensusSitesEnable(true);
        static int site_reports = 0;
        if ((++site_reports % 3) == 0) {
          uint64_t addresses[48], counts[48];
          const size_t n = CensusSites(addresses, counts, 48);
          std::string list;
          for (size_t i = 0; i < n; ++i) {
            list += fmt::format(" +{:X}:{}", addresses[i] - CensusImageBase(), counts[i]);
          }
          if (n) REXLOG_INFO("[census] flush-mode switch sites (cumulative, image offset:count):{}", list);
        }
        {
          // The third survey: the runtime's own out-of-line calls for what is one load or nothing.
          const double logs = per_s(kCensusLogCall), heaps = per_s(kCensusLookupHeap);
          const double refs = per_s(kCensusRetainRelease), states = per_s(kCensusKernelState);
          const double pcr_owner = per_s(kCensusCritSecPcrOwner);
          REXLOG_INFO("[census] runtime overhead per second: {:.0f} log statements reached (written or not), {:.0f} "
                      "heap lookups for a pointer translation, {:.0f} object retains and releases, {:.0f} kernel "
                      "state reads; {:.0f} critical section owners from the PCR",
                      logs, heaps, refs, states, pcr_owner);
        }
        {
          // The kernel exports the game calls most (rex/hook.h counts every REX_EXPORT in PC builds).
          static uint64_t last_exports = 0;
          const char* names[16];
          uint64_t counts[16];
          const size_t n = ExportCensus(names, counts, 16);
          uint64_t shown = 0;
          std::string list;
          for (size_t i = 0; i < n; ++i) {
            list += fmt::format(" {}:{}", names[i] + (std::strncmp(names[i], "__imp__", 7) == 0 ? 7 : 0), counts[i]);
            shown += counts[i];
          }
          if (n) {
            REXLOG_INFO("[census] kernel exports by calls (cumulative, the 16 most called; {:.0f} a second of "
                        "them):{}",
                        double(shown - last_exports) / double(seconds), list);
          }
          last_exports = shown;
        }
        REXLOG_INFO("[census] runtime calls per second: guest clock {:.0f} ({:.0f} found its mutex taken); critical "
                    "sections entered {:.0f} ({:.0f} already owned, {:.0f} tried, {:.0f} waited); events set {:.0f}, "
                    "semaphores released {:.0f}, waits {:.0f}; handle lookups {:.0f} ({:.0f} found the global lock "
                    "taken), dispatcher objects looked up {:.0f}; timestamp ticks {:.0f}",
                    clock, clock_blocked, enter, recursive, try_enter, cs_wait, set, sem, wait, lookup, lookup_blocked,
                    native, stamp);
      }
#endif
    }
  });
}

}  // namespace sa2::enh
