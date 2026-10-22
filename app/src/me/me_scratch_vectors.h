#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace me::native {

// Per-thread, allocation-free counters. "warm" means a lease started with
// retained capacity, not a count of malloc calls avoided.
struct ScratchVectorStats { uint64_t leases = 0, warm = 0; };
inline thread_local ScratchVectorStats scratch_vector_stats;

// Single-threaded, nested scratch storage. Slots never move while an outer call
// holds a reference; deeper nesting and disabled reuse use lease-local vectors.
template <class T, std::size_t Slots = 8, std::size_t MaxRetained = 2048>
class ScratchVectors {
 public:
  ScratchVectors() = default;
  ScratchVectors(const ScratchVectors&) = delete;
  ScratchVectors& operator=(const ScratchVectors&) = delete;
  ScratchVectors(ScratchVectors&&) = delete;
  ScratchVectors& operator=(ScratchVectors&&) = delete;

  class Lease {
   public:
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&&) = delete;
    Lease& operator=(Lease&&) = delete;

    ~Lease() {
      assert(pool_.depth_ == depth_ + 1);  // leases must leave in reverse acquisition order
      if (pooled_) {
        auto& values = pool_.slots_[depth_];
        if (values.capacity() > MaxRetained) std::vector<T>().swap(values);
        else values.clear();
      }
      --pool_.depth_;
    }

    std::vector<T>& get() { return pooled_ ? pool_.slots_[depth_] : local_; }

   private:
    friend class ScratchVectors;
    Lease(ScratchVectors& pool, bool reuse)
        : pool_(pool), depth_(pool.depth_++), pooled_(reuse && depth_ < Slots) {
      ++scratch_vector_stats.leases;
      if (pooled_ && pool_.slots_[depth_].capacity()) ++scratch_vector_stats.warm;
      if (pooled_) pool_.slots_[depth_].clear();
    }

    ScratchVectors& pool_;
    const std::size_t depth_;
    const bool pooled_;
    std::vector<T> local_;
  };

  [[nodiscard]] Lease Acquire(bool reuse) { return Lease(*this, reuse); }

 private:
  std::array<std::vector<T>, Slots> slots_;
  std::size_t depth_ = 0;
};

}  // namespace me::native
