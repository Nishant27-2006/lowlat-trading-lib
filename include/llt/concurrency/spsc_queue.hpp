#pragma once

// =============================================================================
// llt/concurrency/spsc_queue.hpp
//
// SpscQueue<T, Capacity> -- a bounded, lock-free, wait-free single-producer /
// single-consumer ring buffer. This is the flagship primitive of the library;
// the memory-ordering reasoning below is the most important commentary in the
// codebase, so it is documented in detail.
//
// Contract (READ THIS):
//   * Exactly ONE thread may call try_push()/emplace() (the producer).
//   * Exactly ONE thread may call try_pop()/front()/pop() (the consumer).
//   * The producer and consumer may be different threads, or the same thread.
//   * It is undefined behaviour to have two producers or two consumers. There
//     is intentionally no internal locking -- correctness for the SPSC case is
//     guaranteed purely by the atomics and their memory orders. If you need
//     MPSC/MPMC, this is the wrong class.
//
// Capacity:
//   * Must be a power of two (enforced by static_assert). This turns the
//     modulo index wrap into a single bitwise AND with `kMask`, which is a
//     handful of cycles instead of an integer division.
//   * The ring stores up to Capacity - 1 elements. One slot is intentionally
//     left empty so that the "empty" state (head == tail) is distinguishable
//     from the "full" state (tail + 1 == head) without an extra size counter
//     that both threads would have to write to (which would reintroduce false
//     sharing and a contended cache line).
// =============================================================================

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "llt/common/cache.hpp"

namespace llt {

template <typename T, std::size_t Capacity>
class SpscQueue {
  static_assert(Capacity >= 2, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

 public:
  using value_type = T;

  SpscQueue() noexcept : head_(0), tail_(0) {}

  // Non-copyable, non-movable: the queue owns raw storage and is shared by
  // reference (or pointer) between two threads. Copying/moving it across a
  // live producer/consumer would be a data race by construction.
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;

  ~SpscQueue() {
    // Drain any elements the consumer never took so their destructors run.
    if (!std::is_trivially_destructible<T>::value) {
      while (try_pop_destroy()) {
      }
    }
  }

  // Usable element capacity (one slot is reserved as the empty/full sentinel).
  static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

  // ---------------------------------------------------------------------------
  // try_push -- copy/move an item in. Returns false if the queue is full.
  // Called ONLY by the producer thread.
  // ---------------------------------------------------------------------------
  template <typename U>
  bool try_push(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
    // (1) Read our own producer index. We are the only writer of tail_, so a
    //     relaxed load is sufficient -- no other thread can change it under us.
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = (tail + 1) & kMask;

    // (2) Read the consumer's index with ACQUIRE. This synchronises-with the
    //     consumer's release store in try_pop: it guarantees that any slot the
    //     consumer has freed (by advancing head_) is genuinely free and that
    //     we observe the consumer's progress. If next == head the ring is full.
    if (LLT_UNLIKELY(next == head_.load(std::memory_order_acquire))) {
      return false;
    }

    // (3) Construct the element in place in the now-known-free slot. This write
    //     happens-before the release store in step (4).
    ::new (slot(tail)) T(std::forward<U>(value));

    // (4) Publish with RELEASE. This is the crux: the release store ensures the
    //     element construction in (3) is visible to the consumer BEFORE the
    //     consumer can observe the advanced tail_ via its acquire load. Without
    //     release/acquire pairing the consumer could read tail_, see the new
    //     index, and then read stale/garbage slot memory.
    tail_.store(next, std::memory_order_release);
    return true;
  }

  // ---------------------------------------------------------------------------
  // emplace -- construct an item in place from constructor args. Same ordering
  // discipline as try_push. Returns false if full. Producer thread only.
  // ---------------------------------------------------------------------------
  template <typename... Args>
  bool emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = (tail + 1) & kMask;
    if (LLT_UNLIKELY(next == head_.load(std::memory_order_acquire))) {
      return false;
    }
    ::new (slot(tail)) T(std::forward<Args>(args)...);
    tail_.store(next, std::memory_order_release);
    return true;
  }

