// =============================================================================
// bench_spsc_queue.cpp
//
// Microbenchmarks for SpscQueue. We measure the single-threaded enqueue+dequeue
// round-trip cost (the per-operation overhead of the ring + atomics, free of
// inter-thread coherency traffic) so the number isolates the data-structure
// cost itself. The inter-thread latency story is in examples/example_spsc.cpp.
// =============================================================================

#include "llt/concurrency/spsc_queue.hpp"

#include <cstdint>

#include <benchmark/benchmark.h>

namespace {

// Single-thread push-then-pop round trip. Reports ns/op for one push + one pop.
void BM_SpscPushPop(benchmark::State& state) {
  llt::SpscQueue<std::uint64_t, 1024> q;
  std::uint64_t v = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(q.try_push(v));
    std::uint64_t out = 0;
    benchmark::DoNotOptimize(q.try_pop(out));
    benchmark::DoNotOptimize(out);
    ++v;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop);

// Burst: fill the queue, then drain it, amortizing over the batch. This keeps
// the ring partly full so the producer/consumer indices actually move.
void BM_SpscBatchFillDrain(benchmark::State& state) {
  constexpr std::size_t kCap = 1024;
  llt::SpscQueue<std::uint64_t, kCap> q;
  const std::size_t batch = kCap - 1;
  for (auto _ : state) {
    for (std::size_t i = 0; i < batch; ++i) {
      benchmark::DoNotOptimize(q.try_push(static_cast<std::uint64_t>(i)));
    }
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < batch; ++i) {
      benchmark::DoNotOptimize(q.try_pop(out));
    }
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}
BENCHMARK(BM_SpscBatchFillDrain);

}  // namespace

BENCHMARK_MAIN();
