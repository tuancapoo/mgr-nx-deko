// Standalone: clang++ -std=c++20 this.cpp -o test_me_submission_completion
#include "../src/me/me_submission_completion.h"

#include <array>
#include <cstdio>

namespace {
struct Slot { bool pending; uint64_t order; };

// A reused slot can be between two predecessors in physical array order. The
// readback publication order must follow the queue, leaving newer work alone.
constexpr bool RotatedSlotsPreservePublicationOrder() {
  std::array<Slot, 4> slots{{{true, 4}, {true, 5}, {true, 3}, {true, 6}}};
  for (uint64_t expected : {uint64_t(3), uint64_t(4)}) {
    const size_t next = me::native::OldestPendingSubmissionBefore(slots, 5);
    if (next == slots.size() || slots[next].order != expected) return false;
    slots[next].pending = false;
  }
  return me::native::OldestPendingSubmissionBefore(slots, 5) == slots.size() &&
         slots[1].pending && slots[3].pending;
}
static_assert(RotatedSlotsPreservePublicationOrder());

constexpr std::array<Slot, 4> mixed{{{false, 1}, {true, 9}, {true, 7}, {false, 0}}};
static_assert(me::native::OldestPendingSubmissionBefore(mixed, 10) == 2);
static_assert(me::native::OldestPendingSubmissionBefore(mixed, 8) == 2);
static_assert(me::native::OldestPendingSubmissionBefore(mixed, 7) == mixed.size());
static_assert(me::native::OldestPendingSubmissionBefore(mixed, 0) == mixed.size());
}  // namespace

int main() {
  std::puts("PASS: submission completion order, inactive slots, requested/newer work preserved");
}
