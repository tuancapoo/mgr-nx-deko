// Sonic Adventure 2 - localized copies of functions the renderer hooks too (sa2_hot_guest.cpp, SA2_COPY_FOR_HOOK).
// The renderer's hook (src/me/sa2_me_hooks.cpp) calls Copy_ADDR where it called __imp__sub_ADDR: the copy when its key
// (sa2_cpu_native_sub_ADDR) is on and the loaded game code matches, the recompiled original otherwise.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace sa2::hot {

void Copy_82565C60(PPCContext& ctx, uint8_t* base);  // D3DDevice_DrawVertices
void Copy_82556A60(PPCContext& ctx, uint8_t* base);  // D3D draw-time shader load (FlushState part)
void Copy_82555CC8(PPCContext& ctx, uint8_t* base);  // D3D shader constant flush (sa2_cpu_direct_flush hooks it)
void Copy_82555B70(PPCContext& ctx, uint8_t* base);  // D3D fetch constant flush (sa2_cpu_direct_flush hooks it)
void Copy_82565758(PPCContext& ctx, uint8_t* base);  // D3D draw path with inline vertices (sa2_d3d_api.cpp hooks it)

}  // namespace sa2::hot
