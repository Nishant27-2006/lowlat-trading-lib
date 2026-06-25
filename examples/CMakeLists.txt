# Example programs. Each links the header-only library (which brings in
# Threads::Threads transitively).

add_executable(example_spsc example_spsc.cpp)
target_link_libraries(example_spsc PRIVATE llt::llt)
target_compile_options(example_spsc PRIVATE ${LLT_WARNINGS})

add_executable(example_pool example_pool.cpp)
target_link_libraries(example_pool PRIVATE llt::llt)
target_compile_options(example_pool PRIVATE ${LLT_WARNINGS})
