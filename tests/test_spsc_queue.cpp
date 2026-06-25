// =============================================================================
// test_spsc_queue.cpp
//
// Correctness tests for SpscQueue:
//   * single-threaded: FIFO order, empty/full behaviour, capacity accounting,
//   * non-trivial element types (destructor accounting),
//   * a bounded multi-threaded producer/consumer test that asserts every item
//     is received exactly once and in order. The test is finite and joins both
//     threads, so CI cannot hang on it.
// =============================================================================

#include "llt/concurrency/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

TEST(SpscQueue, StartsEmpty) {
  llt::SpscQueue<int, 8> q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size_approx(), 0u);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueue, FifoOrderSingleThread) {
  llt::SpscQueue<int, 8> q;  // usable capacity == 7
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(q.try_push(i));
  }
  // One slot reserved as sentinel, so the 8th push must fail (full).
  EXPECT_FALSE(q.try_push(99));

  for (int i = 0; i < 7; ++i) {
    int out = -1;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  int out = -1;
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, CapacityReportsUsableSlots) {
  llt::SpscQueue<int, 16> q;
  EXPECT_EQ(q.capacity(), 15u);
}

TEST(SpscQueue, WrapAroundReuseSlots) {
  llt::SpscQueue<int, 4> q;  // usable capacity == 3
  // Push/pop repeatedly so the indices wrap around the ring multiple times.
  int value = 0;
  for (int round = 0; round < 100; ++round) {
    EXPECT_TRUE(q.try_push(value));
    int out = -1;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, value);
    ++value;
  }
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, EmplaceConstructsInPlace) {
  llt::SpscQueue<std::pair<int, int>, 8> q;
  EXPECT_TRUE(q.emplace(3, 4));
  std::pair<int, int> out{0, 0};
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out.first, 3);
  EXPECT_EQ(out.second, 4);
}

// Element type that counts live instances so we can verify the queue runs
// destructors correctly (including for items left in the queue at destruction).
namespace {
struct Counted {
  static std::atomic<int> live;
  int v;
  explicit Counted(int x = 0) : v(x) { live.fetch_add(1, std::memory_order_relaxed); }
  Counted(const Counted& o) : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
  Counted(Counted&& o) noexcept : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
  Counted& operator=(const Counted&) = default;
  Counted& operator=(Counted&&) noexcept = default;
  ~Counted() { live.fetch_sub(1, std::memory_order_relaxed); }
};
std::atomic<int> Counted::live{0};
}  // namespace

TEST(SpscQueue, DestroysRemainingElements) {
  Counted::live.store(0);
  {
    llt::SpscQueue<Counted, 8> q;
    EXPECT_TRUE(q.try_push(Counted{1}));
    EXPECT_TRUE(q.try_push(Counted{2}));
    EXPECT_TRUE(q.emplace(3));
    // Pop one; two remain to be cleaned up by the destructor.
    Counted out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.v, 1);
  }
  // After the queue is destroyed and `out` goes out of scope, no live objects.
  EXPECT_EQ(Counted::live.load(), 0);
}

TEST(SpscQueue, MultiThreadedProducerConsumerInOrder) {
  // Bounded, deterministic: push exactly N items on one thread, pop exactly N
  // on another, assert in-order receipt. Both threads are joined => no hang.
  constexpr std::uint64_t kN = 500'000;
  llt::SpscQueue<std::uint64_t, 1024> q;

  std::atomic<bool> order_ok{true};
  std::atomic<std::uint64_t> received{0};

  std::thread consumer([&] {
    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    while (expected < kN) {
      if (q.try_pop(value)) {
        if (value != expected) {
          order_ok.store(false, std::memory_order_relaxed);
        }
        ++expected;
        received.store(expected, std::memory_order_relaxed);
      } else {
        llt::cpu_pause();
      }
    }
  });

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN;) {
      if (q.try_push(i)) {
        ++i;
      } else {
        llt::cpu_pause();
      }
    }
  });

  producer.join();
  consumer.join();

  EXPECT_TRUE(order_ok.load());
  EXPECT_EQ(received.load(), kN);
  EXPECT_TRUE(q.empty());
}
