// =============================================================================
// bench_pool.cpp
//
// Microbenchmarks comparing the thread-local object pool against the global heap
// (operator new/delete) for an allocate+free round trip. The pool should show
// both lower mean ns/op and far lower variance.
// =============================================================================

#include "llt/memory/object_pool.hpp"

#include <cstdint>

#include <benchmark/benchmark.h>

namespace {

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

// Pool construct + destroy round trip.
void BM_PoolConstructDestroy(benchmark::State& state) {
  using Pool = llt::ObjectPool<Order, kPoolSize>;
  std::uint64_t i = 0;
  for (auto _ : state) {
    Order* o = Pool::construct(i, i, i);
    benchmark::DoNotOptimize(o);
    Pool::destroy(o);
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolConstructDestroy);

// Heap new + delete round trip (baseline).
void BM_HeapNewDelete(benchmark::State& state) {
  std::uint64_t i = 0;
  for (auto _ : state) {
    Order* o = new Order(i, i, i);
    benchmark::DoNotOptimize(o);
    delete o;
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapNewDelete);

}  // namespace

BENCHMARK_MAIN();
