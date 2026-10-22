// Sonic Adventure 2 hooks for the native renderer (src/me, from masseffect-nx through banjo-nx), with SA2's addresses
// (docs/sa2-notes.md, "Named D3D functions": found by tools/d3d_match.py against Banjo's named XDK functions).
//
// The renderer reads the game's PM4 ring by itself; these hooks only feed it what the ring cannot say:
//
//   - Shader objects. NoteShaderObject reads the container copy inside a D3D shader object and its microcode pointer.
//     SA2's draw-time shader load is sub_82556A60 (r3 = device; Banjo's sub_8222BF10, the same code): it reads the
//     bound VS from device+13072 and the PS from device+13068 (2012 XDK; Banjo's 2008 XDK: +12696 / +12692), so the
//     objects are noted there, before the original patches the VS for the vertex declaration.
//   - Draw records, a hint for pairing (NoteDrawCall: primitive type, then r5..r7 as D3D received them):
//       D3DDevice_DrawVertices        sub_82565C60 (dev, type, start, count)
//       D3DDevice_DrawIndexedVertices sub_82566078 (dev, type, base, start, count)
//       D3DDevice_DrawVerticesUP      sub_82565C18 (dev, type, count, data, stride)
//   - Shader containers for the library (sa2_dump_shaders = true): the blob D3DDevice_CreateVertexShader (sub_8255E750)
//     and D3DDevice_CreatePixelShader (sub_8255E568) receive in r3. It describes itself: [0] flags, [1] header size
//     (the microcode starts there), [2] microcode size. Written whole to shaders/vs_<hash>.bin / ps_<hash>.bin next to
//     the executable, once each; tools/build_shader_library.sh turns the folder into the shader package.
//
//   - The direct renderer, step 1 (sa2_cpu_direct_state): around each of the three draw functions, BeginDrawDirect and
//     EndDrawDirect turn the state packets the draw function wrote into a host record the ring applies (one NOP
//     packet covers them; the draw packet and every address Direct3D keeps stay where they were). The only guest
//     memory a hook writes: the NOP's three words over the packets' first three, which no guest code reads back.
//
// The game frame counter (the flip, sub_82557FC0) lives in src/sa2_d3d_trace.cpp. Every hook here calls the original,
// or for DrawVertices and the shader load its localized copy (src/cpu/sa2_hot_guest.cpp, SA2_COPY_FOR_HOOK: the same
// operations, exact by construction); none changes a PPC register itself.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "cpu/sa2_hot_copies.h"
#include "me/me_native_system.h"
#include "me/sa2_api.h"
#include "me/sa2_d3d_api.h"

REXCVAR_DEFINE_BOOL(sa2_dump_shaders, false, "SA2",
                    "Write every shader container the game creates to shaders/ next to the executable "
                    "(vs_<hash>.bin / ps_<hash>.bin), for tools/build_shader_library.sh");

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kDeviceVertexShader = 13072;  // 0x3310
constexpr uint32_t kDevicePixelShader = 13068;   // 0x330C

// The draw records (NoteDrawCall) read the bound shaders from the same device fields.
const bool g_device_layout_set = [] {
  me::native::SetDeviceShaderOffsets(kDeviceVertexShader, kDevicePixelShader);
  return true;
}();

std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped;

void DumpShaderBlob(const uint8_t* base, uint32_t blob_address, bool vertex) {
  if (!REXCVAR_GET(sa2_dump_shaders) || !blob_address) {
    return;
  }
  const uint32_t header = Load32(base, blob_address + 4);
  const uint32_t ucode = Load32(base, blob_address + 8);
  if (header < 12 || header > (1u << 20) || !ucode || ucode > (1u << 20)) {
    REXLOG_WARN("[shaders] implausible {} blob at {:08X} (header {}, microcode {}), skipped", vertex ? "VS" : "PS",
                blob_address, header, ucode);
    return;
  }
  const uint8_t* blob = base + blob_address;
  const size_t size = size_t(header) + ucode;
  const uint64_t hash = XXH3_64bits(blob, size);
  {
    std::lock_guard lock(g_dump_mutex);
    if (!g_dumped.insert(hash).second) {
      return;
    }
  }
  const auto folder = rex::filesystem::GetExecutableFolder() / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", hash);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(blob), std::streamsize(size));
}

}  // namespace

// Draw-time shader load (FlushState's shader part, called from every Draw*).
REX_EXTERN(__imp__sub_82556A60);
REX_HOOK_RAW(sub_82556A60) {
  const uint32_t device = ctx.r3.u32;
  if (device >= 0x40000000 && device < 0x7F000000) {
    const uint32_t vs = Load32(base, device + kDeviceVertexShader), ps = Load32(base, device + kDevicePixelShader);
    if (!me::native::ShaderObjectsFinished(vs, ps)) {
      me::native::NoteShaderObject(base, vs, true);
      me::native::NoteShaderObject(base, ps, false);
    }
  }
  sa2::hot::Copy_82556A60(ctx, base);  // the localized copy (sa2_cpu_native_sub_82556A60) or __imp__sub_82556A60
}

