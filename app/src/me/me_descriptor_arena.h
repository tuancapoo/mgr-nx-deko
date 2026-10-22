#pragma once
#include <cstddef>
#include <vector>

namespace me::native {

// Single recording thread. Rewind only after the slot's GPU fence AND command
// buffer reset: previously recorded sets must never be updated while in flight.
// A handle is used once per recording, retained across completed recordings.
template <class Handle, size_t Limit>
class DescriptorArena {
 public:
  template <class Allocate>
  Handle Acquire(Allocate allocate) {
    if (used_ == Limit) return {};
    if (used_ == handles_.size()) {
      const Handle handle = allocate();
      if (handle == Handle{}) return {};
      handles_.push_back(handle);
    }
    return handles_[used_++];
  }
  void Rewind() { used_ = 0; }
  size_t allocated() const { return handles_.size(); }
  size_t used() const { return used_; }
 private:
  std::vector<Handle> handles_;
  size_t used_ = 0;
};

}  // namespace me::native
