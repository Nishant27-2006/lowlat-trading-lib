// =============================================================================
// test_rdtsc_clock.cpp
//
// Tests for RdtscClock:
//   * timestamps are monotonically non-decreasing,
//   * calibration yields a plausible positive frequency,
//   * cycles_to_ns produces sane values,
//   * elapsed_ns over a known sleep is in the right ballpark.
//
// These are tolerant of CI jitter: we assert ranges, not exact values.
// =============================================================================

#include "llt/sys/rdtsc_clock.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

using llt::sys::RdtscClock;

TEST(RdtscClock, MonotonicNonDecreasing) {
  std::uint64_t prev = RdtscClock::now_serialized();
  for (int i = 0; i < 10000; ++i) {
    const std::uint64_t cur = RdtscClock::now();
    EXPECT_GE(cur, prev);
    prev = cur;
  }
}

TEST(RdtscClock, CalibrationPositiveFrequency) {
  const double hz = RdtscClock::calibrate(30);
  EXPECT_GT(hz, 0.0);
  EXPECT_GT(RdtscClock::cycles_per_ns_value(), 0.0);

  if (RdtscClock::uses_tsc()) {
    // A real invariant TSC runs somewhere between ~0.5 GHz and ~10 GHz.
    EXPECT_GT(hz, 5.0e8);
    EXPECT_LT(hz, 1.0e10);
  } else {
    // Fallback: steady_clock nanoseconds => 1 cycle per ns => 1 GHz.
    EXPECT_NEAR(hz, 1.0e9, 1.0);
  }
}

TEST(RdtscClock, CyclesToNsSane) {
  RdtscClock::calibrate(30);
  const double cpns = RdtscClock::cycles_per_ns_value();
  // Converting `cpns` cycles should give ~1 ns.
  const double ns = RdtscClock::cycles_to_ns(static_cast<std::uint64_t>(cpns + 0.5));
  EXPECT_GT(ns, 0.0);
  EXPECT_LT(ns, 100.0);
}

TEST(RdtscClock, ElapsedOverKnownSleepInRange) {
  RdtscClock::calibrate(30);
  const std::uint64_t start = RdtscClock::now_serialized();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const std::uint64_t end = RdtscClock::now_end();

  const double ns = RdtscClock::elapsed_ns(start, end);
  // Expect ~20 ms == 20,000,000 ns, but be generous for scheduler jitter on CI:
  // anywhere from 10 ms to 200 ms is acceptable for a "sane" check.
  EXPECT_GT(ns, 1.0e7);
  EXPECT_LT(ns, 2.0e8);
}
