cmake_minimum_required(VERSION 3.16)

project(lowlat_trading_lib
        VERSION 1.0.0
        DESCRIPTION "Ultra-Low Latency Trading Infrastructure Library"
        LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Standard
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release build if the user did not specify one. The whole point of
# this library is latency, so an un-optimized default would be misleading.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
# Only build tests/benches/examples when this is the top-level project, so the
# library can be pulled in via add_subdirectory/FetchContent without dragging in
# GoogleTest etc. The variables can also be overridden explicitly.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(LLT_IS_TOP_LEVEL ON)
else()
  set(LLT_IS_TOP_LEVEL OFF)
endif()

option(LLT_BUILD_TESTS      "Build unit tests"        ${LLT_IS_TOP_LEVEL})
option(LLT_BUILD_BENCHMARKS "Build microbenchmarks"   ${LLT_IS_TOP_LEVEL})
option(LLT_BUILD_EXAMPLES   "Build example programs"  ${LLT_IS_TOP_LEVEL})

# -----------------------------------------------------------------------------
# The library: header-only INTERFACE target `llt`.
# -----------------------------------------------------------------------------
add_library(llt INTERFACE)
add_library(llt::llt ALIAS llt)

target_include_directories(llt INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_compile_features(llt INTERFACE cxx_std_17)

# pthread (affinity, std::thread). Threads::Threads is the modern, portable way.
find_package(Threads REQUIRED)
target_link_libraries(llt INTERFACE Threads::Threads)

# -----------------------------------------------------------------------------
# Warning flags applied to OUR test/bench/example targets (not exported to
# consumers of the INTERFACE library).
# -----------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(LLT_WARNINGS -Wall -Wextra -Wpedantic)
else()
  set(LLT_WARNINGS "")
endif()

# -----------------------------------------------------------------------------
# Dependencies via FetchContent (no system installs required for CI).
# -----------------------------------------------------------------------------
include(FetchContent)

if(LLT_BUILD_TESTS OR LLT_BUILD_BENCHMARKS)
  if(LLT_BUILD_TESTS)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG release-1.14.0)
    # Do not install gtest as part of our install step.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    # On MSVC, link gtest against the shared CRT consistently.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()

  if(LLT_BUILD_BENCHMARKS)
    FetchContent_Declare(
      benchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG v1.8.3)
    # Keep benchmark lean: no self-tests, no gtest dependency, no install.
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  endif()

  if(LLT_BUILD_TESTS)
    FetchContent_MakeAvailable(googletest)
  endif()
  if(LLT_BUILD_BENCHMARKS)
    FetchContent_MakeAvailable(benchmark)
  endif()
endif()

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
if(LLT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LLT_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(LLT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# -----------------------------------------------------------------------------
# Examples live in examples/ but have no CMakeLists of their own; declare here
# only if the directory is built. (examples/CMakeLists.txt handles the targets.)
# -----------------------------------------------------------------------------
