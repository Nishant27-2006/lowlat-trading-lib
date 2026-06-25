#pragma once

// =============================================================================
// llt/common/cache.hpp
//
// Low-level, cross-compiler primitives that the rest of the library builds on:
//   * the platform cache-line size and a portable alignment macro,
//   * branch-prediction hints (likely / unlikely),
//   * a compiler reordering barrier,
//   * a CPU "pause"/spin-loop relaxation hint,
//   * a prefetch wrapper.
//
// Everything here is header-only and free of allocation. These helpers are the
// difference between a spin loop that pegs a core at 100% with no useful work
// and one that lets the CPU's pipeline / SMT sibling make progress, and between
// two hot fields silently sharing a cache line and being correctly isolated.
// =============================================================================

#include <cstddef>

namespace llt {

// -----------------------------------------------------------------------------
// Cache-line size
// -----------------------------------------------------------------------------
// 64 bytes is correct for essentially every modern x86-64 part and for the
// common AArch64 cores used in cloud (Graviton, Ampere). We deliberately use a
// compile-time constant rather than std::hardware_destructive_interference_size
// because the latter is (a) C++17-optional and frequently unimplemented, and
// (b) on some toolchains warns about ABI instability. A fixed 64 keeps layouts
// reproducible across translation units and compilers.
inline constexpr std::size_t cacheline_size = 64;

}  // namespace llt

// -----------------------------------------------------------------------------
// Alignment macro
// -----------------------------------------------------------------------------
// Use as:  struct LLT_CACHELINE_ALIGNED Foo { ... };
// or:      alignas(::llt::cacheline_size) std::atomic<size_t> idx;
// The macro form is convenient where `alignas` placement is awkward.
#if defined(_MSC_VER)
#define LLT_CACHELINE_ALIGNED __declspec(align(64))
#else
#define LLT_CACHELINE_ALIGNED __attribute__((aligned(64)))
#endif

// -----------------------------------------------------------------------------
// Branch prediction hints
// -----------------------------------------------------------------------------
// LLT_LIKELY(x)   -> tells the compiler the condition is usually true
// LLT_UNLIKELY(x) -> usually false (error / contention paths)
// On the hot path these keep the fall-through (likely) code laid out inline and
// push cold code out of the instruction stream.
#if defined(__GNUC__) || defined(__clang__)
#define LLT_LIKELY(x) (__builtin_expect(!!(x), 1))
#define LLT_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define LLT_LIKELY(x) (x)
#define LLT_UNLIKELY(x) (x)
#endif

// -----------------------------------------------------------------------------
// Force-inline / no-inline
// -----------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
#define LLT_ALWAYS_INLINE inline __attribute__((always_inline))
#define LLT_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define LLT_ALWAYS_INLINE __forceinline
#define LLT_NOINLINE __declspec(noinline)
#else
#define LLT_ALWAYS_INLINE inline
#define LLT_NOINLINE
#endif

namespace llt {

// -----------------------------------------------------------------------------
// compiler_barrier()
// -----------------------------------------------------------------------------
// Prevents the *compiler* from reordering memory operations across this point.
// It emits no instructions and is NOT a hardware fence -- it does not stop the
// CPU from reordering. Use std::atomic with the right memory_order for actual
// inter-thread ordering; use this only to pin down compiler scheduling (e.g.
// around an rdtsc read or when reasoning about a benchmark loop).
LLT_ALWAYS_INLINE void compiler_barrier() noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("" ::: "memory");
#elif defined(_MSC_VER)
  _ReadWriteBarrier();
#endif
}

// -----------------------------------------------------------------------------
// cpu_pause()
// -----------------------------------------------------------------------------
// The relaxation hint to drop into the body of a busy-wait spin loop. On x86
// the PAUSE instruction (a) hints the core that this is a spin-wait so it can
// avoid a costly memory-order-violation pipeline flush when the loop finally
// exits, and (b) yields pipeline resources to the SMT sibling. On AArch64 the
// closest analogue is the YIELD hint. On unknown targets it degrades to a
// compiler barrier so the loop condition is re-read.
LLT_ALWAYS_INLINE void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER)
  _mm_pause();
#else
  __builtin_ia32_pause();
#endif
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  compiler_barrier();
#endif
}

// -----------------------------------------------------------------------------
// prefetch()
// -----------------------------------------------------------------------------
// Hint the hardware prefetcher to pull a cache line toward the core before it
// is dereferenced. `locality` is 0..3 (0 = no temporal locality / evict soon,
// 3 = high locality / keep). `write` set to true requests the line for
// modification (prefetch-for-write). No-op where unsupported.
template <int Locality = 3, bool Write = false>
LLT_ALWAYS_INLINE void prefetch(const void* addr) noexcept {
  static_assert(Locality >= 0 && Locality <= 3, "locality must be in [0,3]");
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, Write ? 1 : 0, Locality);
#else
  (void)addr;
#endif
}

}  // namespace llt