  // ---------------------------------------------------------------------------
  // try_pop -- move the front item into `out`. Returns false if empty.
  // Called ONLY by the consumer thread.
  // ---------------------------------------------------------------------------
  bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable<T>::value) {
    // (1) Read our own consumer index -- relaxed, we are its sole writer.
    const std::size_t head = head_.load(std::memory_order_relaxed);

    // (2) Read the producer index with ACQUIRE. This synchronises-with the
    //     producer's release store in try_push: observing tail != head here
    //     guarantees the corresponding element construction is visible to us.
    if (LLT_UNLIKELY(head == tail_.load(std::memory_order_acquire))) {
      return false;  // empty
    }

    // (3) Move the element out and destroy the source object in the ring.
    T* p = slot(head);
    out = std::move(*p);
    p->~T();

    // (4) Publish the freed slot with RELEASE so the producer's acquire load of
    //     head_ sees that this slot is available only after we are completely
    //     done reading it. This prevents the producer from overwriting a slot
    //     we have not finished moving out of.
    head_.store((head + 1) & kMask, std::memory_order_release);
    return true;
  }

  // ---------------------------------------------------------------------------
  // empty() / size_approx()
  // ---------------------------------------------------------------------------
  // These are inherently racy when called by the "other" side -- they read both
  // indices and the queue may change immediately after. They are exact only on
  // the thread that owns the relevant index and are intended for diagnostics or
  // single-threaded use. Documented as approximate on purpose.
  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

  std::size_t size_approx() const noexcept {
    const std::size_t t = tail_.load(std::memory_order_acquire);
    const std::size_t h = head_.load(std::memory_order_acquire);
    return (t - h) & kMask;
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  // Raw, properly-aligned storage for one T. We manage construction and
  // destruction by hand (placement-new / explicit dtor) so that slots which do
  // not currently hold a live object cost nothing and so that non-default-
  // constructible / non-trivial T work correctly.
  using Storage = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

  T* slot(std::size_t i) noexcept { return reinterpret_cast<T*>(&buffer_[i]); }

  // Internal helper used only by the destructor to drain leftover elements
  // without needing a destination object.
  bool try_pop_destroy() noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    slot(head)->~T();
    head_.store((head + 1) & kMask, std::memory_order_release);
    return true;
  }

  // ---------------------------------------------------------------------------
  // FALSE SHARING AVOIDANCE -- the reason this class is fast.
  //
  // head_ is written only by the consumer; tail_ is written only by the
  // producer. If they shared a cache line, every producer write to tail_ would
  // invalidate the consumer's cached copy of head_ (and vice versa), forcing a
  // cross-core cache-coherency round trip (MESI ping-pong) on EVERY operation
  // even though the two threads touch logically independent data. That is the
  // single largest source of latency in a naive SPSC queue.
  //
  // We therefore place each index on its OWN cache line via alignas(64) and pad
  // so that nothing else lands on the same line. The buffer is likewise pushed
  // onto a fresh line. The padding bytes are never read; they exist purely to
  // keep the coherency traffic for the two indices fully independent.
  // ---------------------------------------------------------------------------
  alignas(cacheline_size) std::atomic<std::size_t> head_;  // owned by consumer
  // Pad out the remainder of the consumer's cache line. [[maybe_unused]] keeps
  // -Wunused-private-field (clang) quiet -- these bytes exist purely for layout.
  [[maybe_unused]] char pad0_[cacheline_size - sizeof(std::atomic<std::size_t>)];

  alignas(cacheline_size) std::atomic<std::size_t> tail_;  // owned by producer
  [[maybe_unused]] char pad1_[cacheline_size - sizeof(std::atomic<std::size_t>)];

  alignas(cacheline_size) Storage buffer_[Capacity];
};

}  // namespace llt
