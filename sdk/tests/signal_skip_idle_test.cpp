// Stress test of the protocol behind thread_signal_skip_idle (sdk/src/core/threading_posix.cpp, PosixConditionBase):
// a signal whose condition has no blocked waiter skips the notify. The classes below repeat that file's Event and
// Semaphore logic line for line (state under the mutex, waiters counted under the same mutex around the wait, the
// waiter count read together with the state change, the notify after the unlock) so the protocol itself can be run
// on a PC, where threading_posix.cpp is not built.
//
// Each test is a strict handshake: a lost wake-up leaves a thread blocked for ever, which the watchdog reports.
//   clang++ -O2 -std=c++20 sdk/tests/signal_skip_idle_test.cpp -o out/tools/signal_skip_idle_test.exe
//   signal_skip_idle_test [seconds=5]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::atomic<uint64_t> g_notified{0}, g_skipped{0};

class ConditionBase {
 public:
  virtual ~ConditionBase() = default;
  bool Wait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    bool executed;
    if (signaled()) {
      executed = true;
    } else {
      ++cond_waiters_;
      executed = cond_.wait_for(lock, timeout, [this] { return signaled(); });
      --cond_waiters_;
    }
    if (executed) post_execution();
    return executed;
  }

 protected:
  virtual bool signaled() const = 0;
  virtual void post_execution() = 0;
  void WakeWaiters(uint32_t waiters) {
    if (waiters != 0) {
      cond_.notify_all();
      g_notified.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_skipped.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::condition_variable cond_;
  std::mutex mutex_;
  uint32_t cond_waiters_ = 0;
};

class Event : public ConditionBase {
 public:
  explicit Event(bool manual_reset) : manual_reset_(manual_reset) {}
  void Signal() {
    uint32_t waiters;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      signal_ = true;
      waiters = cond_waiters_;
    }
    WakeWaiters(waiters);
  }
  void Reset() {
    std::unique_lock<std::mutex> lock(mutex_);
    signal_ = false;
  }

 private:
  bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) signal_ = false;
  }
  bool signal_ = false;
  const bool manual_reset_;
};

class Semaphore : public ConditionBase {
 public:
  void Release(uint32_t n) {
    std::unique_lock<std::mutex> lock(mutex_);
    count_ += n;
    const uint32_t waiters = cond_waiters_;
    lock.unlock();
    WakeWaiters(waiters);
  }

 private:
  bool signaled() const override { return count_ > 0; }
  void post_execution() override {
    count_--;
    WakeWaiters(cond_waiters_);
  }
  uint32_t count_ = 0;
};

constexpr auto kStuck = std::chrono::milliseconds(4000);
std::atomic<bool> g_failed{false};

void Fail(const char* what) {
  if (!g_failed.exchange(true)) std::printf("FAILED: %s\n", what);
}

// Ping-pong on two auto-reset events: every Signal must wake exactly one Wait. Returns the round trips.
uint64_t PingPong(std::chrono::steady_clock::time_point end, int spin) {
  Event ping(false), pong(false);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> trips{0};
  std::thread echo([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (!ping.Wait(kStuck)) {
        if (!stop.load()) Fail("auto-reset event: a signal woke nobody (echo side)");
        return;
      }
      for (volatile int i = 0; i < spin; ++i) {}
      pong.Signal();
    }
  });
  while (std::chrono::steady_clock::now() < end && !g_failed.load()) {
    ping.Signal();
    if (!pong.Wait(kStuck)) {
      Fail("auto-reset event: a signal woke nobody (caller side)");
      break;
    }
    trips.fetch_add(1, std::memory_order_relaxed);
  }
  stop.store(true);
  ping.Signal();
  echo.join();
  return trips.load();
}

// Producers release a semaphore, consumers take it: every unit released is consumed.
uint64_t SemaphoreFlow(std::chrono::steady_clock::time_point end, int producers, int consumers) {
  Semaphore sem;
  std::atomic<uint64_t> produced{0}, consumed{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < consumers; ++i) {
    threads.emplace_back([&] {
      for (;;) {
        if (sem.Wait(std::chrono::milliseconds(50))) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else if (stop.load() && consumed.load() >= produced.load()) {
          return;
        }
      }
    });
  }
  for (int i = 0; i < producers; ++i) {
    threads.emplace_back([&, i] {
      uint32_t n = 0;
      while (std::chrono::steady_clock::now() < end) {
        produced.fetch_add(1, std::memory_order_relaxed);
        sem.Release(1);
        if ((++n & (i ? 63 : 7)) == 0) std::this_thread::yield();
        // Never far ahead of the consumers: the test is about wake-ups, not about a queue growing.
        while (produced.load(std::memory_order_relaxed) - consumed.load(std::memory_order_relaxed) > 64 &&
               std::chrono::steady_clock::now() < end + kStuck) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (int i = consumers; i < int(threads.size()); ++i) threads[size_t(i)].join();
  stop.store(true);
  const auto deadline = std::chrono::steady_clock::now() + kStuck;
  while (consumed.load() < produced.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  if (consumed.load() != produced.load()) Fail("semaphore: released units were never consumed (a lost wake-up)");
  for (int i = 0; i < consumers; ++i) threads[size_t(i)].join();
  return consumed.load();
}

// A manual-reset event set once wakes every waiter, whether it blocked before or arrives after.
uint64_t ManualBroadcast(std::chrono::steady_clock::time_point end) {
  uint64_t rounds = 0;
  while (std::chrono::steady_clock::now() < end && !g_failed.load()) {
    Event gate(true);
    std::atomic<int> through{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&, i] {
        if (i & 1) std::this_thread::yield();
        if (gate.Wait(kStuck)) through.fetch_add(1);
      });
    }
    if (rounds & 1) std::this_thread::yield();
    gate.Signal();
    for (auto& t : threads) t.join();
    if (through.load() != 4) Fail("manual-reset event: a waiter stayed blocked after the event was set");
    ++rounds;
  }
  return rounds;
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
  const auto slice = std::chrono::milliseconds(seconds * 1000 / 4);
  auto end = std::chrono::steady_clock::now() + slice;
  const uint64_t trips_tight = PingPong(end, 0);
  end = std::chrono::steady_clock::now() + slice;
  const uint64_t trips_spin = PingPong(end, 2000);
  end = std::chrono::steady_clock::now() + slice;
  const uint64_t units = SemaphoreFlow(end, 2, 3);
  end = std::chrono::steady_clock::now() + slice;
  const uint64_t rounds = ManualBroadcast(end);
  std::printf("auto-reset ping-pong: %llu + %llu round trips; semaphore: %llu units through 2 producers and 3 "
              "consumers; manual-reset broadcast: %llu rounds of 4 waiters\n",
              static_cast<unsigned long long>(trips_tight), static_cast<unsigned long long>(trips_spin),
              static_cast<unsigned long long>(units), static_cast<unsigned long long>(rounds));
  std::printf("signals: %llu notified, %llu found nobody blocked and skipped the notify\n",
              static_cast<unsigned long long>(g_notified.load()), static_cast<unsigned long long>(g_skipped.load()));
  std::printf(g_failed.load() ? "RESULT: FAILED\n" : "RESULT: ok (no lost wake-up)\n");
  return g_failed.load() ? 1 : 0;
}
