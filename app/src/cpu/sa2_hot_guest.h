// Sonic Adventure 2 - native replacements of hot guest functions: hook dispatch with a self-check guard (from
// masseffect-nx's me_hot_guest.h; see sa2_hot_common.h).
//
// Hot-path cost matters (memcpy is called from ~2,000 places): the Hook object is constant-initialised (no
// function-local static guard: an acquire load per call, measured at ~1.3 % of Mass Effect's main thread on one hook),
// the only per-call shared state is one byte load, and the call counter that decides which calls are checked is
// thread-local.
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include <rex/hook.h>

#include "cpu/sa2_enhancements.h"
#include "cpu/sa2_hot_common.h"

namespace sa2::hot {

enum : uint8_t { kUninit = 0, kOff = 1, kOn = 2 };

struct Hook {
  const char* name;
  PPCFunc* orig;
  bool (*native)(PPCContext&, uint8_t*);  // false = declined: nothing was modified, the original must run
  void (*writes)(const PPCContext&, const uint8_t*, Writes&);
  Cmp cmp;
  sa2::enh::Enhancement* key;
  uint32_t code_address;  // the guest code the native was written from (tools/guest_code_hash.py)
  uint32_t code_bytes;
  uint64_t code_hash;
  // Runtime state. Until the first call initialises it the zeros are harmless: kUninit goes to the slow path, and a
  // stale guard_calls/period_mask of 0 on another core only means "check this call".
  std::atomic<uint8_t> state{kUninit};
  uint32_t guard_calls = 0;         // the first N calls of each thread are always checked ...
  uint32_t period_mask = 0;         // ... afterwards every (mask+1)-th call
  std::atomic<bool> failed{false};  // a check found a difference: the original runs from now on
  std::atomic<uint64_t> checks{0};
  // sa2_cpu_hook_counter_ctx: this hook's counter in every thread's context (PPCContext::host_counters), given at
  // the first call (InitHook); kNoSlot until then, kSlotsFull when the slots ran out.
  std::atomic<uint8_t> slot{0xFF};
};
// PPCContext::host_counters: the hooks' slots (first called, first served); the last four belong to the native
// Direct3D layer (kHostCounterTick, me/sa2_api_internal.h).
inline constexpr uint8_t kCtxSlots = 60;
inline constexpr uint8_t kNoSlot = 0xFF, kSlotsFull = 0xFE;

extern ::sa2::enh::Enhancement g_counter_shared;  // sa2_cpu_hook_counter_shared (sa2_hot_guest.cpp)
extern ::sa2::enh::Enhancement g_counter_ctx;     // sa2_cpu_hook_counter_ctx (sa2_hot_guest.cpp)

// A slot of PPCContext::host_counters for one hook (kSlotsFull when none is left). Two threads at a hook's first call
// each take a number and one of them stays unused. Defined in sa2_hot_guest.cpp.
void TakeCtxSlot(std::atomic<uint8_t>& slot);

// First call of a hook (any thread): key, code check, guard settings. Defined in sa2_hot_guest.cpp.
void InitHook(Hook& h, uint8_t* base);

// One guarded call: native on a copy of the registers with the written ranges snapshotted, rollback, original,
// compare. Leaves the state exactly as the original would. Defined in sa2_hot_guest.cpp.
void Check(Hook& h, PPCContext& ctx, uint8_t* base);

// Use counter for the 10 s report: one shared add per 256 calls of a thread.
inline void CountUse(Hook& h, uint32_t c) {
  if ((c & 255) == 0) h.key->Use(256);
}

template <auto Native>
inline bool NativeB(PPCContext& ctx, uint8_t* base) {
  if constexpr (std::is_void_v<decltype(Native(ctx, base))>) {
    Native(ctx, base);
    return true;
  } else {
    return Native(ctx, base);
  }
}

template <auto Native>
inline void DispatchCount(Hook& h, uint32_t c, PPCContext& ctx, uint8_t* base) {
  CountUse(h, c);
  if (c < h.guard_calls || (c & h.period_mask) == 0) [[unlikely]] {
    Check(h, ctx, base);
    return;
  }
  if (!NativeB<Native>(ctx, base)) h.orig(ctx, base);
}

template <auto Native>
inline void Dispatch(Hook& h, uint32_t& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  DispatchCount<Native>(h, n++, ctx, base);
}

// sa2_cpu_hook_counter_shared: one counter per hook for all threads (a relaxed load and store, no read-modify-write:
// racing threads may count a call twice, which only moves which calls the guard samples). On the Switch a
// thread_local costs a call to __aarch64_read_tp on every hooked call (memcpy alone comes from ~2,000 call sites).
// The guard then checks the first N calls of the process instead of the first N of each thread.
template <auto Native>
inline void DispatchShared(Hook& h, std::atomic<uint32_t>& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  const uint32_t c = n.load(std::memory_order_relaxed);
  n.store(c + 1, std::memory_order_relaxed);
  if ((c & 255) == 0) [[unlikely]] g_counter_shared.Use(256);
  DispatchCount<Native>(h, c, ctx, base);
}

// sa2_cpu_hook_counter_ctx (sasrt-nx's, 2026-10-12): the call counter of each hook in the calling thread's own
// context, which the hook is handed anyway: the per-thread counter the guard was designed with (the first N calls of
// each thread checked, then every period-th), without the thread_local's call to __aarch64_read_tp and without the
// shared counter's store, whose cache line every thread calling the hook pulls to its core (memcpy and the hot copies
// are called from the game's main and render threads alike). The counter only decides which calls the guard checks.
template <auto Native>
inline void DispatchCtx(Hook& h, uint32_t& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  const uint32_t c = n++;
  if ((c & 255) == 0) [[unlikely]] g_counter_ctx.Use(256);
  DispatchCount<Native>(h, c, ctx, base);
}

// The use counter of an unguarded copy (the 10 s report only): the thread's own slot with sa2_cpu_hook_counter_ctx,
// else the lossy shared count the copies had (a relaxed load and store).
inline uint32_t CountCopyCall(uint8_t slot, std::atomic<uint32_t>& shared, PPCContext& ctx) {
  if (g_counter_ctx.active && slot < kCtxSlots) [[likely]] return ctx.host_counters[slot]++;
  const uint32_t c = shared.load(std::memory_order_relaxed);
  shared.store(c + 1, std::memory_order_relaxed);
  return c;
}


}  // namespace sa2::hot

