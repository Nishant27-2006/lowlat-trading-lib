#pragma once

// =============================================================================
// llt/util/latency_histogram.hpp
//
// LatencyHistogram -- a small, fixed-memory, HDR-style latency histogram for
// reporting p50 / p99 / p99.9 in benchmarks and examples.
//
// DESIGN: classic HDR histogram idea. Values (nanoseconds) are bucketed on a
// logarithmic scale: the bucket index is the position of the highest set bit,
// optionally refined with a few sub-buckets per power of two for precision.
// This gives roughly constant relative error across many orders of magnitude
// (a few ns to seconds) using a small fixed array.
//
// HOT PATH: record() does NO allocation -- the bucket array is a fixed member.
// It is a couple of integer ops plus one array increment, so it can be called
// inside a measured loop without perturbing the measurement materially.
//
// THREAD-SAFETY: not thread-safe. Use one histogram per thread and merge, or
// only record from a single thread. (Typical bench/example usage records on one
// thread then reports.)
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>

namespace llt {
namespace util {

// SubBucketBits controls precision: 2^SubBucketBits sub-buckets per power of
// two. 4 bits => 16 sub-buckets => ~1/16 (~6%) max relative error, which is
// plenty for p50/p99/p99.9 latency reporting.
template <unsigned SubBucketBits = 4>
class BasicLatencyHistogram {
  static_assert(SubBucketBits >= 1 && SubBucketBits <= 10, "SubBucketBits out of range");

 public:
  static constexpr unsigned kSubBucketBits = SubBucketBits;
  static constexpr unsigned kSubBucketCount = 1u << SubBucketBits;
  // 64 power-of-two "exponent" buckets (covers the full uint64 range), each
  // split into kSubBucketCount linear sub-buckets.
  static constexpr unsigned kExponentBuckets = 64;
  static constexpr std::size_t kNumBuckets =
      static_cast<std::size_t>(kExponentBuckets) * kSubBucketCount;

  BasicLatencyHistogram() noexcept { clear(); }

  void clear() noexcept {
    counts_.fill(0);
    count_ = 0;
    min_ = UINT64_MAX;
    max_ = 0;
    sum_ = 0;
  }

  // -------------------------------------------------------------------------
  // record -- add one sample (nanoseconds). No allocation, no syscalls.
  // -------------------------------------------------------------------------
  void record(std::uint64_t value_ns) noexcept {
    const std::size_t idx = bucket_index(value_ns);
    ++counts_[idx];
    ++count_;
    sum_ += value_ns;
    if (value_ns < min_) min_ = value_ns;
    if (value_ns > max_) max_ = value_ns;
  }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
  }

  // -------------------------------------------------------------------------
  // percentile -- value (ns) at the given percentile p in [0,100].
  // Walks the cumulative distribution and returns the representative value of
  // the bucket containing the target rank. Returns 0 if no samples.
  // -------------------------------------------------------------------------
  std::uint64_t percentile(double p) const noexcept {
    if (count_ == 0) {
      return 0;
    }
    if (p < 0.0) p = 0.0;
    if (p > 100.0) p = 100.0;

    // Rank of the desired sample (1-based). For p=100 this is the last sample.
    std::uint64_t target = static_cast<std::uint64_t>((p / 100.0) * static_cast<double>(count_));
    if (target == 0) target = 1;
    if (target > count_) target = count_;

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kNumBuckets; ++i) {
      cumulative += counts_[i];
      if (cumulative >= target) {
        return bucket_representative_value(i);
      }
    }
    return max_;
  }

  // Merge another histogram into this one (same template params). Useful for
  // per-thread histograms aggregated at the end.
  void merge(const BasicLatencyHistogram& other) noexcept {
    for (std::size_t i = 0; i < kNumBuckets; ++i) {
      counts_[i] += other.counts_[i];
    }
    count_ += other.count_;
    sum_ += other.sum_;
    if (other.count_ != 0) {
      if (other.min_ < min_) min_ = other.min_;
      if (other.max_ > max_) max_ = other.max_;
    }
  }

 private:
  // Index of the most significant set bit (0 for value 0). 63 for the top bit.
  static unsigned highest_bit(std::uint64_t v) noexcept {
    if (v == 0) {
      return 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    return 63u - static_cast<unsigned>(__builtin_clzll(v));
#else
    unsigned r = 0;
    while (v >>= 1) ++r;
    return r;
#endif
  }

  // Map a value to a (exponent, sub-bucket) flat index.
  static std::size_t bucket_index(std::uint64_t value) noexcept {
    const unsigned exp = highest_bit(value);
    // Extract the next kSubBucketBits below the top bit for the sub-bucket.
    unsigned sub = 0;
    if (exp >= kSubBucketBits) {
      sub = static_cast<unsigned>((value >> (exp - kSubBucketBits)) & (kSubBucketCount - 1));
    } else {
      // Small values: scale up so they still distribute across sub-buckets.
      sub = static_cast<unsigned>((value << (kSubBucketBits - exp)) & (kSubBucketCount - 1));
    }
    std::size_t idx = static_cast<std::size_t>(exp) * kSubBucketCount + sub;
    if (idx >= kNumBuckets) {
      idx = kNumBuckets - 1;
    }
    return idx;
  }

  // A representative (lower-edge) value for a bucket, for percentile reporting.
  static std::uint64_t bucket_representative_value(std::size_t idx) noexcept {
    const unsigned exp = static_cast<unsigned>(idx / kSubBucketCount);
    const unsigned sub = static_cast<unsigned>(idx % kSubBucketCount);
    if (exp == 0) {
      return sub;
    }
    if (exp >= kSubBucketBits) {
      // value ~= (1 << exp) base, plus sub * (1 << (exp - SubBucketBits))
      const std::uint64_t base = static_cast<std::uint64_t>(1) << exp;
      const std::uint64_t step = static_cast<std::uint64_t>(1) << (exp - kSubBucketBits);
      return base + static_cast<std::uint64_t>(sub) * step;
    }
    return static_cast<std::uint64_t>(1) << exp;
  }

  std::array<std::uint64_t, kNumBuckets> counts_;
  std::uint64_t count_;
  std::uint64_t min_;
  std::uint64_t max_;
  std::uint64_t sum_;
};

// Convenient default alias.
using LatencyHistogram = BasicLatencyHistogram<4>;

}  // namespace util
}  // namespace llt
