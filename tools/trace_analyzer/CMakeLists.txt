cmake_minimum_required(VERSION 3.10)
project(trace_analyzer)

# Point to DynamoRIO's cmake directory
set(DynamoRIO_DIR ${CMAKE_SOURCE_DIR}/../../ext/dynamorio/build/cmake)

find_package(DynamoRIO)
if (NOT DynamoRIO_FOUND)
  message(FATAL_ERROR "DynamoRIO package required to build")
endif()

add_library(analyzer SHARED analyzer.cpp)
configure_DynamoRIO_client(analyzer)
