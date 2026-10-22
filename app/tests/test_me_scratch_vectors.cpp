// Standalone: clang++ -std=c++20 this.cpp -o test_me_scratch_vectors
#include "../src/me/me_scratch_vectors.h"

#include <array>
#include <cstdio>
#include <type_traits>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return false; \
} } while (false)

namespace {
using Pool = me::native::ScratchVectors<int>;
static_assert(!std::is_copy_constructible_v<Pool> && !std::is_move_constructible_v<Pool>);
static_assert(!std::is_copy_constructible_v<Pool::Lease> && !std::is_move_constructible_v<Pool::Lease>);

bool Nested(Pool& pool, std::array<std::vector<int>*, 12>& active, std::size_t depth) {
  auto lease = pool.Acquire(true);
  auto& values = lease.get();
  CHECK(values.empty());
  for (std::size_t i = 0; i < depth; ++i) CHECK(&values != active[i]);
  values = {int(depth), int(depth + 100)};
  active[depth] = &values;
  const auto* data = values.data();
  if (depth + 1 < active.size()) CHECK(Nested(pool, active, depth + 1));
  CHECK(values.data() == data && values.size() == 2);
  CHECK(values[0] == int(depth) && values[1] == int(depth + 100));
  if (depth == 7) {
    // An overflow lease's small allocation must not be retained in a ninth slot.
    { auto overflow = pool.Acquire(true); overflow.get().reserve(32); }
    { auto overflow = pool.Acquire(true); CHECK(overflow.get().capacity() == 0); }
  }
  return true;
}

bool EarlyReturn(Pool& pool, std::vector<int>* expected) {
  auto lease = pool.Acquire(true);
  CHECK(&lease.get() == expected && lease.get().empty());
  lease.get().push_back(42);
  return true;
}

bool ReuseAndFallback() {
  Pool pool;
  std::vector<int>* first = nullptr;
  std::size_t capacity = 0;
  {
    auto lease = pool.Acquire(true);
    first = &lease.get();
    lease.get().reserve(64);
    lease.get().push_back(1);
    capacity = lease.get().capacity();
  }
  {
    auto lease = pool.Acquire(true);
    CHECK(&lease.get() == first && lease.get().empty() && lease.get().capacity() == capacity);
  }
  {
    auto disabled = pool.Acquire(false);
    CHECK(&disabled.get() != first && disabled.get().empty());
    disabled.get().push_back(7);
    {
      auto nested = pool.Acquire(true);
      CHECK(&nested.get() != first && &nested.get() != &disabled.get());
      nested.get().push_back(9);
    }
    CHECK(disabled.get().size() == 1 && disabled.get()[0] == 7);
  }
  { auto disabled = pool.Acquire(false); CHECK(disabled.get().capacity() == 0); }
  CHECK(EarlyReturn(pool, first));
  CHECK(EarlyReturn(pool, first));  // prior return released the depth and cleared its slot
  {
    auto lease = pool.Acquire(true);
    CHECK(&lease.get() == first && lease.get().capacity() == capacity);
  }
  return true;
}

bool RetentionBounds() {
  Pool pool;
  {
    auto lease = pool.Acquire(true);
    lease.get().resize(2048, 3);
  }
  {
    auto lease = pool.Acquire(true);
    CHECK(lease.get().empty() && lease.get().capacity() <= 2048);
    lease.get().reserve(4096);
    lease.get().push_back(5);
    lease.get().clear();  // an empty but oversized allocation must also be released
  }
  {
    auto lease = pool.Acquire(true);
    CHECK(lease.get().empty() && lease.get().capacity() == 0);
    lease.get().resize(4096, 6);
  }
  { auto lease = pool.Acquire(true); CHECK(lease.get().empty() && lease.get().capacity() == 0); }
  return true;
}
}  // namespace

int main() {
  Pool pool;
  std::array<std::vector<int>*, 12> active{};
  if (!Nested(pool, active, 0) || !Nested(pool, active, 0) || !ReuseAndFallback() || !RetentionBounds()) return 1;
  std::puts("PASS scratch vectors: 12 nested leases, stable outer storage, reuse, disabled/overflow fallback, "
            "bounded retention and early returns");
}