// KEY: the enhancement key (sa2_cpu_native_*); NS: namespace of the n_*.h header; ADDR: hex address without 0x;
// BYTES / HASH: tools/guest_code_hash.py ADDR; CMP: sa2::hot::Cmp::kReturn or kAll.
#define SA2_HOT_HOOK(ADDR, KEY, DESC, NS, WRITES, CMP, BYTES, HASH)                                            \
  SA2_ENHANCEMENT(g_enh_##ADDR, KEY, sa2::enh::kCpu, true, DESC);                                          \
  REX_EXTERN(__imp__sub_##ADDR);                                                                           \
  REX_HOOK_RAW(sub_##ADDR) {                                                                               \
    static sa2::hot::Hook h{"sub_" #ADDR, &__imp__sub_##ADDR, &sa2::hot::NativeB<&NS::Native>, WRITES, CMP, \
                            &g_enh_##ADDR, 0x##ADDR##u, BYTES, HASH};                                       \
    uint8_t st = h.state.load(std::memory_order_relaxed);                                                  \
    if (st == sa2::hot::kUninit) [[unlikely]] {                                                            \
      sa2::hot::InitHook(h, base);                                                                         \
      st = h.state.load(std::memory_order_relaxed);                                                        \
    }                                                                                                      \
    if (st == sa2::hot::kOn) [[likely]] {                                                                  \
      const uint8_t slot = h.slot.load(std::memory_order_relaxed);                                         \
      if (sa2::hot::g_counter_ctx.active && slot < sa2::hot::kCtxSlots) [[likely]] {                       \
        sa2::hot::DispatchCtx<&NS::Native>(h, ctx.host_counters[slot], ctx, base);                         \
      } else if (sa2::hot::g_counter_shared.active) [[likely]] {                                           \
        static std::atomic<uint32_t> n_shared;                                                             \
        sa2::hot::DispatchShared<&NS::Native>(h, n_shared, ctx, base);                                     \
      } else {                                                                                             \
        static thread_local uint32_t n_calls;                                                              \
        sa2::hot::Dispatch<&NS::Native>(h, n_calls, ctx, base);                                            \
      }                                                                                                    \
      return;                                                                                              \
    }                                                                                                      \
    __imp__sub_##ADDR(ctx, base);                                                                          \
  }

// Unguarded: for localized copies of functions that call other functions. The guard cannot run both versions there
// (the callees' side effects would happen twice), so the copy runs on every call once its key and code check pass.
// Exact by construction (tools/localize_native.py); checked end to end on the PC (lockstep capture).
// OFF by default (experimental): a copy writes back and reloads every localized register at each call, so copies that
// make real calls were SLOWER (sasrt-nx PC A/B: game threads +13-18 % CPU per frame with 36 copies on; per function up
// to 2x the samples). Leaves and functions that only call the save/restore helpers got faster.
#define SA2_HOT_HOOK_UNGUARDED(ADDR, KEY, DESC, NS, BYTES, HASH) SA2_HOT_HOOK_COPY(ADDR, KEY, false, DESC, NS, BYTES, HASH)
// The same with the key's default given (round 3: copies with smart sync).
#define SA2_HOT_HOOK_COPY(ADDR, KEY, DEFAULT, DESC, NS, BYTES, HASH)                               \
  SA2_ENHANCEMENT(g_enh_##ADDR, KEY, sa2::enh::kCpu, DEFAULT, DESC);                            \
  REX_EXTERN(__imp__sub_##ADDR);                                                                \
  REX_HOOK_RAW(sub_##ADDR) {                                                                    \
    static std::atomic<uint8_t> state{sa2::hot::kUninit};                                       \
    static std::atomic<uint32_t> n_calls; /* the use report only: a lossy relaxed count, no TLS */ \
    static std::atomic<uint8_t> slot{sa2::hot::kNoSlot}; /* sa2_cpu_hook_counter_ctx */         \
    uint8_t st = state.load(std::memory_order_relaxed);                                         \
    if (st == sa2::hot::kUninit) [[unlikely]] {                                                 \
      const bool on = g_enh_##ADDR.active &&                                                    \
                      sa2::enh::GuestCodeMatches(base, 0x##ADDR##u, BYTES, HASH, #KEY);         \
      st = on ? sa2::hot::kOn : sa2::hot::kOff;                                                 \
      sa2::hot::TakeCtxSlot(slot);                                                              \
      state.store(st, std::memory_order_relaxed);                                               \
    }                                                                                           \
    if (st == sa2::hot::kOn) [[likely]] {                                                       \
      const uint32_t c =                                                                        \
          sa2::hot::CountCopyCall(slot.load(std::memory_order_relaxed), n_calls, ctx);          \
      if ((c & 255) == 0) g_enh_##ADDR.Use(256);                                                \
      NS::Native(ctx, base);                                                                    \
      return;                                                                                   \
    }                                                                                           \
    __imp__sub_##ADDR(ctx, base);                                                               \
  }

// A localized copy of a function the renderer hooks too (src/me/sa2_me_hooks.cpp): a function has one override, so
// there is no REX_HOOK_RAW here; the renderer's hook calls sa2::hot::Copy_ADDR (cpu/sa2_hot_copies.h) where it called
// __imp__sub_ADDR. Unguarded and exact by construction like SA2_HOT_HOOK_COPY. Not in localize_native.py's natives:
// the renderer's hook reads argument registers itself, so a copy that calls this function syncs everything.
#define SA2_COPY_FOR_HOOK(ADDR, KEY, DEFAULT, DESC, NS, BYTES, HASH)                               \
  SA2_ENHANCEMENT(g_enh_##ADDR, KEY, sa2::enh::kCpu, DEFAULT, DESC);                            \
  REX_EXTERN(__imp__sub_##ADDR);                                                                \
  void sa2::hot::Copy_##ADDR(PPCContext& ctx, uint8_t* base) {                                  \
    static std::atomic<uint8_t> state{sa2::hot::kUninit};                                       \
    static std::atomic<uint32_t> n_calls; /* the use report only: a lossy relaxed count, no TLS */ \
    static std::atomic<uint8_t> slot{sa2::hot::kNoSlot}; /* sa2_cpu_hook_counter_ctx */         \
    uint8_t st = state.load(std::memory_order_relaxed);                                         \
    if (st == sa2::hot::kUninit) [[unlikely]] {                                                 \
      const bool on = g_enh_##ADDR.active &&                                                    \
                      sa2::enh::GuestCodeMatches(base, 0x##ADDR##u, BYTES, HASH, #KEY);         \
      st = on ? sa2::hot::kOn : sa2::hot::kOff;                                                 \
      sa2::hot::TakeCtxSlot(slot);                                                              \
      state.store(st, std::memory_order_relaxed);                                               \
    }                                                                                           \
    if (st == sa2::hot::kOn) [[likely]] {                                                       \
      const uint32_t c =                                                                        \
          sa2::hot::CountCopyCall(slot.load(std::memory_order_relaxed), n_calls, ctx);          \
      if ((c & 255) == 0) g_enh_##ADDR.Use(256);                                                \
      NS::Native(ctx, base);                                                                    \
      return;                                                                                   \
    }                                                                                           \
    __imp__sub_##ADDR(ctx, base);                                                               \
  }
