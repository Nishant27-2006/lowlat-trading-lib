# Ultra-Low Latency Trading Infrastructure Library (`llt`)

A reusable C++17 library of low-latency primitives for the critical path of an
electronic-trading system: lock-free messaging, heap-free memory management,
cycle-accurate timing, and the systems-level controls (CPU pinning, huge pages)
that turn "usually fast" into "deterministically fast".

Everything lives under namespace `llt`. The library is **header-only**: you
consume it through a single CMake `INTERFACE` target, `llt::llt`.

---

## Why this exists

In the trading hot path, the enemy is not average latency, it is **tail
latency** and **jitter**. A median of 100 ns is worthless if your p99.9 spikes
to 50 µs because `malloc` took a lock, the kernel migrated your thread to a cold
core, or two hot counters shared a cache line and ping-ponged between cores.

This library packages the well-known techniques used to keep the hot path
quiet:

- **Lock-free SPSC queue** for moving messages between threads with no locks and
  no false sharing.
- **Thread-local fixed-block pools** so the hot path never touches the OS heap.
- **Cycle-accurate timing** (`RDTSC`) for measuring nanosecond-scale latencies
  without syscall overhead.
- **CPU affinity** to keep a thread on one warm core.
- **Huge-page helpers** to cut TLB misses.
- **A fixed-memory latency histogram** so you can report p50/p99/p99.9 honestly.

---

## Components

| Header | Component | What it does / why it matters |
| --- | --- | --- |
| `llt/common/cache.hpp` | Cache & CPU intrinsics | Cache-line size + alignment macro, `LLT_LIKELY`/`LLT_UNLIKELY`, compiler barrier, `cpu_pause()` spin hint, `prefetch()`. The building blocks for everything else. |
| `llt/concurrency/spsc_queue.hpp` | **`SpscQueue<T, N>`** (flagship) | Bounded, lock-free, wait-free single-producer/single-consumer ring. Producer and consumer indices live on **separate cache lines** to eliminate false sharing; correctness rests on acquire/release ordering, documented line-by-line. |
| `llt/memory/thread_local_pool.hpp` | `FixedBlockPool`, `ThreadLocalPool` | Fixed-size-block allocator backed by one contiguous chunk + an intrusive free list. O(1), lock-free, zero syscalls after construction. Per-thread instance removes the need for synchronization. |
| `llt/memory/object_pool.hpp` | `ObjectPool<T, N>` | Type-safe front end over the block pool: `construct<T>(args...)` / `destroy(p)` with correct object lifetime, no heap. |
| `llt/sys/cpu_affinity.hpp` | CPU pinning | Pin the current/given thread to a core (`pthread_setaffinity_np`), query core count. Linux implementation; portable stubs elsewhere. |
| `llt/sys/rdtsc_clock.hpp` | `RdtscClock` | `RDTSC`/`RDTSCP` timestamping with `LFENCE` serialization, TSC-frequency calibration, cycles→ns conversion. Falls back to `std::chrono::steady_clock` on non-x86. |
| `llt/sys/hugepages.hpp` | Huge-page helpers | `MAP_HUGETLB` allocation and `MADV_HUGEPAGE` advice to reduce TLB pressure. Linux real impl; stubs elsewhere. |
| `llt/util/latency_histogram.hpp` | `LatencyHistogram` | HDR-style, fixed-memory histogram. `record(ns)` does no allocation; `percentile(p)` reports p50/p99/p99.9. |

---

## Architecture

```
                 +---------------------------------------------+
   examples/     |  example_spsc       example_pool            |
   bench/        |  bench_spsc_queue   bench_pool              |
                 +----------------------+----------------------+
                                        | use
                 +----------------------v----------------------+
                 |               llt::llt (INTERFACE)          |
                 +----------------------+----------------------+
                                        |
   util/   ---------------------------> latency_histogram
   concurrency/ -----> spsc_queue ----\
   memory/ -> object_pool -> thread_local_pool
   sys/    -> cpu_affinity   rdtsc_clock   hugepages
   common/ -> cache  (intrinsics: alignment, pause, prefetch, barriers)
```

`common/cache.hpp` is the foundation; the SPSC queue and the histogram build on
it; the memory pools layer `object_pool` over `thread_local_pool`; the `sys/`
headers are independent system-interface helpers. Examples and benchmarks
compose them into end-to-end demonstrations.

---

## Building

Requirements: CMake ≥ 3.16 and a C++17 compiler (gcc-11+ or clang-14+). The
build fetches GoogleTest and Google Benchmark via `FetchContent`, so no extra
system packages are needed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Build knobs (default `ON` when this is the top-level project):

```bash
-DLLT_BUILD_TESTS=ON
-DLLT_BUILD_BENCHMARKS=ON
-DLLT_BUILD_EXAMPLES=ON
```

### Consuming the library

