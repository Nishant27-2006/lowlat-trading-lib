# Microbenchmarks built on Google Benchmark.

add_executable(bench_spsc_queue bench_spsc_queue.cpp)
target_link_libraries(bench_spsc_queue PRIVATE llt::llt benchmark::benchmark)
target_compile_options(bench_spsc_queue PRIVATE ${LLT_WARNINGS})

add_executable(bench_pool bench_pool.cpp)
target_link_libraries(bench_pool PRIVATE llt::llt benchmark::benchmark)
target_compile_options(bench_pool PRIVATE ${LLT_WARNINGS})
