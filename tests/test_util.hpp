#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "backends/mock/mock_backend.hpp"
#include "gpuoc/api.hpp"
#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace gpuoc::test {

struct MockRig {
  std::shared_ptr<SimClock> clock;
  std::shared_ptr<mock::MockBackend> backend;
  std::shared_ptr<const SafetyPolicy> policy;
  GpuId id;
  std::unique_ptr<Gpu> gpu;
};

inline SafetyConfig permissive_safety() {
  SafetyConfig c;
  c.acknowledge_risk = true;
  c.allow_voltage = true;
  c.allow_power_above_stock = true;
  return c;
}

inline MockRig make_rig(std::uint64_t seed = 1, SafetyConfig safety = {}, mock::MockGpuSpec spec = {},
                        std::string state_file = {}) {
  MockRig r;
  r.clock = std::make_shared<SimClock>();
  mock::MockOptions o;
  o.seed = seed;
  o.clock = r.clock;
  o.specs = {spec};
  o.state_file = std::move(state_file);
  r.backend = std::make_shared<mock::MockBackend>(o);
  r.policy = std::make_shared<SafetyPolicy>(safety);
  r.id = r.backend->enumerate().value().at(0);
  r.gpu = std::make_unique<Gpu>(r.backend, r.id, r.policy);
  return r;
}

// Wine reports unhandled-exception exits differently from Windows (no
// NTSTATUS exit code), so a few process tests relax their expectations there.
inline bool running_under_wine() {
#ifdef _WIN32
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
  return false;
#endif
}

// A unique, empty temporary directory removed on destruction.
class TempDir {
 public:
  TempDir() {
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() / ("gpuoc-test-" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::string str(const std::string& leaf = {}) const {
    return platform::path_utf8(leaf.empty() ? path_ : path_ / platform::u8path(leaf));
  }

 private:
  std::filesystem::path path_;
};

// A directory shared between a death test and its child process. gtest's
// "threadsafe" death tests re-run the test body in a fresh process, so the
// path travels through an environment variable the child inherits.
class SharedDeathDir {
 public:
  explicit SharedDeathDir(const char* env_name) : env_(env_name) {
    const std::string v = platform::getenv_utf8(env_name);
    if (v.empty()) {
      tmp_ = std::make_unique<TempDir>();
      path_ = tmp_->path();
      platform::setenv_utf8(env_name, platform::path_utf8(path_));
    } else {
      path_ = platform::u8path(v);  // child process: reuse the parent's directory
    }
  }
  ~SharedDeathDir() {
    if (tmp_) platform::setenv_utf8(env_, "");
  }
  SharedDeathDir(const SharedDeathDir&) = delete;
  SharedDeathDir& operator=(const SharedDeathDir&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  const char* env_;
  std::unique_ptr<TempDir> tmp_;
  std::filesystem::path path_;
};

// Set an environment variable for the scope of a test.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const std::string& value) : name_(name), old_(platform::getenv_utf8(name)) {
    platform::setenv_utf8(name, value);
  }
  ~ScopedEnv() { platform::setenv_utf8(name_, old_); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* name_;
  std::string old_;
};

}  // namespace gpuoc::test
