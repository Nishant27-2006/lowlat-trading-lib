#pragma once

// =============================================================================
// llt/sys/cpu_affinity.hpp
//
// Helpers to pin a thread to a specific CPU core and to query the core count.
//
// WHY: on the critical path you want a thread to stay on one core so that its
// L1/L2 caches stay warm and the scheduler never migrates it (a migration costs
// thousands of cycles of cold caches plus TLB refills). Combined with kernel
// isolation (isolcpus / nohz_full -- see docs/KERNEL_TUNING.md) pinning is what
// turns "usually fast" into "deterministically fast".
//
// PLATFORM: real implementation on Linux via pthread_setaffinity_np /
// sched_setaffinity. On other platforms the functions compile but return an
// error/empty result so downstream code is portable.
//
// IMPORTANT: _GNU_SOURCE must be defined BEFORE any libc header is included for
// the GNU affinity extensions and the cpu_set_t macros to be declared. We do it
// here, guarded, before our own includes pull anything in.
// =============================================================================

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <cstddef>
#include <system_error>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace llt {
namespace sys {

// Result type: ok flag plus a portable error code (errno on Linux, generic
// "not supported" elsewhere). We avoid throwing so this is usable on no-except
// hot paths and in CI environments that restrict affinity calls.
struct AffinityResult {
  bool ok = false;
  int error = 0;  // errno-style code; 0 when ok

  explicit operator bool() const noexcept { return ok; }
};

// ---------------------------------------------------------------------------
// hardware_concurrency -- number of logical cores visible to the process.
// ---------------------------------------------------------------------------
inline std::size_t hardware_concurrency() noexcept {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1u : n;
}

// ---------------------------------------------------------------------------
// pin_this_thread -- pin the CALLING thread to a single logical core.
// ---------------------------------------------------------------------------
inline AffinityResult pin_this_thread(std::size_t core_id) noexcept {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core_id, &set);
  // pthread_setaffinity_np returns an errno-style code directly (0 == success).
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (rc != 0) {
    return AffinityResult{false, rc};
  }
  return AffinityResult{true, 0};
#else
  (void)core_id;
  return AffinityResult{false, /*ENOSYS-ish*/ 38};
#endif
}

// ---------------------------------------------------------------------------
// pin_thread -- pin a std::thread (by native handle) to a single core. Must be
// called while the thread is alive/joinable.
// ---------------------------------------------------------------------------
inline AffinityResult pin_thread(std::thread& t, std::size_t core_id) noexcept {
#if defined(__linux__)
  if (!t.joinable()) {
    return AffinityResult{false, /*EINVAL*/ 22};
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core_id, &set);
  const int rc = pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
  if (rc != 0) {
    return AffinityResult{false, rc};
  }
  return AffinityResult{true, 0};
#else
  (void)t;
  (void)core_id;
  return AffinityResult{false, 38};
#endif
}

// ---------------------------------------------------------------------------
// current_core -- which core is the calling thread currently the *only* member
// of its affinity mask for? Returns -1 if it is pinned to more than one core or
// the query is unsupported. Primarily used by tests to read back a pin.
// ---------------------------------------------------------------------------
inline int current_affinity_single_core() noexcept {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  const int rc = pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
  if (rc != 0) {
    return -1;
  }
  int found = -1;
  const int n = CPU_COUNT(&set);
  if (n != 1) {
    return -1;  // pinned to multiple cores -> not a single-core pin
  }
  // Find the one set bit. Scan a generous range of possible CPU ids.
  for (int i = 0; i < CPU_SETSIZE; ++i) {
    if (CPU_ISSET(i, &set)) {
      found = i;
      break;
    }
  }
  return found;
#else
  return -1;
#endif
}

}  // namespace sys
}  // namespace llt
