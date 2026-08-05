cmake_minimum_required(VERSION 3.24)

project(xdec
  VERSION 0.1.0
  DESCRIPTION "A general-purpose, self-contained, multi-level IL decompiler"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep all runnable artifacts in one place so the CLI can find plugins next to it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(XdecOptions)
include(XdecWarnings)

add_subdirectory(src)

if(XDEC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "xdec ${PROJECT_VERSION} configured")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests      : ${XDEC_BUILD_TESTS}")
message(STATUS "  sanitizers : ${XDEC_SANITIZERS}")
