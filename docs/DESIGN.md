# Design Rationale

This document explains the *why* behind each component, the interfaces, their
dependencies, and — critically — their thread-safety contracts. The overriding
design principle is **determinism on the hot path**: bounded, predictable
latency matters more than peak throughput, and tail latency matters more than the
mean.

---

## Design principles

1. **No locks on the hot path.** Mutexes invite priority inversion, syscalls,
   and unbounded tail latency. We use single-writer atomics with explicit memory
   ordering instead.
2. **No heap on the hot path.** `malloc`/`new` have unbounded worst-case latency.
   Pre-allocate everything; recycle through pools.
3. **No false sharing.** Independently-written hot data is placed on separate
   cache lines so cores do not fight over coherency.
4. **Measure honestly.** Cycle-accurate timing, percentile reporting, and a
   clear separation between *design targets* and *what CI actually measured*.
5. **Portable by construction.** Linux-specific and x86-specific code is guarded;
   everything compiles and runs (possibly degraded) on ARM / non-Linux so CI and
   developer laptops are not blocked.

---

## `common/cache.hpp`

**Purpose:** the lowest-level cross-compiler primitives.

- `cacheline_size = 64` (compile-time constant). We deliberately avoid
  `std::hardware_destructive_interference_size` because it is optional in C++17,
  frequently unimplemented, and can produce ABI-instability warnings. A fixed
  constant keeps struct layouts reproducible across translation units and
  compilers.
- `LLT_CACHELINE_ALIGNED`, `LLT_LIKELY/UNLIKELY`, `LLT_ALWAYS_INLINE`.
- `compiler_barrier()` — stops *compiler* reordering only (no instructions
  emitted). Not a hardware fence.
- `cpu_pause()` — `PAUSE` on x86 (avoids the memory-order-violation pipeline
  flush when a spin loop exits, and yields to the SMT sibling), `YIELD` on
  AArch64, compiler barrier elsewhere.
- `prefetch<Locality, Write>()` — wrapper over `__builtin_prefetch`.

**Dependencies:** none (only `<cstddef>`).
**Thread-safety:** all are pure/stateless; safe everywhere.

---

## `concurrency/spsc_queue.hpp` — flagship

**Interface:** `SpscQueue<T, Capacity>` with `try_push`, `emplace`, `try_pop`,
`empty`, `size_approx`, `capacity`.

**Why a ring + power-of-two capacity:** the index wrap becomes a single bitwise
AND (`& mask`) instead of an integer division. We store up to `Capacity − 1`
elements and reserve one slot as a sentinel so empty (`head == tail`) and full
(`tail + 1 == head`) are distinguishable **without** a shared size counter that
both threads would write (which would reintroduce a contended line).

**Memory ordering (the crux):**

- The producer owns `tail_`; the consumer owns `head_`. Each loads its own index
  `relaxed` (it is the sole writer) and loads the *other* index `acquire`.
- The producer constructs the element, then publishes the new `tail_` with
  `release`. The release store guarantees the element write is visible to the
  consumer *before* the consumer's `acquire` load can observe the advanced index.
- Symmetrically, the consumer moves the element out, destroys it, then publishes
  the freed slot via a `release` store to `head_`, so the producer cannot reuse a
  slot the consumer has not finished reading.

This acquire/release pairing is exactly what prevents the classic bug where a
reader sees an advanced index but reads stale slot memory.

**False sharing:** `head_` and `tail_` are each `alignas(64)` and padded onto
their own cache lines; the buffer starts on a fresh line. Without this, every
producer write to `tail_` would invalidate the consumer's cached `head_` and
vice versa — MESI ping-pong on every single operation. This is the single most
important performance decision in the class.

**Dependencies:** `common/cache.hpp`, `<atomic>`, `<new>`, `<type_traits>`,
`<utility>`.

**Thread-safety contract:** **exactly one** producer thread and **exactly one**
consumer thread (which may be the same thread). Two producers or two consumers is
undefined behavior — there is intentionally no internal locking. `empty()` and
`size_approx()` are exact only on the owning side; treated as approximate
otherwise. Non-trivial element destructors are run, including for elements left
in the queue at destruction.

---

## `memory/thread_local_pool.hpp`

**Interface:** `FixedBlockPool<BlockSize, BlockCount>` (`allocate`/`deallocate`/
`owns`/accounting) and `ThreadLocalPool<BlockSize, BlockCount>` (per-thread
singleton accessor).

**Why:** general allocators take locks or walk size classes and have unbounded
tail latency. A fixed-block pool turns allocation into popping a node off an
intrusive free list — O(1), no syscalls, deterministic.

**How:** one contiguous, max-aligned storage array; free blocks are linked
through their own first `sizeof(void*)` bytes (hence `BlockSize ≥ sizeof(void*)`).
`BlockSize` is rounded up to `alignof(std::max_align_t)` so successive blocks stay
aligned and any standard type fits.

**Dependencies:** `<cstddef>`, `<cstdint>`, `<new>`, `<type_traits>`.

