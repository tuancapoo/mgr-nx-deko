// Metal Gear Rising hooks for the native renderer (src/me, the SA2 port's renderer from masseffect-nx through
// banjo-nx), with MGR's addresses (MGR-PORT-PLAN.md, stage 4 milestone 1: XDK 2.0.21173, the 2012 device layout).
//
// The renderer reads the game's PM4 ring by itself; these hooks only feed it what the ring cannot say:
//
//   - Shader objects. The draw-time shader load is sub_826CB580 (r3 = device): it reads the bound VS from device+13072
//     and the PS from device+13068 (read in its listing: lwz r30,13072(r31) / lwz r29,13068(r31), the same offsets as
//     SA2's sub_82556A60), so the objects are noted there, before the original patches the VS for the declaration.
//   - Draw records, a hint for pairing (NoteDrawCall: primitive type, then r5..r7 as Direct3D received them):
//       D3DDevice_DrawIndexedVertices sub_826BCD40 (writes DRAW_INDX 0xC0032201: an index buffer; 260 calls/frame)
//       D3DDevice_DrawVertices        sub_826BC928 (writes DRAW_INDX 0xC0012201: auto index; 163 calls/frame)
//     Argument order assumed from SA2's (dev, type, start, count) / (dev, type, base, start, count): same XDK family,
//     not checked against MGR's callers yet. DrawVerticesUP has not been found in MGR (the title draws none).
//
// Direct state records wrap both MGR Draw* entry points. MGR's listings confirm the same command-buffer fields
// (+48/+52/+56) as the shared frontend. Its constant/group/fetch flushes below were matched by instructions,
// including the fetch tail's canonical register run (0x5000 = 0, 0x25000, 0), not by SA2's function addresses.
// Not here yet: the full native Direct3D layer (Clear, Resolve, scissor, inline allocator). The shader container dump is in
// mgr_shader_dump.cpp. Renderer hooks call the original; the optional Wwise leaf below
// has a differential-tested native implementation with an original-code fallback.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <chrono>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "me/me_native_system.h"
#include "renut_engine/gpu/sa2_renut_yield.h"
#include "me/me_capture_file.h"
#include "cpu/sa2_enhancements.h"
#include "sa2_capture.h"

REXCVAR_DECLARE(bool, mgr_dump_shaders);  // mgr_shader_dump.cpp
REXCVAR_DEFINE_INT32(mgr_fps_limit, 0, "MGR",
                     "Diagnostic: cap game swaps without changing guest time (0 = game default; 30 for speed testing)")
    .range(0, 240);
REXCVAR_DEFINE_BOOL(mgr_dump_memory, false, "MGR",
                    "Write diagnostic heap and physical-memory snapshots after 1200 swaps");
REXCVAR_DEFINE_BOOL(mgr_renut_diag_original_draw, false, "MGR/Diagnostics",
                    "Diagnostic only: retain original Draw CPU state updates before the reNut snapshot")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
std::filesystem::path MgrShaderDumpDirectory();  // mgr_shader_dump.cpp

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + rex::memory::detail::PhysicalHostOffset(address), 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kDeviceVertexShader = 13072;  // 0x3310
constexpr uint32_t kDevicePixelShader = 13068;   // 0x330C

#if MGR_NATIVE_RENDERER
bool MgrDirectSupported(const uint8_t* base) {
  // A different XDK layout must retain the original PM4 path. Hash the complete
  // five functions once, before any native flush can write a placeholder.
  static const bool supported =
      sa2::enh::GuestCodeMatches(base, 0x826BC928, 1048, 0xAD63BE9471EDC229ull, "MGR DrawVertices") &&
      sa2::enh::GuestCodeMatches(base, 0x826BCD40, 1144, 0xFA3C03C4FEC99269ull, "MGR DrawIndexedVertices") &&
      sa2::enh::GuestCodeMatches(base, 0x826CA450, 192, 0x5C8020AE02E4F592ull, "MGR register flush") &&
      sa2::enh::GuestCodeMatches(base, 0x826CA690, 344, 0x3A5AD604999EA5A6ull, "MGR fetch flush") &&
      sa2::enh::GuestCodeMatches(base, 0x826CA7E8, 336, 0x76573D35C23F84FEull, "MGR constant flush");
  return supported;
}

// The draw records (NoteDrawCall) read the bound shaders from the same device fields.
const bool g_device_layout_set = [] {
  me::native::SetDeviceShaderOffsets(kDeviceVertexShader, kDevicePixelShader);
  return true;
}();
#endif

