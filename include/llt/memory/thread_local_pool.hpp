#pragma once

// =============================================================================
// llt/memory/thread_local_pool.hpp
//
// FixedBlockPool<BlockSize, BlockCount> -- a fixed-size-block memory pool backed
// by a single contiguous chunk and an intrusive free list. Plus a thread_local
// accessor so each thread gets its own private pool.
//
// WHY: malloc/free (and operator new/delete) are general-purpose allocators.
// They take locks (or per-thread arenas with their own bookkeeping), may walk
// size-class structures, can fault in new pages, and have unbounded tail
// latency. On the critical trading path that jitter is unacceptable. A
// fixed-block pool turns allocate()/deallocate() into a couple of pointer
// operations with deterministic, near-constant latency and zero syscalls after
// construction.
//
// THREAD-SAFETY CONTRACT:
//   * FixedBlockPool itself is NOT thread-safe. It performs NO synchronisation.
//   * A given pool instance must be used by exactly one thread.
//   * The intended usage is ThreadLocalPool<...>::instance(), which hands each
//     thread its own pool via `thread_local` storage, so the not-shared
//     invariant holds automatically. Do NOT pass a block allocated on thread A
//     to thread B's pool to free -- that mixes pools and corrupts free lists.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <new>

namespace llt {

template <std::size_t BlockSize, std::size_t BlockCount>
class FixedBlockPool {
  static_assert(BlockCount >= 1, "BlockCount must be >= 1");
  // Every block must be able to hold the intrusive free-list pointer we thread
  // through free blocks, and must be large enough to be useful.
  static_assert(BlockSize >= sizeof(void*), "BlockSize must be >= sizeof(void*)");

 public:
  // We over-align each block to the platform max-align so any standard type
  // placed in a block is correctly aligned. EffectiveBlockSize rounds BlockSize
  // up to a multiple of that alignment so successive blocks stay aligned too.
  static constexpr std::size_t kAlign = alignof(std::max_align_t);
  static constexpr std::size_t kEffectiveBlockSize =
      ((BlockSize + kAlign - 1) / kAlign) * kAlign;

  FixedBlockPool() noexcept : free_list_(nullptr), allocated_(0) { build_free_list(); }

  FixedBlockPool(const FixedBlockPool&) = delete;
  FixedBlockPool& operator=(const FixedBlockPool&) = delete;
  FixedBlockPool(FixedBlockPool&&) = delete;
  FixedBlockPool& operator=(FixedBlockPool&&) = delete;

  // ---------------------------------------------------------------------------
  // allocate -- pop a block off the free list. Returns nullptr when exhausted.
  // O(1), no locks, no syscalls.
  // ---------------------------------------------------------------------------
  void* allocate() noexcept {
    Node* n = free_list_;
    if (n == nullptr) {
      return nullptr;  // pool exhausted
    }
    free_list_ = n->next;
    ++allocated_;
    return static_cast<void*>(n);
  }

  // ---------------------------------------------------------------------------
  // deallocate -- push a block back onto the free list. `p` MUST be a pointer
  // previously returned by allocate() on THIS pool. Passing nullptr is a no-op.
  // ---------------------------------------------------------------------------
  void deallocate(void* p) noexcept {
    if (p == nullptr) {
      return;
    }
    Node* n = static_cast<Node*>(p);
    n->next = free_list_;
    free_list_ = n;
    --allocated_;
  }

  // True if a pointer falls within this pool's backing storage. Useful for
  // assertions / debugging; not on the hot path.
  bool owns(const void* p) const noexcept {
    const std::uint8_t* b = storage_;
    const std::uint8_t* e = storage_ + sizeof(storage_);
    return p >= b && p < e;
  }

  static constexpr std::size_t block_size() noexcept { return kEffectiveBlockSize; }
  static constexpr std::size_t block_count() noexcept { return BlockCount; }
  std::size_t in_use() const noexcept { return allocated_; }
  std::size_t available() const noexcept { return BlockCount - allocated_; }

 private:
  // Intrusive free-list node. While a block is free we reuse its first bytes to
  // store the `next` pointer; while allocated, the whole block belongs to the
  // caller. This is why BlockSize >= sizeof(void*) is required.
  struct Node {
    Node* next;
  };

  void build_free_list() noexcept {
    // Link every block into the free list in order. Index 0 ends up at the tail
    // (next == nullptr) and the last block at the head; ordering is irrelevant.
    free_list_ = nullptr;
    for (std::size_t i = 0; i < BlockCount; ++i) {
      std::uint8_t* raw = storage_ + i * kEffectiveBlockSize;
      Node* n = reinterpret_cast<Node*>(raw);
      n->next = free_list_;
      free_list_ = n;
    }
  }

  // Contiguous backing storage, correctly aligned for any block content.
  alignas(kAlign) std::uint8_t storage_[kEffectiveBlockSize * BlockCount];
  Node* free_list_;
  std::size_t allocated_;
};

// -----------------------------------------------------------------------------
// ThreadLocalPool -- per-thread singleton accessor over a FixedBlockPool.
//
// Each distinct (BlockSize, BlockCount) instantiation gets one pool PER THREAD.
// The first call on a thread constructs that thread's pool; it is destroyed
// when the thread exits. Because the storage is thread_local, no two threads
// ever touch the same pool, which is exactly what makes the lock-free,
// no-synchronisation design correct.
// -----------------------------------------------------------------------------
template <std::size_t BlockSize, std::size_t BlockCount>
class ThreadLocalPool {
 public:
  using pool_type = FixedBlockPool<BlockSize, BlockCount>;

  static pool_type& instance() noexcept {
    // Block-scope thread_local: lazily constructed once per thread, with the
    // standard thread-safe-init guarantee, and freed at thread exit.
    static thread_local pool_type pool;
    return pool;
  }

  static void* allocate() noexcept { return instance().allocate(); }
  static void deallocate(void* p) noexcept { instance().deallocate(p); }

 private:
  ThreadLocalPool() = delete;
};

}  // namespace llt
