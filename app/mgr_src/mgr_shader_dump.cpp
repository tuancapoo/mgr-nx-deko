// Metal Gear Rising: dump of every shader container the game creates, for the native renderer's shader package
// (Stage 4, milestone 2; the SA2 port's sa2_me_hooks.cpp DumpShaderBlob with MGR's addresses).
//
//   D3DDevice_CreateVertexShader  sub_826ACDF8  (clears an 872-byte header: the VS object, whose +872/+896/+904 the
//                                                draw-time shader load sub_826CB580 reads; 396 calls at boot)
//   D3DDevice_CreatePixelShader   sub_826ACC10  (its pair: both called from the twin engine functions sub_82792170 /
//                                                sub_827920B8; 852 calls at boot)
// MGR-PORT-PLAN.md, "Mốc 1, phần 2". r3 is the container blob: [0] flags/magic, [1] header size (the microcode starts
// there), [2] microcode size (the SA2 layout; checked here before trusting it). Written once each to
// shaders/vs_<hash>.bin / ps_<hash>.bin next to the executable with --mgr_dump_shaders=true.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_set>

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
#include "me/me_capture_file.h"

REXCVAR_DEFINE_BOOL(mgr_dump_shaders, false, "MGR",
                    "Write every shader container the game creates to shaders/ next to the executable "
                    "(vs_<hash>.bin / ps_<hash>.bin)");
REXCVAR_DEFINE_STRING(mgr_shader_dump_dir, "shaders", "MGR",
                      "Shader dump directory, relative to the executable folder unless absolute");

std::filesystem::path MgrShaderDumpDirectory() {
  return rex::filesystem::GetExecutableFolder() / std::filesystem::path(REXCVAR_GET(mgr_shader_dump_dir));
}

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + rex::memory::detail::PhysicalHostOffset(address), 4);
  return __builtin_bswap32(v);
}

std::mutex g_mutex;
std::unordered_set<uint64_t> g_dumped;
uint32_t g_counts[2];  // [0] PS, [1] VS: containers written

void Dump(const uint8_t* base, uint32_t blob, bool vertex) {
  if (!REXCVAR_GET(mgr_dump_shaders) || !blob) {
    return;
  }
  const uint32_t magic = Load32(base, blob);
  const uint32_t header = Load32(base, blob + 4);
  const uint32_t ucode = Load32(base, blob + 8);
  // MGR's containers (XDK 21173) start with 0x102A1101 (vertex) / 0x102A1100 (pixel); both create functions branch on
  // bit 0 (872- or 40-byte object header). Some calls carry no container in r3 (the global filter at 0x836A49F8/FC
  // returns before the blob is read): the first run without this check wrote ~90 garbage files.
  if ((magic & 0xFFFFFF00u) != 0x102A1100u) {
    return;
  }
  // The container says what it is: the first run saw the "VS" function receive pixel containers and the reverse
  // (7 times at the title; why is not known yet), so the file is named after the container, not the hook.
  if ((magic & 1u) != (vertex ? 1u : 0u)) {
    REXLOG_INFO("[shaders] {} create got a {} container at {:08X}", vertex ? "VS" : "PS", (magic & 1u) ? "VS" : "PS",
                blob);
    vertex = (magic & 1u) != 0;
  }
  if (header < 12 || header > (1u << 20) || !ucode || ucode > (1u << 20)) {
    REXLOG_WARN("[shaders] implausible {} blob at {:08X} (magic {:08X}, header {}, microcode {}), skipped",
                vertex ? "VS" : "PS", blob, magic, header, ucode);
    return;
  }
  const size_t size = size_t(header) + ucode;
  const auto* data = rex::system::kernel_state()->memory()->TranslateVirtual(blob);
  const uint64_t hash = XXH3_64bits(data, size);
  uint32_t n;
  {
    std::lock_guard lock(g_mutex);
    if (!g_dumped.insert(hash).second) {
      return;
    }
    n = 0;
  }
  const auto folder = MgrShaderDumpDirectory();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", hash);
  const auto result = me::capture::WriteOnce(path, data, size);
  if (result == me::capture::WriteResult::kPresent) return;
  if (result != me::capture::WriteResult::kWritten) {
    REXLOG_WARN("[shaders] capture failed/conflicting artifact: {}", path.string());
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    n = ++g_counts[vertex ? 1 : 0];
  }
  if (n <= 3 || n % 100 == 0) {
    REXLOG_INFO("[shaders] {} #{}: magic {:08X}, header {}, microcode {} bytes", vertex ? "VS" : "PS", n, magic,
                header, ucode);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_826ACDF8);
REX_HOOK_RAW(sub_826ACDF8) {  // D3DDevice_CreateVertexShader (r3 = the container blob)
  Dump(base, ctx.r3.u32, true);
  __imp__sub_826ACDF8(ctx, base);
}

REX_EXTERN(__imp__sub_826ACC10);
REX_HOOK_RAW(sub_826ACC10) {  // D3DDevice_CreatePixelShader (r3 = the container blob)
  Dump(base, ctx.r3.u32, false);
  __imp__sub_826ACC10(ctx, base);
}
