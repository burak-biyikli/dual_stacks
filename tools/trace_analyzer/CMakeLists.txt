cmake_minimum_required(VERSION 3.10)
project(trace_analyzer)

# Point to DynamoRIO's cmake directory
set(DynamoRIO_DIR ${CMAKE_SOURCE_DIR}/../../ext/dynamorio/build/cmake)

find_package(DynamoRIO)
if (NOT DynamoRIO_FOUND)
  message(FATAL_ERROR "DynamoRIO package required to build")
endif()

# Build standalone test harness
add_executable(test_analyzer test_analyzer.c analyzer.c)
target_link_libraries(test_analyzer pthread)

# Build DynamoRIO client
add_library(analyzer SHARED dr_client.cpp analyzer.c)
configure_DynamoRIO_client(analyzer)
use_DynamoRIO_extension(analyzer drmgr)
use_DynamoRIO_extension(analyzer drreg)
use_DynamoRIO_extension(analyzer drutil)
use_DynamoRIO_extension(analyzer drcontainers)
add_library(empty SHARED empty_client.cpp)
configure_DynamoRIO_client(empty)
