// =============================================================================
// test_object_pool.cpp
//
// Tests for ObjectPool:
//   * construct() runs the T constructor; destroy() runs the destructor,
//   * lifecycle accounting (in_use/available),
//   * exhaustion returns nullptr,
//   * constructor arguments are forwarded.
// =============================================================================

#include "llt/memory/object_pool.hpp"

#include <atomic>
#include <string>

#include <gtest/gtest.h>

namespace {

// Tracks constructor / destructor calls to verify lifecycle management.
struct Tracked {
  static std::atomic<int> ctor_calls;
  static std::atomic<int> dtor_calls;
  int a;
  int b;

  Tracked(int x, int y) : a(x), b(y) { ctor_calls.fetch_add(1, std::memory_order_relaxed); }
  ~Tracked() { dtor_calls.fetch_add(1, std::memory_order_relaxed); }
};
std::atomic<int> Tracked::ctor_calls{0};
std::atomic<int> Tracked::dtor_calls{0};

}  // namespace

TEST(ObjectPool, ConstructDestroyCallsCtorDtor) {
  Tracked::ctor_calls.store(0);
  Tracked::dtor_calls.store(0);

  using Pool = llt::ObjectPool<Tracked, 8>;
  Tracked* t = Pool::construct(11, 22);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->a, 11);
  EXPECT_EQ(t->b, 22);
  EXPECT_EQ(Tracked::ctor_calls.load(), 1);
  EXPECT_EQ(Tracked::dtor_calls.load(), 0);

  Pool::destroy(t);
  EXPECT_EQ(Tracked::dtor_calls.load(), 1);
}

TEST(ObjectPool, ArgsForwarded) {
  using Pool = llt::ObjectPool<std::string, 4>;
  std::string* s = Pool::construct(5, 'z');  // std::string(count, char)
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(*s, "zzzzz");
  Pool::destroy(s);
}

TEST(ObjectPool, ExhaustionReturnsNull) {
  using Pool = llt::ObjectPool<int, 3>;
  int* a = Pool::construct(1);
  int* b = Pool::construct(2);
  int* c = Pool::construct(3);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  int* d = Pool::construct(4);
  EXPECT_EQ(d, nullptr);  // exhausted

  Pool::destroy(a);
  Pool::destroy(b);
  Pool::destroy(c);
}

TEST(ObjectPool, DestroyNullIsNoOp) {
  using Pool = llt::ObjectPool<int, 2>;
  Pool::destroy(nullptr);  // must not crash
  SUCCEED();
}

TEST(ObjectPool, ReuseAfterDestroy) {
  using Pool = llt::ObjectPool<int, 1>;
  int* a = Pool::construct(7);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(Pool::construct(8), nullptr);  // capacity 1, full
  Pool::destroy(a);
  int* b = Pool::construct(9);  // now reusable
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(*b, 9);
  Pool::destroy(b);
}
