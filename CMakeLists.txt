cmake_minimum_required(VERSION 3.20)

project(ExtremeGpuOc
  VERSION 1.0.0
  DESCRIPTION "Autonomous NVIDIA GPU auto-overclocker for Windows"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GPUOC_BUILD_TESTS "Build the GoogleTest suite" ON)
option(GPUOC_WERROR "Treat warnings as errors" ON)
option(GPUOC_BUILD_VULKAN "Build the Vulkan compute workload" ON)

if(NOT WIN32)
  message(STATUS "gpuoc: non-Windows host build (development only; see docs/DECISIONS.md D-001)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GpuocDependencies)
include(GpuocWarnings)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_subdirectory(src)

if(GPUOC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

foreach(_t gpuoc gpuocd gpuoc-worker)
  if(TARGET ${_t})
    install(TARGETS ${_t} RUNTIME DESTINATION bin)
  endif()
endforeach()
install(DIRECTORY examples/ DESTINATION examples)
install(DIRECTORY docs/ DESTINATION docs)