// Shader containers rebuilt from the bound objects (--mgr_dump_shaders=true), for the shaders that never pass through
// CreateVertexShader / CreatePixelShader: with only the Create* dumps the title screen identified 9 % of its shader
// loads (64 % with the "missing" rebuilds), so most objects come another way (in-place objects: MGR links XGRAPHICS).
// Object layout, read in sub_826ACD00 / sub_826AC3D8 (the Create* helpers that fill the object):
//   VS: [0] type 6, [32] microcode pointer, the container's header copied at +872
//   PS: [0] type 7, [24] microcode pointer, the container's header copied at +40
// Header word [1] is the header size, [2] the microcode size. The container is the header followed by the microcode.
// The microcode may already carry Direct3D's vertex-fetch patches (the renderer ignores those fields when it
// identifies a load). Written as shaders/v_<hash>.bin / p_<hash>.bin: tools/build_shader_library.sh takes them as
// in-place shaders (vs_i* / ps_i*).
std::mutex g_object_mutex;
std::unordered_set<uint64_t> g_objects_seen;  // (object, microcode address): an address is reused after a free
std::unordered_set<uint64_t> g_objects_dumped;

void DumpShaderObject(const uint8_t* base, uint32_t object, bool vertex) {
  if (!REXCVAR_GET(mgr_dump_shaders) || object < 0x40000000 || object >= 0x7F000000) {
    return;
  }
  if ((Load32(base, object) & 0xFF) != (vertex ? 6u : 7u)) {
    return;
  }
  {
    const uint64_t key = uint64_t(object) << 32 | Load32(base, object + (vertex ? 32u : 24u));
    std::lock_guard lock(g_object_mutex);
    if (!g_objects_seen.insert(key).second) {
      return;
    }
  }
  const uint32_t header_at = object + (vertex ? 872u : 40u);
  const uint32_t magic = Load32(base, header_at);
  const uint32_t header = Load32(base, header_at + 4);
  const uint32_t ucode = Load32(base, header_at + 8);
  const uint32_t ucode_at = Load32(base, object + (vertex ? 32u : 24u));
  if ((magic & 0xFFFFFF00u) != 0x102A1100u || header < 12 || header > (1u << 16) || !ucode || ucode > (1u << 20) ||
      !ucode_at) {
    REXLOG_WARN("[shaders] {} object {:08X}: no container (magic {:08X}, header {}, microcode {} at {:08X})",
                vertex ? "VS" : "PS", object, magic, header, ucode, ucode_at);
    return;
  }
  // The E0000000 physical view starts at physical 0x1000. Masking the virtual address alone is one page short.
  auto* memory = rex::system::kernel_state()->memory();
  const uint32_t physical = memory->GetPhysicalAddress(ucode_at);
  REXLOG_INFO("[shaders] object {} {:08X}: microcode at {:08X} (physical {:08X}), {} bytes", vertex ? "VS" : "PS", object,
              ucode_at, physical, ucode);
  std::vector<uint8_t> container(size_t(header) + ucode);
  std::memcpy(container.data(), memory->TranslateVirtual(header_at), header);
  std::memcpy(container.data() + header, memory->TranslateVirtual(ucode_at), ucode);
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
  uint32_t n;
  {
    std::lock_guard lock(g_object_mutex);
    if (!g_objects_dumped.insert(hash).second) {
      return;
    }
    n = uint32_t(g_objects_dumped.size());
  }
  const auto folder = MgrShaderDumpDirectory();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{}_{:016x}.bin", vertex ? "v" : "p", hash);
  const auto result = me::capture::WriteOnce(path, container.data(), container.size());
  if (result == me::capture::WriteResult::kPresent) return;
  if (result != me::capture::WriteResult::kWritten) {
    REXLOG_WARN("[shaders] object capture failed/conflicting artifact: {}", path.string());
    return;
  }
  if (n <= 3 || n % 50 == 0) {
    REXLOG_INFO("[shaders] object #{}: {} {:08X}, header {}, microcode {} bytes", n, vertex ? "VS" : "PS", object,
                header, ucode);
  }
}

}  // namespace

