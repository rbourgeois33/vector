cmake_minimum_required(VERSION 3.20)
project(vector_exercise LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2)
FetchContent_MakeAvailable(googletest)

set(BENCHMARK_ENABLE_TESTING OFF)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.9.0)
FetchContent_MakeAvailable(benchmark)

add_executable(tests tests.cpp)
target_include_directories(tests PRIVATE include)
target_link_libraries(tests PRIVATE GTest::gtest_main benchmark::benchmark)

enable_testing()
add_test(NAME tests COMMAND tests)