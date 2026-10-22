// Temporary boot-debugging hooks: log who calls a function and with which arguments, then call the original.
// Each entry says which crash it is chasing; remove it once the cause is fixed. Empty while no crash needs one.
//
// Pattern (the setjmp/longjmp case of the fourth boot is written up in docs/sa2-notes.md):
//   extern "C" REX_FUNC(__imp__sub_XXXXXXXX);
//   REX_HOOK_RAW(sub_XXXXXXXX) {
//     REXLOG_WARN("[dbg] sub_XXXXXXXX(r3={:08X}) from lr={:08X}", ctx.r3.u32, uint32_t(ctx.lr));
//     __imp__sub_XXXXXXXX(ctx, base);
//   }

#include <rex/hook.h>
#include <rex/logging.h>