// Draw-time shader load (FlushState's shader part, called from every Draw*).
REX_EXTERN(__imp__sub_826CB580);
REX_HOOK_RAW(sub_826CB580) {
  const uint32_t device = ctx.r3.u32;
  if (REXCVAR_GET(mgr_dump_shaders) && !(device >= 0x40000000 && device < 0x7F000000)) {
    // Diagnostic: a "device" outside the main heap (a recorded command buffer's context?) is skipped below.
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard lock(m);
    if (seen.size() < 16 && seen.insert(device).second)
      REXLOG_WARN("[shaders] shader load with device {:08X} outside 40000000-7F000000: skipped", device);
  }
  if (device >= 0x40000000 && device < 0x7F000000) {
    const uint32_t vs = Load32(base, device + kDeviceVertexShader), ps = Load32(base, device + kDevicePixelShader);
    DumpShaderObject(base, vs, true);
    DumpShaderObject(base, ps, false);
#if MGR_NATIVE_RENDERER
    if (!me::native::ShaderObjectsFinished(vs, ps)) {
      me::native::NoteShaderObject(base, vs, true);
      me::native::NoteShaderObject(base, ps, false);
    }
#endif
  }
  __imp__sub_826CB580(ctx, base);
}

// Diagnostic (--mgr_dump_memory=true): after 1200 presented frames, write the physical pages around the unidentified
// vertex shader loads (IM_LOAD at 098ED040 / 09946040 / 098EB040, MGR-PORT-PLAN.md) to shaders/mem_<page>.bin, to see
// whether the 64 bytes before each load are an in-place container's header.
REX_EXTERN(__imp__sub_826B3510);
REX_HOOK_RAW(sub_826B3510) {  // D3DDevice_Swap
  // Slow presentation through backpressure while leaving the guest clock at real time.
  // A fixed-step game will slow down here; a delta-time game should preserve its speed.
  const int limit = REXCVAR_GET(mgr_fps_limit);
  if (limit > 0) {
    using clock = std::chrono::steady_clock;
    static thread_local clock::time_point next{};
    if (next != clock::time_point{}) std::this_thread::sleep_until(next);
    next = clock::now() + std::chrono::nanoseconds(1000000000 / limit);
  }
  static uint32_t frames = 0;
  if (REXCVAR_GET(mgr_dump_memory) && ++frames == 1200) {
    const auto folder = MgrShaderDumpDirectory();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    auto* memory = rex::system::kernel_state()->memory();
    for (uint32_t page : {0x098EC000u, 0x098ED000u, 0x09945000u, 0x09946000u, 0x098EA000u, 0x098EB000u}) {
      const auto path = folder / fmt::format("mem_{:08X}.bin", page);
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(memory->TranslatePhysical(page)), 0x1000);
    }
    REXLOG_INFO("[shaders] wrote the pages around the unidentified VS loads");
    // The game heap (where the D3D shader objects live) and the physical memory, to find what points at the sibling
    // microcode pages (an object with microcode pointer E98ED000..., or a command buffer with an IM_LOAD of 098ED040).
#if defined(_WIN32)
    // Only the committed pages (the heap is reserved, mostly uncommitted): records of [guest address, size, bytes].
    {
      std::ofstream heap(folder / "heap_40000000.bin", std::ios::binary);
      for (uint64_t a = 0x40000000u; a < 0x80000000u;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(base + a, &info, sizeof(info))) break;
        const uint64_t start = uint64_t(static_cast<const uint8_t*>(info.BaseAddress) - base);
        const uint64_t end = std::min<uint64_t>(start + info.RegionSize, 0x80000000u);
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
          const uint32_t header[2] = {uint32_t(std::max<uint64_t>(start, a)), uint32_t(end - std::max<uint64_t>(start, a))};
          heap.write(reinterpret_cast<const char*>(header), sizeof(header));
          heap.write(reinterpret_cast<const char*>(base + header[0]), header[1]);
        }
        a = std::max<uint64_t>(end, a + 0x1000);
      }
    }
#endif
    std::ofstream(folder / "phys_00000000.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(memory->TranslatePhysical(0)), 0x20000000);
    REXLOG_INFO("[shaders] wrote heap_40000000.bin and phys_00000000.bin");
  }
  SA2_RENUT_YIELD_SHARED(sub_826B3510);
  __imp__sub_826B3510(ctx, base);
  g_sa2_game_frames.fetch_add(1, std::memory_order_relaxed);
}

