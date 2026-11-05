cmake_minimum_required(VERSION 3.14)
project(locks CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOCKS_NATIVE_CPU "Tune for the build machine's CPU (-mcpu=native on ARM, -march=native on x86). Use when building on the target." OFF)
set(LOCKS_SPIN_MAX_BACKOFF "" CACHE STRING "Override the spin backoff limit (relax() calls per round before yielding). Empty = header default.")

if(LOCKS_NATIVE_CPU)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm)")
    # On ARMv8.1+ cores this inlines LSE atomics (swp/cas) instead of
    # LL/SC loops or outline-atomics library calls.
    add_compile_options(-mcpu=native)
  else()
    add_compile_options(-march=native)
  endif()
endif()

find_package(Threads REQUIRED)
find_package(Boost 1.66 REQUIRED COMPONENTS thread atomic)

add_library(locks INTERFACE)
target_include_directories(locks INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(locks INTERFACE Boost::thread Boost::atomic Threads::Threads)
if(NOT LOCKS_SPIN_MAX_BACKOFF STREQUAL "")
  target_compile_definitions(locks INTERFACE LOCKS_SPIN_MAX_BACKOFF=${LOCKS_SPIN_MAX_BACKOFF})
endif()

enable_testing()

add_executable(spinlock_tests tests/spinlock_tests.cpp)
target_link_libraries(spinlock_tests PRIVATE locks)
target_include_directories(spinlock_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)
# Keep asserts on in the tests regardless of build type.
target_compile_options(spinlock_tests PRIVATE -UNDEBUG)
add_test(NAME spinlock_tests COMMAND spinlock_tests)

add_executable(lock_benchmark bench/lock_benchmark.cpp)
target_link_libraries(lock_benchmark PRIVATE locks)
