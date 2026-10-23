// Guest functions the code generator did not find, written by hand and registered at startup.
//
// Two forwarding thunks in the 48-byte gap 0x824410D8-0x82441108, between sub_824410B0 and sub_82441108 (which
// tools/pointer_fixpoint.py found: overrides.toml). Each loads the object at *r4 and calls one slot of its vtable.
// Found at run time (2026-10-23, the Switch, the Sundowner fight: mgr_006..008.log, "Call to invalid or unregistered
// function at guest address 0x824410D8"; tools/runtime_gap_resolver.py: +0x0 in the gap). 0x824410F0 is the same
// thunk for the next slot, at the next address of the gap. Words from the image:
//   824410D8  80640000 lwz r3,0(r4)     824410F0  80640000 lwz r3,0(r4)
//   824410DC  7CA42B78 mr r4,r5         824410F4  7CE63B78 mr r6,r7
//   824410E0  81630000 lwz r11,0(r3)    824410F8  81630000 lwz r11,0(r3)
//   824410E4  814B0024 lwz r10,36(r11)  824410FC  814B0028 lwz r10,40(r11)
//   824410E8  7D4903A6 mtctr r10        82441100  7D4903A6 mtctr r10
//   824410EC  4E800420 bctr             82441104  4E800420 bctr
// The proper fix is an overrides.toml entry for each and a new codegen run; these keep the generated code as it is.

#include "../generated/default/sa2_pch.h"

#include <rex/system/function_dispatcher.h>

#include "mgr_missing_functions.h"

DEFINE_REX_FUNC(sub_824410D8) {
  REX_FUNC_PROLOGUE();
  ctx.r3.u64 = REX_LOAD_D_U32(ctx.r4.u32, 0);
  ctx.r4.u64 = ctx.r5.u64;
  ctx.r11.u64 = REX_LOAD_D_U32(ctx.r3.u32, 0);
  ctx.r10.u64 = REX_LOAD_D_U32(ctx.r11.u32, 36);
  ctx.ctr.u64 = ctx.r10.u64;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

DEFINE_REX_FUNC(sub_824410F0) {
  REX_FUNC_PROLOGUE();
  ctx.r3.u64 = REX_LOAD_D_U32(ctx.r4.u32, 0);
  ctx.r6.u64 = ctx.r7.u64;
  ctx.r11.u64 = REX_LOAD_D_U32(ctx.r3.u32, 0);
  ctx.r10.u64 = REX_LOAD_D_U32(ctx.r11.u32, 40);
  ctx.ctr.u64 = ctx.r10.u64;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

namespace mgr {

void RegisterMissingFunctions(rex::runtime::FunctionDispatcher* dispatcher) {
  static const struct {
    uint32_t guest;
    PPCFunc* host;
  } kFunctions[] = {
      {0x824410D8, sub_824410D8},
      {0x824410F0, sub_824410F0},
  };
  int registered = 0;
  for (const auto& f : kFunctions) {
    if (dispatcher->GetFunction(f.guest)) {
      continue;  // a later codegen run found it: keep the generated one
    }
    if (dispatcher->SetFunction(f.guest, f.host)) {
      ++registered;
    }
  }
  REXLOG_INFO("[mgr] {} hand-written guest functions registered (mgr_missing_functions.cpp)", registered);
}

}  // namespace mgr