REX_EXTERN(__imp__sub_826BCD40);
REX_HOOK_RAW(sub_826BCD40) {  // D3DDevice_DrawIndexedVertices
#if SA2_RENUT_RENDERER
  if (banjo::renut_gpu::Active() && REXCVAR_GET(mgr_renut_diag_original_draw)) {
    const PPCContext arguments = ctx;
    __imp__sub_826BCD40(ctx, base);
    ctx = arguments;
  }
#endif
  SA2_RENUT_YIELD_SHARED(sub_826BCD40);
#if MGR_NATIVE_RENDERER
  auto direct = MgrDirectSupported(base) ? me::native::BeginDrawDirect(base, ctx.r3.u32) : me::native::DrawDirect{};
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  __imp__sub_826BCD40(ctx, base);
  me::native::EndDrawDirect(base, direct);
#else
  __imp__sub_826BCD40(ctx, base);
#endif
}

REX_EXTERN(__imp__sub_826BC928);
REX_HOOK_RAW(sub_826BC928) {  // D3DDevice_DrawVertices
#if SA2_RENUT_RENDERER
  if (banjo::renut_gpu::Active() && REXCVAR_GET(mgr_renut_diag_original_draw)) {
    const PPCContext arguments = ctx;
    __imp__sub_826BC928(ctx, base);
    ctx = arguments;
  }
#endif
  SA2_RENUT_YIELD_SHARED(sub_826BC928);
#if MGR_NATIVE_RENDERER
  auto direct = MgrDirectSupported(base) ? me::native::BeginDrawDirect(base, ctx.r3.u32) : me::native::DrawDirect{};
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  __imp__sub_826BC928(ctx, base);
  me::native::EndDrawDirect(base, direct);
#else
  __imp__sub_826BC928(ctx, base);
#endif
}

#if MGR_NATIVE_RENDERER
// MGR XDK 21173: shader constants, 16 words per dirty bit, with 16-byte-aligned payloads.
REX_EXTERN(__imp__sub_826CA7E8);
REX_HOOK_RAW(sub_826CA7E8) {
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                me::native::FlushKind::kConstants, &__imp__sub_826CA7E8)) return;
  __imp__sub_826CA7E8(ctx, base);
}

// Register groups, one word per dirty bit. Non-draw callers retain the original implementation.
REX_EXTERN(__imp__sub_826CA450);
REX_HOOK_RAW(sub_826CA450) {
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                me::native::FlushKind::kGroups, &__imp__sub_826CA450)) return;
  __imp__sub_826CA450(ctx, base);
}

// Fetch constants at device+1152: six words per dirty bit; the original's fast tail writes two 64-bit
// 0x0002500000000000 values, canonically a three-register run with values {0, 0x25000, 0}.
REX_EXTERN(__imp__sub_826CA690);
REX_HOOK_RAW(sub_826CA690) {
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, 0x4800, ctx.r3.u32 + 1152,
                                me::native::FlushKind::kFetch, &__imp__sub_826CA690)) return;
  __imp__sub_826CA690(ctx, base);
}

#endif

// GPU wait without spinning (porting-guide.md 9.5). The engine's sub_8278E640 loops on sub_826B2FC0 (r3 = the ring
// state, r4 = an 12-byte out record: [8] = device+16968 - device+16976) until [8] is 0 or -1, calling it ~2.8 million
// times a frame on the PC (the busiest thread, 91 % of a core). After the original, when the loop is going to go round
// again, the thread sleeps briefly instead of spinning; the values the caller reads are the original's.
REXCVAR_DEFINE_BOOL(mgr_gpu_wait_sleep, false, "MGR",
                    "The engine's GPU wait (sub_8278E640 -> sub_826B2FC0) sleeps 50 us per round instead of spinning");
REXCVAR_DEFINE_INT32(mgr_gpu_wait_sleep_us, 50, "MGR", "Sleep per round of the engine's GPU wait, microseconds");

REX_EXTERN(__imp__sub_826B2FC0);
REX_HOOK_RAW(sub_826B2FC0) {
  const uint32_t record = ctx.r4.u32;
  __imp__sub_826B2FC0(ctx, base);
  if (REXCVAR_GET(mgr_gpu_wait_sleep)) {
    const uint32_t pending = Load32(base, record + 8);
    if (pending + 1u >= 2u) {
      std::this_thread::sleep_for(std::chrono::microseconds(REXCVAR_GET(mgr_gpu_wait_sleep_us)));
    }
  }
}

// Kept in this existing translation unit so native-only changes build incrementally.
#include "mgr_wwise_dsp.inc"
