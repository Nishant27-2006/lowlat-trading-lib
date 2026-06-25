// =============================================================================
// example_pool.cpp
//
// Demonstrates the thread-local fixed-block pool / object pool and contrasts its
// allocation cost with the general-purpose heap (new/delete). The pool turns an
// allocation into a pointer pop off a free list -- deterministic and fast --
// whereas new/delete go through the global allocator with all of its variance.
//
// We time a tight alloc/free loop for each strategy and print ns/op. The pool
// should be both faster and far more consistent.
// =============================================================================

#include <cstdint>
#include <cstdio>

#include "llt/memory/object_pool.hpp"
#include "llt/sys/rdtsc_clock.hpp"

namespace {

using llt::sys::RdtscClock;

// A modestly-sized payload representative of, say, an order record.
struct Order {
  std::uint64_t id;
  std::uint64_t price;
  std::uint64_t qty;
  char symbol[8];

  Order(std::uint64_t i, std::uint64_t p, std::uint64_t q) : id(i), price(p), qty(q) {
    symbol[0] = '\0';
  }
};

constexpr std::size_t kPoolSize = 4096;
constexpr std::uint64_t kIterations = 2'000'000;

// Prevent the optimizer from deleting the alloc/free pair entirely.
volatile std::uint64_t g_sink = 0;

}  // namespace

int main() {
  RdtscClock::calibrate(50);

  using Pool = llt::ObjectPool<Order, kPoolSize>;

  // -------------------------------------------------------------------------
  // Pool: construct/destroy round trips.
  // -------------------------------------------------------------------------
  {
    const std::uint64_t start = RdtscClock::now_serialized();
    for (std::uint64_t i = 0; i < kIterations; ++i) {
      Order* o = Pool::construct(i, i * 2, i * 3);
      if (o != nullptr) {
        g_sink += o->id;
        Pool::destroy(o);
      }
    }
    const std::uint64_t end = RdtscClock::now_end();
    const double ns_per_op = RdtscClock::elapsed_ns(start, end) / static_cast<double>(kIterations);
    std::printf("ObjectPool construct+destroy : %7.2f ns/op\n", ns_per_op);
  }

  // -------------------------------------------------------------------------
  // Heap: new/delete round trips.
  // -------------------------------------------------------------------------
  {
    const std::uint64_t start = RdtscClock::now_serialized();
    for (std::uint64_t i = 0; i < kIterations; ++i) {
      Order* o = new Order(i, i * 2, i * 3);
      g_sink += o->id;
      delete o;
    }
    const std::uint64_t end = RdtscClock::now_end();
    const double ns_per_op = RdtscClock::elapsed_ns(start, end) / static_cast<double>(kIterations);
    std::printf("operator new + delete        : %7.2f ns/op\n", ns_per_op);
  }

  std::printf("(sink = %llu)\n", static_cast<unsigned long long>(g_sink));
  return 0;
}