Header-only, so just add the include path. With CMake + FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(llt
  GIT_REPOSITORY https://github.com/<you>/lowlat-trading-lib.git
  GIT_TAG main)
FetchContent_MakeAvailable(llt)

target_link_libraries(my_app PRIVATE llt::llt)
```

---

## Usage examples

### Lock-free SPSC hand-off

```cpp
#include "llt/concurrency/spsc_queue.hpp"

llt::SpscQueue<int, 1024> q;   // capacity must be a power of two

// Producer thread:
if (!q.try_push(42)) { /* queue full */ }

// Consumer thread:
int value;
if (q.try_pop(value)) { /* use value */ }
```

### Heap-free object allocation

```cpp
#include "llt/memory/object_pool.hpp"

struct Order { std::uint64_t id, price, qty; };
using Pool = llt::ObjectPool<Order, 4096>;   // per-thread, 4096 objects

Order* o = Pool::construct();   // no malloc; nullptr if exhausted
// ... use o ...
Pool::destroy(o);               // returns the block to the pool
```

### Cycle-accurate timing

```cpp
#include "llt/sys/rdtsc_clock.hpp"
using llt::sys::RdtscClock;

RdtscClock::calibrate();                       // once at startup
auto t0 = RdtscClock::now_serialized();
// ... work ...
auto t1 = RdtscClock::now_end();
double ns = RdtscClock::elapsed_ns(t0, t1);
```

### Pin a thread to a core

```cpp
#include "llt/sys/cpu_affinity.hpp"

auto r = llt::sys::pin_this_thread(2);   // pin to core 2
if (!r) { /* not permitted; r.error has the errno */ }
```

---

## Kernel tuning (summary)

To approach the design-target latencies you must quiet the machine, not just the
code. The full guide is in [`docs/KERNEL_TUNING.md`](docs/KERNEL_TUNING.md). In
brief:

- **`isolcpus` + `nohz_full` + `rcu_nocbs`** on your hot cores — remove them from
  the scheduler, stop the periodic timer tick, and move RCU callbacks elsewhere.
- **IRQ affinity** — steer device interrupts away from the isolated cores.
- **CPU governor = `performance`** — pin the frequency high; no DVFS ramp-up.
- **Disable deep C-states** — avoid the wake-up latency of a sleeping core.
- **Transparent / explicit huge pages** — fewer TLB misses on big hot data.
- **NIC busy-poll / kernel-bypass** — avoid interrupt and softirq latency on the
  network path.

Each of these is explained, with the *why*, in the tuning doc.

---

## Performance

**Design targets (tuned bare metal):** on an isolated, pinned core with the
`performance` governor, deep C-states disabled, and huge pages, the SPSC queue
targets **~15 ns inter-thread hand-off latency**, and the pools deliver
**zero heap allocations on the hot path** with single-digit-nanosecond
allocate/free.

**Honesty note — read this.** The 15 ns figure is a *design target on tuned
bare-metal Linux*, not something CI measured. CI runs on **shared GitHub
runners**: virtualized, un-isolated, with a powersave-ish governor and noisy
neighbors. The benchmarks and the `example_spsc` program **do run in CI and
print real measured `ns/op` for that runner**, but those absolute numbers will
be materially higher (and noisier) than tuned hardware — typically hundreds of
nanoseconds for the inter-thread path. That is expected and is not a regression.
Treat CI numbers as a *functional smoke test and relative comparison*
(pool vs. heap, queue overhead trend), and reproduce on tuned hardware following
`docs/KERNEL_TUNING.md` to evaluate against the design target.

### Methodology

- Timing uses `RDTSC`/`RDTSCP` with `LFENCE` serialization and a calibrated
  cycles→ns ratio, so we are not paying syscall overhead inside the measured
  region.
- Latencies are aggregated in a fixed-memory HDR-style histogram and reported as
  **p50 / p99 / p99.9 / max**, never as a single mean — tail behavior is the
  point.
- The pool-vs-heap benchmark reports `ns/op` for an allocate+free round trip so
  the two strategies are compared apples-to-apples.
- For real evaluation: pin producer and consumer to **sibling-free, isolated**
  cores, warm up, and discard the first samples.

---

## Project layout

```
lowlat-trading-lib/
├── CMakeLists.txt
├── include/llt/
│   ├── common/cache.hpp
│   ├── concurrency/spsc_queue.hpp
│   ├── memory/{thread_local_pool,object_pool}.hpp
│   ├── sys/{cpu_affinity,rdtsc_clock,hugepages}.hpp
│   └── util/latency_histogram.hpp
├── examples/{example_spsc,example_pool}.cpp
├── tests/        # GoogleTest unit tests
├── bench/        # Google Benchmark microbenchmarks
├── docs/{DESIGN,KERNEL_TUNING}.md
└── .github/workflows/ci.yml
```

## License

MIT © 2026 Nishant Gadde. See [`LICENSE`](LICENSE).
