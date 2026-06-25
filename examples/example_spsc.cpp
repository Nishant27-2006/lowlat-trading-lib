// =============================================================================
// example_spsc.cpp
//
// End-to-end demonstration of the flagship SPSC queue: a producer thread pinned
// to one core hands timestamps to a consumer thread pinned to another core, and
// we measure the inter-thread hand-off latency, reporting p50 / p99 / p99.9.
//
// The producer stamps each message with an rdtsc timestamp; the consumer stamps
// arrival and records (arrival - send) into a latency histogram. This is the
// canonical "how fast can I move a message between two threads" measurement.
//
// NOTE: absolute numbers depend heavily on the machine and its tuning. On a
// shared CI runner expect hundreds of nanoseconds; on tuned, isolated bare
// metal (see docs/KERNEL_TUNING.md) the design target is ~15 ns.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "llt/concurrency/spsc_queue.hpp"
#include "llt/sys/cpu_affinity.hpp"
#include "llt/sys/rdtsc_clock.hpp"
#include "llt/util/latency_histogram.hpp"

namespace {

using llt::sys::RdtscClock;

struct Message {
  std::uint64_t send_tsc;  // timestamp taken by the producer at enqueue
  std::uint64_t seq;       // sequence number for sanity checking
};

constexpr std::size_t kQueueCapacity = 1024;   // power of two
constexpr std::uint64_t kNumMessages = 1'000'000;

}  // namespace

int main() {
  RdtscClock::calibrate(50);
  std::printf("TSC clock: uses_tsc=%d, cycles/ns=%.4f\n",
              static_cast<int>(RdtscClock::uses_tsc()), RdtscClock::cycles_per_ns_value());

  llt::SpscQueue<Message, kQueueCapacity> queue;
  llt::util::LatencyHistogram hist;

  std::atomic<bool> consumer_ready{false};

  // Consumer: pinned to core 1, drains kNumMessages and records latency.
  std::thread consumer([&] {
    const auto r = llt::sys::pin_this_thread(1);
    if (!r) {
      std::printf("[consumer] could not pin to core 1 (error %d) -- continuing unpinned\n",
                  r.error);
    }
    consumer_ready.store(true, std::memory_order_release);

    std::uint64_t received = 0;
    std::uint64_t expected_seq = 0;
    Message msg;
    while (received < kNumMessages) {
      if (queue.try_pop(msg)) {
        const std::uint64_t now = RdtscClock::now_end();
        if (msg.seq != expected_seq) {
          std::printf("[consumer] ORDER VIOLATION: got %llu expected %llu\n",
                      static_cast<unsigned long long>(msg.seq),
                      static_cast<unsigned long long>(expected_seq));
        }
        ++expected_seq;
        const double ns = RdtscClock::elapsed_ns(msg.send_tsc, now);
        // Guard against the rare negative-looking delta if threads migrated.
        if (ns >= 0.0) {
          hist.record(static_cast<std::uint64_t>(ns));
        }
        ++received;
      } else {
        llt::cpu_pause();
      }
    }
  });

  // Wait until the consumer has set its affinity before the producer starts, so
  // the first samples are not skewed by thread startup.
  while (!consumer_ready.load(std::memory_order_acquire)) {
    llt::cpu_pause();
  }

  // Producer: pinned to core 0, pushes kNumMessages timestamps.
  std::thread producer([&] {
    const auto r = llt::sys::pin_this_thread(0);
    if (!r) {
      std::printf("[producer] could not pin to core 0 (error %d) -- continuing unpinned\n",
                  r.error);
    }
    for (std::uint64_t seq = 0; seq < kNumMessages;) {
      Message m{RdtscClock::now_serialized(), seq};
      if (queue.try_push(m)) {
        ++seq;
      } else {
        llt::cpu_pause();  // queue full: spin until consumer drains
      }
    }
  });

  producer.join();
  consumer.join();

  std::printf("\nInter-thread SPSC latency over %llu messages:\n",
              static_cast<unsigned long long>(hist.count()));
  std::printf("  min   : %8llu ns\n", static_cast<unsigned long long>(hist.min()));
  std::printf("  p50   : %8llu ns\n", static_cast<unsigned long long>(hist.percentile(50.0)));
  std::printf("  p99   : %8llu ns\n", static_cast<unsigned long long>(hist.percentile(99.0)));
  std::printf("  p99.9 : %8llu ns\n", static_cast<unsigned long long>(hist.percentile(99.9)));
  std::printf("  max   : %8llu ns\n", static_cast<unsigned long long>(hist.max()));
  std::printf("  mean  : %8.1f ns\n", hist.mean());

  return 0;
}
