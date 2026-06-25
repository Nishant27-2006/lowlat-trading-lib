// =============================================================================
// test_cpu_affinity.cpp
//
// Tests for cpu_affinity. CI runners (and containers) frequently restrict
// sched_setaffinity, so these tests are ROBUST: if pinning is not permitted we
// GTEST_SKIP() rather than failing. On non-Linux the whole suite is skipped.
// =============================================================================

#include "llt/sys/cpu_affinity.hpp"

#include <cstddef>

#include <gtest/gtest.h>

TEST(CpuAffinity, HardwareConcurrencyAtLeastOne) {
  EXPECT_GE(llt::sys::hardware_concurrency(), 1u);
}

TEST(CpuAffinity, PinThisThreadAndReadBack) {
#if !defined(__linux__)
  GTEST_SKIP() << "CPU affinity is only implemented on Linux";
#else
  const auto r = llt::sys::pin_this_thread(0);
  if (!r) {
    // The environment forbids setting affinity (common in CI sandboxes).
    GTEST_SKIP() << "pin_this_thread(0) not permitted here (error " << r.error << ")";
  }
  const int core = llt::sys::current_affinity_single_core();
  if (core < 0) {
    GTEST_SKIP() << "could not read back a single-core affinity in this environment";
  }
  EXPECT_EQ(core, 0);
#endif
}

TEST(CpuAffinity, PinToSecondCoreIfAvailable) {
#if !defined(__linux__)
  GTEST_SKIP() << "CPU affinity is only implemented on Linux";
#else
  if (llt::sys::hardware_concurrency() < 2) {
    GTEST_SKIP() << "only one core visible; cannot test core 1";
  }
  const auto r = llt::sys::pin_this_thread(1);
  if (!r) {
    GTEST_SKIP() << "pin_this_thread(1) not permitted here (error " << r.error << ")";
  }
  const int core = llt::sys::current_affinity_single_core();
  if (core < 0) {
    GTEST_SKIP() << "could not read back affinity";
  }
  EXPECT_EQ(core, 1);
#endif
}
