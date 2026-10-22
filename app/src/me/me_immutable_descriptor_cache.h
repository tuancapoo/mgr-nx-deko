#pragma once

#include <cstddef>
#include <vector>

namespace me::native {
// Contents never change during a recording: a cache hit must not update the
// descriptor set. Clear only after the work slot's fence and command-pool reset.
template <typename Key, typename Handle, size_t Capacity = 128>
class ImmutableDescriptorCache {
 public:
  Handle Find(const Key& key) const {
    for (const auto& entry : entries_) if (entry.key == key) return entry.handle;
    return Handle{};
  }
  void Remember(const Key& key, Handle handle) {
    if (handle != Handle{} && entries_.size() < Capacity && Find(key) == Handle{})
      entries_.push_back({key, handle});
  }
  void ClearAfterFence() { entries_.clear(); }
  size_t size() const { return entries_.size(); }
 private:
  struct Entry { Key key; Handle handle; };
  std::vector<Entry> entries_;
};
}  // namespace me::native
