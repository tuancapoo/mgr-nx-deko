#pragma once

#include <cstddef>
#include <cstdint>

namespace me::native {

// Physical slots rotate independently of submission order. Only return an older
// pending submission; the caller's requested fence already covers that work.
template <class Slots>
constexpr size_t OldestPendingSubmissionBefore(const Slots& slots, uint64_t order) {
  size_t oldest = slots.size();
  for (size_t i = 0; i < slots.size(); ++i)
    if (slots[i].pending && slots[i].order < order &&
        (oldest == slots.size() || slots[i].order < slots[oldest].order)) oldest = i;
  return oldest;
}

}  // namespace me::native
