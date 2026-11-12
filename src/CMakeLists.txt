add_library(gpuoc_core STATIC
  # core: device model, safety, LKG
  core/types.cpp
  core/log.cpp
  core/safety.cpp
  core/gpu.cpp
  core/config.cpp
  core/json_codec.cpp
  core/lkg_store.cpp
  core/heartbeat.cpp
  # backends
  backends/mock/mock_backend.cpp
  backends/dry_run_backend.cpp
  backends/nvidia/nvidia_backend.cpp
  # bench
  bench/kernel_reference.cpp
  bench/vulkan_kernel.cpp
  bench/process_workload.cpp
  bench/benchmark.cpp
  # stress
  stress/event_log.cpp
  stress/stress_runner.cpp
  # tuner
  tuner/tuning_log.cpp
  tuner/tuner.cpp
  tuner/retune_policy.cpp
  # controller
  controller/state_store.cpp
  controller/runtime_monitor.cpp
  controller/controller.cpp
  controller/watchdog.cpp
  controller/engine.cpp
  # profiles
  profile/profile.cpp
  profile/profile_store.cpp
  profile/auto_switch.cpp
  # rpc
  rpc/jsonrpc.cpp
  # platform
  platform/fs.cpp
  platform/paths.cpp
  platform/dynlib.cpp
  platform/process.cpp
  platform/service.cpp
  platform/pipe.cpp
)

target_include_directories(gpuoc_core
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gpuoc_core PUBLIC gpuoc_json gpuoc_toml gpuoc_nvml_headers)
if(GPUOC_BUILD_VULKAN)
  target_link_libraries(gpuoc_core PRIVATE gpuoc_vulkan_headers)
  target_compile_definitions(gpuoc_core PRIVATE GPUOC_HAVE_VULKAN=1)
else()
  target_compile_definitions(gpuoc_core PRIVATE GPUOC_HAVE_VULKAN=0)
endif()
gpuoc_set_warnings(gpuoc_core)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(gpuoc_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(WIN32)
  target_link_libraries(gpuoc_core PUBLIC shell32 ole32 advapi32 version)
endif()

# ---- executables ---------------------------------------------------------------
add_executable(gpuoc-worker bench/worker_main.cpp)
target_link_libraries(gpuoc-worker PRIVATE gpuoc_core)
gpuoc_set_warnings(gpuoc-worker)

add_executable(gpuoc cli/main.cpp)
target_link_libraries(gpuoc PRIVATE gpuoc_core)
gpuoc_set_warnings(gpuoc)

add_executable(gpuocd service/main.cpp)
target_link_libraries(gpuocd PRIVATE gpuoc_core)
gpuoc_set_warnings(gpuocd)

if(MINGW)
  # wmain entry points
  target_link_options(gpuoc PRIVATE -municode)
  target_link_options(gpuocd PRIVATE -municode)
endif()
