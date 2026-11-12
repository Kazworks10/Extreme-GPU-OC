// Runtime loading of vendor libraries (nvml.dll, nvapi64.dll, vulkan-1.dll).
// On Windows only System32 and explicit absolute paths are searched, so a
// planted DLL in the working directory is never picked up.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gpuoc/result.hpp"

namespace gpuoc::platform {

class DynLib {
 public:
  // Try each candidate in order. Bare names are looked up in System32 only
  // (Windows) or via the dynamic loader (other platforms).
  static Result<std::shared_ptr<DynLib>> open(const std::vector<std::string>& candidates);
  ~DynLib();
  DynLib(const DynLib&) = delete;
  DynLib& operator=(const DynLib&) = delete;

  [[nodiscard]] void* symbol(const char* name) const;
  template <class Fn>
  [[nodiscard]] Fn get(const char* name) const {
    return reinterpret_cast<Fn>(symbol(name));
  }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  // File version from the PE version resource (Windows), empty otherwise.
  [[nodiscard]] std::string file_version() const;

 private:
  DynLib(void* handle, std::string path) : handle_(handle), path_(std::move(path)) {}
  void* handle_;
  std::string path_;
};

// Directory containing the running executable.
std::string current_exe_dir();

}  // namespace gpuoc::platform
