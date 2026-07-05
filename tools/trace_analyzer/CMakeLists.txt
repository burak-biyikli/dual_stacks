cmake_minimum_required(VERSION 3.10)
project(trace_analyzer)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point to DynamoRIO's cmake directory
set(DynamoRIO_DIR ${CMAKE_SOURCE_DIR}/../../ext/dynamorio/build/cmake)

find_package(DynamoRIO)
if (NOT DynamoRIO_FOUND)
  message(FATAL_ERROR "DynamoRIO package required to build")
endif()

# Find Google Test
find_package(GTest REQUIRED)
enable_testing()

# Build GTest-based unit test harness
add_executable(test_analyzer tests/test_analyzer.cpp analyzer.cpp)
target_link_libraries(test_analyzer GTest::gtest GTest::gtest_main pthread)
gtest_discover_tests(test_analyzer)

# Build standalone integration stress test (run under DynamoRIO)
add_executable(test_fuzzer tests/test_fuzzer_minimal_stress.cpp)
target_link_libraries(test_fuzzer pthread)

# Build DynamoRIO client
add_library(analyzer SHARED dr_client.cpp analyzer.cpp)
target_compile_definitions(analyzer PRIVATE BUILDING_DR_CLIENT)
configure_DynamoRIO_client(analyzer)
use_DynamoRIO_extension(analyzer drmgr)
use_DynamoRIO_extension(analyzer drreg)
use_DynamoRIO_extension(analyzer drutil)
use_DynamoRIO_extension(analyzer drcontainers)
add_library(empty SHARED empty_client.cpp)
configure_DynamoRIO_client(empty)
