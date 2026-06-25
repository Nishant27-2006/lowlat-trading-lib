#pragma once

// =============================================================================
// llt/sys/rdtsc_clock.hpp
//
// RdtscClock -- a high-resolution timestamp source for latency measurement.
//
// On x86-64 we read the Time Stamp Counter (TSC) directly with RDTSC / RDTSCP.
// This is dramatically cheaper than a clock_gettime() syscall/vDSO call (a few
// cycles vs. tens-to-hundreds of nanoseconds) and gives sub-nanosecond
// resolution, which is exactly what you need to measure inter-thread latencies
// in the tens-of-nanoseconds range.
//
// SERIALIZATION: RDTSC can be reordered by the out-of-order engine relative to
// the surrounding instructions. For a "start" timestamp we precede it with an
// LFENCE so earlier instructions have retired; for an "end" timestamp we use
// RDTSCP (which waits for prior instructions to complete) followed by LFENCE so
// later instructions do not float up before the read. now_serialized() bundles
// the begin form.
//
// CALIBRATION: the TSC ticks at a fixed, invariant rate on modern CPUs (it is
// NOT the current core frequency). We measure that rate once by sleeping a known
// wall-clock interval (std::chrono::steady_clock) and dividing the TSC delta by
// the elapsed nanoseconds, giving cycles-per-nanosecond. cycles_to_ns() then
// converts any cycle delta to nanoseconds.
//
// PORTABILITY: on non-x86 targets (e.g. CI ARM runners) the whole thing falls
// back to std::chrono::steady_clock, reporting "cycles" that are actually
// nanoseconds and a ratio of 1.0, so all calling code compiles and runs.
// =============================================================================

#include <cstdint>
#include <chrono>
#include <thread>

#include "llt/common/cache.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#define LLT_HAS_RDTSC 1
#include <x86intrin.h>
#else
#define LLT_HAS_RDTSC 0
#endif

namespace llt {
namespace sys {

class RdtscClock {
 public:
  // -------------------------------------------------------------------------
  // now() -- a fast, lightly-ordered timestamp in "cycles".
  // Cheapest read; suitable for marking the END of an interval after a
  // serialized start, or wherever a tiny amount of skew is tolerable.
  // -------------------------------------------------------------------------
  static LLT_ALWAYS_INLINE std::uint64_t now() noexcept {
#if LLT_HAS_RDTSC
    return __rdtsc();
#else
    return steady_ns();
#endif
  }

  // -------------------------------------------------------------------------
  // now_serialized() -- a fully fenced timestamp suitable for the START of an
  // interval. LFENCE ensures all prior instructions have retired before the
  // counter is sampled, so nothing from before the measured region leaks in.
  // -------------------------------------------------------------------------
  static LLT_ALWAYS_INLINE std::uint64_t now_serialized() noexcept {
#if LLT_HAS_RDTSC
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return steady_ns();
#endif
  }

  // -------------------------------------------------------------------------
  // now_end() -- a fully fenced timestamp suitable for the END of an interval.
  // RDTSCP serializes against prior instruction completion; the trailing LFENCE
  // stops later instructions from being sampled early.
  // -------------------------------------------------------------------------
  static LLT_ALWAYS_INLINE std::uint64_t now_end() noexcept {
#if LLT_HAS_RDTSC
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return steady_ns();
#endif
  }

  // -------------------------------------------------------------------------
  // calibrate -- measure cycles-per-nanosecond by sleeping a known interval.
  // Call once at startup (it sleeps for `sample_ms` milliseconds). Stores the
  // ratio for cycles_to_ns(). Returns the measured TSC frequency in Hz.
  // -------------------------------------------------------------------------
  static double calibrate(unsigned sample_ms = 50) noexcept {
    using clock = std::chrono::steady_clock;

    const std::uint64_t c0 = now_serialized();
    const auto t0 = clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));

    const std::uint64_t c1 = now_end();
    const auto t1 = clock::now();

    const std::uint64_t cycles = c1 - c0;
    const double elapsed_ns =
        std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(t1 - t0).count();

    double cpns = 1.0;
    if (elapsed_ns > 0.0 && cycles > 0) {
#if LLT_HAS_RDTSC
      cpns = static_cast<double>(cycles) / elapsed_ns;
#else
      // In fallback mode "cycles" are already nanoseconds, so 1 cycle == 1 ns.
      cpns = 1.0;
#endif
    }
    if (cpns <= 0.0) {
      cpns = 1.0;  // never store a non-positive ratio
    }
    cycles_per_ns() = cpns;
    return cpns * 1e9;  // Hz
  }

  // -------------------------------------------------------------------------
  // cycles_to_ns -- convert a cycle delta to nanoseconds using the calibrated
  // ratio. If calibrate() was never called the ratio defaults to 1.0, which is
  // exactly correct in the non-x86 fallback and a harmless identity otherwise.
  // -------------------------------------------------------------------------
  static double cycles_to_ns(std::uint64_t cycles) noexcept {
    return static_cast<double>(cycles) / cycles_per_ns();
  }

  // Convenience: nanoseconds elapsed between two timestamps from this clock.
  static double elapsed_ns(std::uint64_t start, std::uint64_t end) noexcept {
    return cycles_to_ns(end - start);
  }

  // The calibrated cycles-per-nanosecond (e.g. ~3.0 on a 3 GHz invariant TSC).
  static double cycles_per_ns_value() noexcept { return cycles_per_ns(); }

  static constexpr bool uses_tsc() noexcept { return LLT_HAS_RDTSC != 0; }

 private:
  // Calibrated ratio stored in a function-local static so the header stays
  // single-definition-safe across translation units (no ODR violation).
  static double& cycles_per_ns() noexcept {
    static double ratio = 1.0;
    return ratio;
  }

#if !LLT_HAS_RDTSC
  static std::uint64_t steady_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
#endif
};

}  // namespace sys
}  // namespace llt
