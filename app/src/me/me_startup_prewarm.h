#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace me::native::startup {
// The UI only reads snapshots. Vulkan resources remain owned by the ring.
struct Progress {
  uint32_t visited = 0, total = 0, prepared = 0, missing = 0, other = 0, failed = 0;
  bool finished = false;
};
inline std::mutex progress_mutex;
inline Progress progress;
inline std::atomic<bool> requested{false};
inline void Request() {
  std::lock_guard lock(progress_mutex);
  progress = {};
  requested.store(true, std::memory_order_release);
}
inline void Publish(Progress value) {
  std::lock_guard lock(progress_mutex);
  progress = value;
  if (value.finished) requested.store(false, std::memory_order_release);
}
inline Progress Read() {
  std::lock_guard lock(progress_mutex);
  return progress;
}
}  // namespace me::native::startup