**Thread-safety contract:** `FixedBlockPool` is **not** thread-safe and does no
synchronization — a single instance belongs to a single thread. The supported
pattern is `ThreadLocalPool::instance()`, which gives each thread its own pool
via block-scope `thread_local`, so the not-shared invariant holds automatically.
Never free on a different thread/pool than the one that allocated.

---

## `memory/object_pool.hpp`

**Interface:** `ObjectPool<T, Count>` with static `construct(args...)`,
`destroy(p)`, and accounting.

**Why:** the type-safe, ergonomic front end you use on the hot path. It manages
object lifetime (placement-new / explicit destructor) over the raw block pool,
with exception safety: if `T`'s constructor throws, the block is returned before
the exception propagates.

**Dependencies:** `memory/thread_local_pool.hpp`, `<new>`, `<utility>`.

**Thread-safety contract:** inherits the pool's contract — per-thread; construct
and destroy a given object on the same thread.

---

## `sys/cpu_affinity.hpp`

**Interface:** `pin_this_thread(core)`, `pin_thread(thread, core)`,
`hardware_concurrency()`, `current_affinity_single_core()`, returning a
non-throwing `AffinityResult { ok, error }`.

**Why:** pinning keeps a thread's caches/TLB warm and stops the scheduler from
migrating it to a cold core (a migration costs thousands of cycles). Combined
with `isolcpus`/`nohz_full` it is what makes latency deterministic.

**Platform:** real on Linux (`pthread_setaffinity_np`, `sched.h`); `_GNU_SOURCE`
is defined *before any include* so the GNU extensions and `cpu_set_t` macros are
declared. Stubs returning an error on other platforms.

**Thread-safety contract:** operates on the calling thread or a supplied thread
handle; no shared mutable state. Failures are reported, never thrown, so CI
sandboxes that forbid affinity can `GTEST_SKIP()` instead of failing.

---

## `sys/rdtsc_clock.hpp`

**Interface:** `RdtscClock::now()`, `now_serialized()`, `now_end()`,
`calibrate(ms)`, `cycles_to_ns()`, `elapsed_ns()`, `cycles_per_ns_value()`,
`uses_tsc()`.

**Why:** `clock_gettime` (even via the vDSO) is far too heavy to sit inside a
region you are measuring at tens-of-nanoseconds resolution. The invariant TSC is
a few cycles to read.

**Serialization:** `now_serialized()` brackets `RDTSC` with `LFENCE` for interval
starts; `now_end()` uses `RDTSCP` (waits for prior instructions) plus a trailing
`LFENCE` for interval ends — so neither earlier nor later instructions leak into
the measured region.

**Calibration:** sleep a known wall-clock interval, divide the TSC delta by
elapsed nanoseconds to get cycles-per-ns. The ratio lives in a function-local
static (no ODR issues in a header-only library).

**Portability:** on non-x86 everything falls back to `steady_clock`, where
"cycles" are nanoseconds and the ratio is 1.0, so all call sites still work.

**Thread-safety contract:** the timestamp reads are pure. `calibrate()` writes
the shared ratio and should be called once at startup before heavy concurrent
use; reading the ratio concurrently afterward is fine in practice (single
aligned `double` write).

---

## `sys/hugepages.hpp`

**Interface:** `allocate_hugepages(bytes, page_size)` returning an RAII
`HugePageRegion`, and `advise_hugepages(addr, bytes)`.

**Why:** huge pages (2 MiB) cover 512× more memory per TLB entry than 4 KiB
pages, cutting TLB misses (and their page-table walks) on large hot structures.

**Two strategies:** `MAP_HUGETLB` (explicit, strongest, needs reserved pages) vs.
`MADV_HUGEPAGE` (best-effort via Transparent Huge Pages). Both fail gracefully so
callers can fall back to normal allocation.

**Thread-safety contract:** `HugePageRegion` is movable, not copyable, and owns
its mapping (unmaps on destruction). Standard "don't use after move / don't share
a non-const region across threads without your own synchronization" rules apply.

---

## `util/latency_histogram.hpp`

**Interface:** `BasicLatencyHistogram<SubBucketBits>` (alias `LatencyHistogram`)
with `record(ns)`, `percentile(p)`, `min/max/mean/count`, `merge`, `clear`.

**Why:** report p50/p99/p99.9 honestly. An HDR-style log-bucketed histogram gives
roughly constant relative error across many orders of magnitude with a small,
fixed array — so `record()` does **no allocation** and is cheap enough to call
inside a measured loop.

**Dependencies:** `<array>`, `<cstdint>`, `<algorithm>`.

**Thread-safety contract:** not thread-safe; one histogram per thread, optionally
`merge()`-d at the end.

---

## Dependency graph

```
common/cache.hpp                (no internal deps)
  ├── concurrency/spsc_queue.hpp
  └── sys/rdtsc_clock.hpp
memory/thread_local_pool.hpp    (no internal deps)
  └── memory/object_pool.hpp
sys/cpu_affinity.hpp            (no internal deps)
sys/hugepages.hpp               (no internal deps)
util/latency_histogram.hpp      (no internal deps)
```

No internal dependency cycles; every header is independently includable.
