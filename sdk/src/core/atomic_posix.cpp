#include <rex/platform.h>
#include <rex/thread/atomic.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

// rexglue ports (sasrt-nx, 2026-10-12): the atomics themselves are inline in rex/thread/atomic.h on POSIX builds.
// They use the __atomic builtins with sequentially consistent ordering: the legacy __sync builtins promise a full
// barrier, so on AArch64 without LSE (the Switch's Cortex-A57) GCC ends each of them with `dmb ish` after the
// exclusive loop (GCC PR 65697): tens of cycles on every critical-section enter and leave, spin lock and
// interlocked list operation. This byte puts the barrier back at run time (the app's key decides; also read by the
// generated code's REX_GUEST_CAS, tools/guest_cas.py).
extern "C" {
unsigned char g_rex_atomic_legacy_barrier = 0;
}
