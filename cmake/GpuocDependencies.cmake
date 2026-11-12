# All third-party dependencies are pinned to an exact commit or a SHA-256 hash.
include(FetchContent)

# GitHub-hosted dependencies are fetched by commit hash (not by tag or archive)
# so the pin cannot move.

# --- TOML parser: toml++ v3.4.0 (header-only) ---------------------------------
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        30172438cee64926dc41fdd9c11fb3ba5b2ba9de) # v3.4.0

# --- JSON library: nlohmann/json v3.11.3 (header-only) ------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03) # v3.11.3

# --- NVML header: NVIDIA CUDA redistributable cuda_nvml_dev 12.6.77 -----------
# Official NVIDIA source. Only nvml.h is used; nvml.lib is never linked
# (nvml.dll is loaded at runtime).
FetchContent_Declare(nvml_headers
  URL      https://developer.download.nvidia.com/compute/cuda/redist/cuda_nvml_dev/windows-x86_64/cuda_nvml_dev-windows-x86_64-12.6.77-archive.zip
  URL_HASH SHA256=45f2466ff154a44701388a90913b7d1c4c0ec591f0ec33b3d30464d5c2ffbe8d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

set(_gpuoc_deps tomlplusplus nlohmann_json nvml_headers)

# --- NVAPI headers: github.com/NVIDIA/nvapi (official) ------------------------
# Only needed for the Windows NVAPI adapter. nvapi64.lib is never linked
# (nvapi64.dll is loaded at runtime).
if(WIN32)
  FetchContent_Declare(nvapi_headers
    GIT_REPOSITORY https://github.com/NVIDIA/nvapi.git
    GIT_TAG        51e5aae4bc40c3ccd3fb7d4dfd94256e7bfc496e)
  list(APPEND _gpuoc_deps nvapi_headers)
endif()

# --- Vulkan headers (built-in compute workload) -------------------------------
if(GPUOC_BUILD_VULKAN)
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        217e93c664ec6704ec2d8c36fa116c1a4a1e2d40) # v1.3.275
  list(APPEND _gpuoc_deps vulkan_headers)
endif()

# --- GoogleTest v1.15.2 -------------------------------------------------------
if(GPUOC_BUILD_TESTS)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59) # v1.15.2
  list(APPEND _gpuoc_deps googletest)
endif()

FetchContent_MakeAvailable(${_gpuoc_deps})

# Interface targets for the header-only NVIDIA SDK pieces. Marked SYSTEM so
# vendor headers do not trip /W4 /WX.
add_library(gpuoc_nvml_headers INTERFACE)
target_include_directories(gpuoc_nvml_headers SYSTEM INTERFACE "${nvml_headers_SOURCE_DIR}/include")

if(WIN32)
  add_library(gpuoc_nvapi_headers INTERFACE)
  target_include_directories(gpuoc_nvapi_headers SYSTEM INTERFACE "${nvapi_headers_SOURCE_DIR}")
endif()

if(GPUOC_BUILD_VULKAN)
  add_library(gpuoc_vulkan_headers INTERFACE)
  target_include_directories(gpuoc_vulkan_headers SYSTEM INTERFACE "${vulkan_headers_SOURCE_DIR}/include")
endif()

add_library(gpuoc_toml INTERFACE)
target_include_directories(gpuoc_toml SYSTEM INTERFACE "${tomlplusplus_SOURCE_DIR}/include")
target_compile_definitions(gpuoc_toml INTERFACE TOML_EXCEPTIONS=0)

add_library(gpuoc_json INTERFACE)
target_include_directories(gpuoc_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/include")