// Each draw hook also hands the state packets the draw function writes to the ring as a host record
// (sa2_cpu_direct_state: BeginDrawDirect before the original, EndDrawDirect right after it).
REX_EXTERN(__imp__sub_82565C60);
REX_HOOK_RAW(sub_82565C60) {  // D3DDevice_DrawVertices
  // sa2_api_draws: the native draw (no PM4 for the draw); false when it does not apply (the old path below).
  if (!sa2::d3d::CensusOn() && sa2::api::DrawVertices(ctx, base)) return;
  const bool census = sa2::d3d::CensusOn();
  if (census) [[unlikely]] {
    sa2::d3d::NoteDevice(ctx.r3.u32);
    sa2::d3d::NoteDrawFlags(base, ctx.r3.u32);
    sa2::d3d::CensusEnter(0x82565C60u, base);
  }
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32);
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  sa2::hot::Copy_82565C60(ctx, base);  // the localized copy (sa2_cpu_native_sub_82565C60) or __imp__sub_82565C60
  me::native::EndDrawDirect(base, direct);
  if (census) [[unlikely]] sa2::d3d::CensusLeave(base);
}

REX_EXTERN(__imp__sub_82566078);
REX_HOOK_RAW(sub_82566078) {  // D3DDevice_DrawIndexedVertices
  const bool census = sa2::d3d::CensusOn();
  if (census) [[unlikely]] {
    sa2::d3d::NoteDevice(ctx.r3.u32);
    sa2::d3d::NoteDrawFlags(base, ctx.r3.u32);
    sa2::d3d::CensusEnter(0x82566078u, base);
  }
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32);
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  __imp__sub_82566078(ctx, base);
  me::native::EndDrawDirect(base, direct);
  if (census) [[unlikely]] sa2::d3d::CensusLeave(base);
}

REX_EXTERN(__imp__sub_82565C18);
REX_HOOK_RAW(sub_82565C18) {  // D3DDevice_DrawVerticesUP
  if (!sa2::d3d::CensusOn() && sa2::api::DrawVerticesUP(ctx, base)) return;  // sa2_api_draws
  const bool census = sa2::d3d::CensusOn();
  if (census) [[unlikely]] {
    sa2::d3d::NoteDevice(ctx.r3.u32);
    sa2::d3d::NoteDrawFlags(base, ctx.r3.u32);
    sa2::d3d::CensusEnter(0x82565C18u, base);
  }
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32);
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  __imp__sub_82565C18(ctx, base);
  me::native::EndDrawDirect(base, direct);
  if (census) [[unlikely]] sa2::d3d::CensusLeave(base);
}

// sa2_cpu_direct_flush (the direct renderer, step 2): Direct3D's state flush functions, called by the Draw* functions'
// FlushState (DrawVertices: two constant flushes, the fetch flush and up to nine register group flushes). Inside a
// converted draw call they write the call's direct record themselves (FlushRunsDirect); otherwise they run as before
// (the localized copy, or the recompiled original). Arguments: r3 = device, r4 = dirty mask (bit 63 first), r5 = the
// first register, r6 = the shadow in the device (the fetch flush has its own: device+1152, registers 0x4800).
REX_EXTERN(__imp__sub_82555CC8);
REX_HOOK_RAW(sub_82555CC8) {  // shader constants: 16 registers a bit (r5 = 0x4000 VS / 0x4400 PS)
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                  me::native::FlushKind::kConstants, &__imp__sub_82555CC8))
    return;
  sa2::hot::Copy_82555CC8(ctx, base);
}

REX_EXTERN(__imp__sub_82555B70);
REX_HOOK_RAW(sub_82555B70) {  // fetch constants: 6 registers a bit from 0x4800, then the 0x5000 end packet
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, 0x4800, ctx.r3.u32 + 1152,
                                  me::native::FlushKind::kFetch, &__imp__sub_82555B70))
    return;
  sa2::hot::Copy_82555B70(ctx, base);
}

REX_EXTERN(__imp__sub_82555930);
REX_HOOK_RAW(sub_82555930) {  // register groups: 1 register a bit
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                  me::native::FlushKind::kGroups, &__imp__sub_82555930))
    return;
  __imp__sub_82555930(ctx, base);
}

// sa2_api_draws, verify mode: Direct3D's inline-data allocator answers at once inside a shadow run of the original.
REX_EXTERN(__imp__sub_8254D9A8);
REX_HOOK_RAW(sub_8254D9A8) {
  if (sa2::api::ShadowInlineAlloc(ctx, base)) return;
  __imp__sub_8254D9A8(ctx, base);
}

// sa2_api_clears: D3DDevice_Clear as a record.
REX_EXTERN(__imp__sub_82563AB0);
REX_HOOK_RAW(sub_82563AB0) {
  if (sa2::api::Clear(ctx, base)) return;
  __imp__sub_82563AB0(ctx, base);
}

// sa2_api_resolves: Direct3D's resolve core (its wrapper and the present path call it) as a record.
REX_EXTERN(__imp__sub_825645A0);
REX_HOOK_RAW(sub_825645A0) {
  if (sa2::api::Resolve(ctx, base)) return;
  __imp__sub_825645A0(ctx, base);
}

// sa2_api_draws: Direct3D's window offset/scissor writer (registers 0x2080-0x2082) as a record.
REX_EXTERN(__imp__sub_8254FCE8);
REX_HOOK_RAW(sub_8254FCE8) {
  if (sa2::api::WindowScissor(ctx, base)) return;
  __imp__sub_8254FCE8(ctx, base);
}

REX_EXTERN(__imp__sub_8255E750);
REX_HOOK_RAW(sub_8255E750) {  // D3DDevice_CreateVertexShader (r3 = the container blob)
  DumpShaderBlob(base, ctx.r3.u32, true);
  __imp__sub_8255E750(ctx, base);
}

REX_EXTERN(__imp__sub_8255E568);
REX_HOOK_RAW(sub_8255E568) {  // D3DDevice_CreatePixelShader (r3 = the container blob)
  DumpShaderBlob(base, ctx.r3.u32, false);
  __imp__sub_8255E568(ctx, base);
}
