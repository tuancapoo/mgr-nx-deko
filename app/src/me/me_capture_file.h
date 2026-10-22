#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace me::capture {

enum class WriteResult { kWritten, kPresent, kFailed, kConflict };

// One producer per content-addressed path. Never truncate an existing artifact.
// FAT/exFAT support rename, but not hard links. A failed/partial write remains
// .pending and is never mistaken for a complete shader by the collector.
inline WriteResult WriteOnce(const std::filesystem::path& path, const void* data, size_t size) {
  const auto same = [&](const std::filesystem::path& name) {
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != std::streamoff(size)) return false;
    file.seekg(0);
    char buffer[4096];
    const auto* bytes = static_cast<const char*>(data);
    for (size_t at = 0; at < size;) {
      const size_t count = std::min(sizeof(buffer), size - at);
      if (!file.read(buffer, std::streamsize(count)) || std::memcmp(buffer, bytes + at, count)) return false;
      at += count;
    }
    return true;
  };
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    return same(path) ? WriteResult::kPresent : WriteResult::kConflict;
  }
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return WriteResult::kFailed;
  auto pending = path;
  pending += ".pending";
  // Retry an interrupted file from an earlier run; the finalized path stays intact.
  std::FILE* file = std::fopen(pending.string().c_str(), "wb");
  if (!file) return WriteResult::kFailed;
  bool good = std::fwrite(data, 1, size, file) == size;
  if (std::fclose(file)) good = false;
  if (!good) return WriteResult::kFailed;
  // Another producer must never replace a different artifact at the same name.
  if (std::filesystem::exists(path, error)) {
    if (!same(path)) return WriteResult::kConflict;
    std::filesystem::remove(pending, error);
    return WriteResult::kPresent;
  }
  std::filesystem::rename(pending, path, error);
  return error ? WriteResult::kFailed : WriteResult::kWritten;
}

}  // namespace me::capture
