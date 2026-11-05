cmake_minimum_required(VERSION 3.14)
project(locks CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(Boost 1.66 REQUIRED COMPONENTS thread atomic)

add_library(locks INTERFACE)
target_include_directories(locks INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(locks INTERFACE Boost::thread Boost::atomic Threads::Threads)

enable_testing()

add_executable(spinlock_tests tests/spinlock_tests.cpp)
target_link_libraries(spinlock_tests PRIVATE locks)
# Keep asserts on in the tests regardless of build type.
target_compile_options(spinlock_tests PRIVATE -UNDEBUG)
add_test(NAME spinlock_tests COMMAND spinlock_tests)

add_executable(lock_benchmark bench/lock_benchmark.cpp)
target_link_libraries(lock_benchmark PRIVATE locks)
