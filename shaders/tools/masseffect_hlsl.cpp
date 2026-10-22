/**
 * @file    masseffect_hlsl.cpp
 * @brief   Command line front end of the Xbox 360 shader translator (XenosRecomp) for Mass Effect.
 *
 * XenosRecomp's own main.cpp is not used. It does three things: translate to HLSL, compile to
 * DXIL/SPIR-V through the DXC library, and package the result with smol-v and zstd. Only the first is
 * needed here: DXC is run as a separate program (shaders/tools/compile_spirv_all.sh) and the packaging is our
 * own format (shaders/tools/me_pack_shaders.cpp).
 *
 * Input:  a folder of original shader containers (*.bin, big-endian, signature 0x102A11xx, the layout
 *         of the 2008 Xbox 360 XDK), as collected by the game's shader dump or ue3_shader_scan.
 * Output: one <name>.hlsl per container. The output folder must be new or empty.
 *
 * Usage
 *   masseffect_hlsl <input folder> <output folder> <shader_common.h>
 *
 * Exit status: 0 if every container was translated, 2 if some were skipped, 1 on a usage/IO error.
 */

#include "XenosRecomp/shader_recompiler.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) return data;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    data.resize(static_cast<size_t>(n));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  std::fclose(f);
  return data;
}

// The container signature is big-endian: 0x102A11xx, where bit 0 of the low byte is the vertex
// shader flag. The low byte belongs to the compiler tool, so it is not compared.
bool ValidSignature(uint32_t flags_be) {
  return (__builtin_bswap32(flags_be) & 0xFFFFFF00u) == 0x102A1100u;
}

