#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace me::native {
// This is a lookup key, never an equality proof. Read at most 48 source bytes;
// the full uploaded block is still compared before accepting a candidate.
inline std::array<uint64_t, 7> ConstantHistorySample(const void* bytes, uint32_t size) {
  std::array<uint64_t, 7> sample{uint64_t(size)};
  if (!size) return sample;
  const auto* p = static_cast<const uint8_t*>(bytes);
  auto* out = reinterpret_cast<uint8_t*>(sample.data() + 1);
  const uint32_t n = size < 16 ? size : 16;
  std::memcpy(out, p, n);
  std::memcpy(out + 16, p + (size - n) / 2, n);
  std::memcpy(out + 32, p + size - n, n);
  return sample;
}
// Read-only lookup in the current CPU-cached upload allocation. Epoch changes
// invalidate every offset, even if the ring returns to the same buffer address.
// Hashes select a candidate only; byte equality is always checked on a hit.
class ConstantUploadHistory {
 public:
  bool Find(const void* bytes, uint32_t size, uint64_t hash, uint64_t epoch,
            const uint8_t* upload, uint64_t capacity, uint64_t& offset,
            uint64_t already_compared = UINT64_MAX) const {
    const Entry& e = entries_[hash % entries_.size()];
    if (!size || !upload || !e.valid || e.epoch != epoch || e.hash != hash ||
        e.size != size || e.offset == already_compared || e.offset > capacity || size > capacity - e.offset)
      return false;
    if (std::memcmp(bytes, upload + e.offset, size) != 0) return false;
    offset = e.offset;
    return true;
  }
  void Record(uint32_t size, uint64_t hash, uint64_t epoch, uint64_t offset) {
    entries_[hash % entries_.size()] = {hash, epoch, offset, size, true};
  }
 private:
  struct Entry {
    uint64_t hash = 0, epoch = 0, offset = 0;
    uint32_t size = 0;
    bool valid = false;
  };
  std::array<Entry, 128> entries_{};
};
}  // namespace me::native
