// =============================================================================
// test_thread_local_pool.cpp
//
// Tests for FixedBlockPool / ThreadLocalPool:
//   * allocate returns distinct, usable, correctly-aligned addresses,
//   * exhaustion returns nullptr,
//   * deallocate makes a block available again (reuse),
//   * accounting (in_use / available) is correct,
//   * each thread gets its own ThreadLocalPool instance.
// =============================================================================

#include "llt/memory/thread_local_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

TEST(FixedBlockPool, AllocateDistinctAddresses) {
  llt::FixedBlockPool<64, 16> pool;
  std::set<void*> seen;
  for (int i = 0; i < 16; ++i) {
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(pool.owns(p));
    EXPECT_TRUE(seen.insert(p).second) << "duplicate address handed out";
  }
}

TEST(FixedBlockPool, ExhaustionReturnsNull) {
  llt::FixedBlockPool<32, 4> pool;
  EXPECT_EQ(pool.available(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_NE(pool.allocate(), nullptr);
  }
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_EQ(pool.in_use(), 4u);
  EXPECT_EQ(pool.allocate(), nullptr);  // exhausted
}

TEST(FixedBlockPool, ReuseAfterFree) {
  llt::FixedBlockPool<32, 2> pool;
  void* a = pool.allocate();
  void* b = pool.allocate();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(pool.allocate(), nullptr);  // full

  pool.deallocate(a);
  EXPECT_EQ(pool.available(), 1u);
  void* c = pool.allocate();  // should reuse a freed block
  ASSERT_NE(c, nullptr);
  EXPECT_TRUE(c == a || c == b);
  EXPECT_EQ(pool.available(), 0u);
}

TEST(FixedBlockPool, BlocksAreUsableAndAligned) {
  llt::FixedBlockPool<sizeof(std::uint64_t), 8> pool;
  std::vector<std::uint64_t*> ptrs;
  for (int i = 0; i < 8; ++i) {
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(std::uint64_t), 0u);
    auto* v = static_cast<std::uint64_t*>(p);
    *v = static_cast<std::uint64_t>(i);  // write should be safe
    ptrs.push_back(v);
  }
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(*ptrs[static_cast<std::size_t>(i)], static_cast<std::uint64_t>(i));
  }
}

TEST(FixedBlockPool, DeallocateNullIsNoOp) {
  llt::FixedBlockPool<32, 2> pool;
  pool.deallocate(nullptr);
  EXPECT_EQ(pool.available(), 2u);
}

TEST(ThreadLocalPool, EachThreadHasOwnInstance) {
  using TLP = llt::ThreadLocalPool<64, 4>;

  void* main_first = TLP::allocate();
  ASSERT_NE(main_first, nullptr);

  void* other_first = nullptr;
  bool other_exhausts_independently = false;

  std::thread t([&] {
    // A fresh pool on this thread => its own 4 blocks regardless of the main
    // thread already having taken one.
    void* p0 = TLP::allocate();
    void* p1 = TLP::allocate();
    void* p2 = TLP::allocate();
    void* p3 = TLP::allocate();
    void* p4 = TLP::allocate();  // 5th must fail -> independent capacity of 4
    other_first = p0;
    other_exhausts_independently =
        (p0 && p1 && p2 && p3 && p4 == nullptr);
    TLP::deallocate(p0);
    TLP::deallocate(p1);
    TLP::deallocate(p2);
    TLP::deallocate(p3);
  });
  t.join();

  EXPECT_TRUE(other_exhausts_independently);
  // The other thread's pool is distinct storage from this thread's.
  EXPECT_NE(other_first, nullptr);

  TLP::deallocate(main_first);
}