// A misplaced physical-memory read can pair a valid shader header with another shader's
// instructions. Reject that input before XenosRecomp follows EXEC addresses without a
// buffer length, or emits HLSL from instructions outside the captured microcode.
void ValidateMicrocode(const std::vector<uint8_t>& data) {
  const auto require = [](bool valid, const std::string& message) {
    if (!valid) throw std::runtime_error(message);
  };
  const auto word = [&](size_t offset) {
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 |
           uint32_t(data[offset + 2]) << 8 | uint32_t(data[offset + 3]);
  };
  const auto within = [](uint64_t offset, uint64_t length, uint64_t limit) {
    return offset <= limit && length <= limit - offset;
  };
  require(data.size() >= 36, "truncated container header");
  const uint32_t virtual_size = word(4), physical_size = word(8), shader = word(24);
  const bool vertex = (word(0) & 1) != 0;
  require(virtual_size >= 36 && within(virtual_size, physical_size, data.size()),
          "truncated container virtual or physical data");
  require(within(shader, vertex ? 36 : 32, virtual_size), "shader header outside virtual data");
  const uint32_t physical_offset = word(shader), code_size = word(shader + 4);
  require(code_size != 0 && code_size % 12 == 0 && physical_offset % 4 == 0 &&
              within(physical_offset, code_size, physical_size),
          "invalid microcode offset or length");
  const size_t code = size_t(virtual_size) + physical_offset;
  const uint32_t instruction_count = code_size / 12;
  const uint32_t interpolators = (word(shader + 20) >> 5) & 31;
  const uint32_t elements = vertex ? word(shader + 28) : 0;
  const uint32_t element_skip = vertex ? word(shader + 24) : 0;
  const uint64_t table = uint64_t(shader) + (vertex ? 36 : 32);
  require(within(table, (uint64_t(element_skip) + elements + interpolators) * 4, virtual_size),
          "shader declarations outside virtual data");
  const uint32_t constant_table = word(16);
  require(!constant_table || within(constant_table, 32, virtual_size),
          "constant table header outside virtual data");
  const bool link_by_register = !constant_table || word(constant_table + 16) == 0;

  // The SDK and translator both find the CF boundary at the lowest EXEC address.
  // Each pair occupies 12 bytes, with two packed 48-bit CF instructions. Keep
  // scanning after EXEC_END, since later clauses may be reached by control flow.
  std::vector<bool> vertex_fetches(instruction_count, false);
  uint32_t cf_pairs = instruction_count;
  for (uint32_t pair = 0; pair < cf_pairs; ++pair) {
    const size_t offset = code + size_t(pair) * 12;
    const uint32_t a = word(offset), b = word(offset + 4), c = word(offset + 8);
    const uint64_t control_flow[2] = {uint64_t(a) | (uint64_t(b & 0xFFFF) << 32),
                                      uint64_t(b >> 16) | (uint64_t(c) << 16)};
    for (uint64_t cf : control_flow) {
      const uint32_t opcode = uint32_t(cf >> 44) & 15;
      if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
      const uint32_t address = uint32_t(cf) & 0xFFF, count = uint32_t(cf >> 12) & 7;
      const uint32_t sequence = uint32_t(cf >> 16) & 0xFFF;
      require(within(address, count, instruction_count),
              "EXEC range " + std::to_string(address) + "+" + std::to_string(count) +
                  " exceeds " + std::to_string(instruction_count) + " microcode instructions");
      if (address) cf_pairs = std::min(cf_pairs, address);
      for (uint32_t i = 0; i < count; ++i) {
        if (((sequence >> (i * 2)) & 1) && (word(code + size_t(address + i) * 12) & 31) == 0)
          vertex_fetches[address + i] = true;
      }
    }
  }
  if (vertex && !link_by_register) {
    for (uint32_t i = 0; i < elements; ++i) {
      const uint32_t address = word(size_t(table + (uint64_t(element_skip) + i) * 4)) & 0xFFF;
      require(address < instruction_count && vertex_fetches[address],
              "declared vertex FETCH " + std::to_string(address) +
                  " is outside microcode or is not a vertex FETCH in an EXEC clause");
    }
  }
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4) {
    std::printf("usage: masseffect_hlsl <input folder> <output folder> <shader_common.h>\n");
    return 1;
  }
  const std::filesystem::path input = argv[1];
  const std::filesystem::path output = argv[2];

  const std::vector<uint8_t> common = ReadAll(argv[3]);
  if (common.empty()) {
    std::printf("cannot read %s\n", argv[3]);
    return 1;
  }
  const std::string_view include(reinterpret_cast<const char*>(common.data()), common.size());

  std::error_code ec;
  if (std::filesystem::exists(output) && !std::filesystem::is_empty(output)) {
    std::fprintf(stderr, "the output folder must be empty so old results are not mixed in\n");
    return 1;
  }
  std::filesystem::create_directories(output, ec);
  if (ec) throw std::runtime_error("cannot create the output folder");

  size_t total = 0, ok = 0, skipped = 0;
  for (const auto& e : std::filesystem::directory_iterator(input)) {
    if (!e.is_regular_file() || e.path().extension() != ".bin") continue;
    const std::string name = e.path().filename().string();
    const std::vector<uint8_t> data = ReadAll(e.path());
    ++total;
    if (data.size() < 24) {
      std::printf("  %s: unreadable file or truncated header\n", name.c_str());
      ++skipped;
      continue;
    }
    uint32_t flags_be = 0;
    std::memcpy(&flags_be, data.data(), 4);
    if (!ValidSignature(flags_be)) {
      std::printf("  %-20s unknown signature 0x%08X\n", name.c_str(), __builtin_bswap32(flags_be));
      ++skipped;
      continue;
    }

    std::printf("  %s\n", name.c_str());
    std::fflush(stdout);
    ShaderRecompiler recompiler;
    try {
      ValidateMicrocode(data);
      recompiler.recompile(data.data(), include);
    } catch (const std::exception& error) {
      std::printf("  %s: translation rejected: %s\n", name.c_str(), error.what());
      ++skipped;
      continue;
    }
    if (recompiler.out.empty()) {
      std::printf("  %-20s produced nothing\n", name.c_str());
      ++skipped;
      continue;
    }

    auto target = output / e.path().filename();
    target.replace_extension(".hlsl");
    FILE* f = std::fopen(target.string().c_str(), "wb");
    if (f) {
      const bool complete = std::fwrite(recompiler.out.data(), 1, recompiler.out.size(), f) == recompiler.out.size();
      const bool closed = std::fclose(f) == 0;
      if (complete && closed) ++ok;
      else ++skipped;
    } else {
      ++skipped;
    }
  }

  std::printf("\n%zu shaders: %zu translated, %zu skipped\n", total, ok, skipped);
  return total > 0 && ok == total ? 0 : 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
