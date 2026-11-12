include(GoogleTest)

add_executable(gpuoc_tests
  test_main.cpp
  unit/test_api.cpp
  unit/test_mock.cpp
  unit/test_safety.cpp
  unit/test_config.cpp
  unit/test_stress.cpp
  unit/test_bench.cpp
  unit/test_tuner.cpp
  unit/test_controller.cpp
  unit/test_service.cpp
  unit/test_profiles.cpp
  integration/test_cli.cpp
  unit/test_nvidia.cpp
  hw/test_hardware.cpp
  unit/test_examples.cpp
)
target_link_libraries(gpuoc_tests PRIVATE gpuoc_core GTest::gtest)
target_include_directories(gpuoc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gpuoc_set_warnings(gpuoc_tests)
add_dependencies(gpuoc_tests gpuoc-worker gpuoc gpuocd)
target_compile_definitions(gpuoc_tests PRIVATE
  GPUOC_WORKER_PATH="$<TARGET_FILE:gpuoc-worker>"
  GPUOC_CLI_PATH="$<TARGET_FILE:gpuoc>"
  GPUOC_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  $<$<CONFIG:Release>:GPUOC_OPTIMIZED_BUILD=1>)

gtest_discover_tests(gpuoc_tests
  DISCOVERY_TIMEOUT 120
  PROPERTIES ENVIRONMENT "GPUOC_TEST_MODE=1")
