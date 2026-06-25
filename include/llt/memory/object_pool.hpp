#pragma once

// =============================================================================
// llt/memory/object_pool.hpp
//
// ObjectPool<T, Count> -- a typed object pool layered on top of the fixed-block
// pool. construct<...>(args) returns a fully-constructed T*; destroy(p) runs the
// destructor and returns the block to the pool.
//
// This is the ergonomic, type-safe front end you actually use on the hot path:
// you get deterministic allocation latency (from FixedBlockPool) plus correct
// object lifetime management, without ever touching the OS heap.
//
// THREAD-SAFETY: same contract as the underlying pool. ObjectPool<T,N> wraps a
// thread_local FixedBlockPool sized for T, so each thread has its own pool and
// no synchronisation is needed. Construct and destroy a given object on the
// same thread.
// =============================================================================

#include <cstddef>
#include <new>
#include <utility>

#include "llt/memory/thread_local_pool.hpp"

namespace llt {

template <typename T, std::size_t Count>
class ObjectPool {
 public:
  using value_type = T;

  // Each block must hold a T *and*, while free, the intrusive free-list pointer
  // the underlying pool threads through it. So the block size is the larger of
  // sizeof(T) and sizeof(void*); the pool additionally rounds this up to
  // max_align so any T is correctly aligned.
  static constexpr std::size_t kBlockSize =
      sizeof(T) > sizeof(void*) ? sizeof(T) : sizeof(void*);

  // The backing block pool: one suitably-sized, suitably-aligned block per
  // poolable object, per thread.
  using pool_type = ThreadLocalPool<kBlockSize, Count>;

  // ---------------------------------------------------------------------------
  // construct -- allocate a block and placement-new a T into it.
  // Returns nullptr if the pool is exhausted. If the T constructor throws, the
  // block is returned to the pool and the exception propagates.
  // ---------------------------------------------------------------------------
  template <typename... Args>
  static T* construct(Args&&... args) {
    void* mem = pool_type::allocate();
    if (mem == nullptr) {
      return nullptr;  // exhausted
    }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
      return ::new (mem) T(std::forward<Args>(args)...);
    } catch (...) {
      pool_type::deallocate(mem);
      throw;
    }
#else
    return ::new (mem) T(std::forward<Args>(args)...);
#endif
  }

  // ---------------------------------------------------------------------------
  // destroy -- run T's destructor and return the block. nullptr is a no-op.
  // The pointer must have come from construct() on the SAME thread.
  // ---------------------------------------------------------------------------
  static void destroy(T* p) noexcept {
    if (p == nullptr) {
      return;
    }
    p->~T();
    pool_type::deallocate(static_cast<void*>(p));
  }

  // Diagnostics (per-thread).
  static std::size_t in_use() noexcept { return pool_type::instance().in_use(); }
  static std::size_t available() noexcept { return pool_type::instance().available(); }
  static constexpr std::size_t capacity() noexcept { return Count; }

 private:
  ObjectPool() = delete;
};

}  // namespace llt
