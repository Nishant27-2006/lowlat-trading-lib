#pragma once

// =============================================================================
// llt/sys/hugepages.hpp
//
// Helpers to back a memory region with huge pages (typically 2 MiB on x86-64
// instead of the default 4 KiB).
//
// WHY: the TLB (translation-lookaside buffer) caches virtual->physical page
// mappings. With 4 KiB pages a few megabytes of hot data needs hundreds of TLB
// entries; misses cost a page-table walk (tens to hundreds of cycles) right in
// the middle of the critical path, adding latency jitter. A single 2 MiB huge
// page covers 512x more memory per TLB entry, so large hot structures (ring
// buffers, pools, order books) incur far fewer TLB misses and behave more
// deterministically.
//
// TWO STRATEGIES:
//   * allocate_hugepages(): mmap an anonymous region with MAP_HUGETLB. This
//     demands huge pages explicitly and fails if none are reserved
//     (/proc/sys/vm/nr_hugepages). Strongest guarantee.
//   * advise_hugepages(): madvise(MADV_HUGEPAGE) on an existing region, asking
//     the kernel's Transparent Huge Pages machinery to promote it. Best-effort,
//     no reservation required.
//
// PLATFORM: real on Linux; stubs elsewhere so callers stay portable. None of
// this throws; failures are reported via return value / bool.
// =============================================================================

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <cstddef>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace llt {
namespace sys {

// Default Linux huge page size on x86-64.
inline constexpr std::size_t kDefaultHugePageSize = 2u * 1024u * 1024u;  // 2 MiB

// A handle to an explicitly-mapped huge-page region. Owns the mapping and
// unmaps it on destruction (RAII). Movable, not copyable.
class HugePageRegion {
 public:
  HugePageRegion() noexcept : ptr_(nullptr), size_(0) {}

  HugePageRegion(void* p, std::size_t n) noexcept : ptr_(p), size_(n) {}

  HugePageRegion(const HugePageRegion&) = delete;
  HugePageRegion& operator=(const HugePageRegion&) = delete;

  HugePageRegion(HugePageRegion&& other) noexcept : ptr_(other.ptr_), size_(other.size_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
  }

  HugePageRegion& operator=(HugePageRegion&& other) noexcept {
    if (this != &other) {
      release();
      ptr_ = other.ptr_;
      size_ = other.size_;
      other.ptr_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  ~HugePageRegion() { release(); }

  void* data() const noexcept { return ptr_; }
  std::size_t size() const noexcept { return size_; }
  bool valid() const noexcept { return ptr_ != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

 private:
  void release() noexcept {
#if defined(__linux__)
    if (ptr_ != nullptr) {
      ::munmap(ptr_, size_);
    }
#endif
    ptr_ = nullptr;
    size_ = 0;
  }

  void* ptr_;
  std::size_t size_;
};

// ---------------------------------------------------------------------------
// allocate_hugepages -- mmap `bytes` (rounded up to a huge page multiple) using
// MAP_HUGETLB. Returns an invalid HugePageRegion on failure (e.g. no huge pages
// reserved). Caller should check .valid() and fall back to normal allocation.
// ---------------------------------------------------------------------------
inline HugePageRegion allocate_hugepages(std::size_t bytes,
                                         std::size_t page_size = kDefaultHugePageSize) noexcept {
#if defined(__linux__)
  if (bytes == 0 || page_size == 0) {
    return HugePageRegion{};
  }
  const std::size_t rounded = ((bytes + page_size - 1) / page_size) * page_size;
  void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (p == MAP_FAILED) {
    return HugePageRegion{};  // not available; caller falls back
  }
  return HugePageRegion{p, rounded};
#else
  (void)bytes;
  (void)page_size;
  return HugePageRegion{};
#endif
}

// ---------------------------------------------------------------------------
// advise_hugepages -- ask THP to back an EXISTING region with huge pages.
// Best-effort; returns true if the advice call succeeded (the kernel may still
// decline to promote). For best effect the region should be huge-page aligned
// and sized, but it is safe to call on any region.
// ---------------------------------------------------------------------------
inline bool advise_hugepages(void* addr, std::size_t bytes) noexcept {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  if (addr == nullptr || bytes == 0) {
    return false;
  }
  return ::madvise(addr, bytes, MADV_HUGEPAGE) == 0;
#else
  (void)addr;
  (void)bytes;
  return false;
#endif
}

// True if this platform provides a real huge-page implementation.
inline constexpr bool hugepages_supported() noexcept {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

}  // namespace sys
}  // namespace llt
