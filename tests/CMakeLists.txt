# Unit tests. Each test executable links the library and GoogleTest.

include(GoogleTest)

set(LLT_TESTS
  test_spsc_queue
  test_thread_local_pool
  test_object_pool
  test_rdtsc_clock
  test_cpu_affinity)

foreach(test_name ${LLT_TESTS})
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE llt::llt GTest::gtest_main)
  target_compile_options(${test_name} PRIVATE ${LLT_WARNINGS})
  # gtest_discover_tests registers each TEST() with CTest so failures are
  # individually reported by `ctest --output-on-failure`.
  gtest_discover_tests(${test_name})
endforeach()
