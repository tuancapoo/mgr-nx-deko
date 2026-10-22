#pragma once
#include <array>
#include <cstdint>

namespace me::native {
// Stack-local to ONE draw-extent estimate. Never reuse permission results after
// returning to the guest or across a draw, allocation, free or protection change.
class ReadableRangeCache {
 public:
  template <typename Query>
  bool Check(uint32_t address, uint32_t length, Query query) {
    constexpr uint32_t limit = 0x20000000u, page = 0x1000u;
    if (!length || address >= limit || length > limit - address) return false;
    for (const auto& r : ranges_)
      if (r.length && address >= r.start && address - r.start < r.length &&
          length <= r.length - (address - r.start)) return true;
    const uint32_t start = address & ~(page - 1);
    const uint32_t bytes = (address - start + length + page - 1) & ~(page - 1);
    if (!query(start, bytes)) return false;
    ranges_[next_++ % ranges_.size()] = {start, bytes};
    return true;
  }
 private:
  struct Range { uint32_t start = 0, length = 0; };
  std::array<Range, 8> ranges_{};
  uint32_t next_ = 0;
};
}  // namespace me::native
