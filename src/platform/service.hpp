// Windows service plumbing. ServiceLifecycle is the portable, testable core:
// stop and preshutdown both run the stop hook (stock restore) synchronously
// and only then report Stopped.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

#include "gpuoc/clock.hpp"
#include "gpuoc/result.hpp"

namespace gpuoc::platform {

enum class ServiceStatus { StartPending, Running, StopPending, Stopped };
enum class ServiceControl { Stop, Preshutdown, Shutdown, Interrogate };

const char* to_string(ServiceStatus s) noexcept;

class IStatusReporter {
 public:
  virtual ~IStatusReporter() = default;
  virtual void report(ServiceStatus status, int exit_code, Ms wait_hint) = 0;
};

class ServiceLifecycle {
 public:
  struct Hooks {
    std::function<Result<void>()> start;                            // e.g. Engine::startup
    std::function<void(const std::atomic<bool>& stop)> run;         // returns when stop is set
    std::function<void(const std::string& reason)> stop;            // synchronous stock restore
  };

  ServiceLifecycle(IStatusReporter& reporter, Hooks hooks);

  // Blocking. Returns the service exit code.
  int run();
  // Called from the SCM handler thread (or a console handler).
  void control(ServiceControl c);
  [[nodiscard]] const std::atomic<bool>& stop_flag() const noexcept { return stop_; }

 private:
  IStatusReporter& reporter_;
  Hooks hooks_;
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  std::string reason_ = "stop";
  ServiceStatus current_ = ServiceStatus::StartPending;
};

inline constexpr const wchar_t* kServiceName = L"ExtremeGpuOc";
inline constexpr const wchar_t* kServiceDisplayName = L"Extreme GPU OC";

// SCM integration (Windows only; other platforms return Unsupported).
int run_as_service(ServiceLifecycle::Hooks hooks);   // StartServiceCtrlDispatcherW
Result<void> install_service(const std::string& exe_path, const std::string& args);
Result<void> uninstall_service();
Result<void> start_service();
Result<void> stop_service();
Result<std::string> query_service();  // "running", "stopped", "not installed", ...

// Console Ctrl+C / Ctrl+Break / close / logoff / shutdown handling. The
// callback runs synchronously in the handler (it must restore stock before
// returning, because the process may be terminated right after).
void install_console_handler(std::function<void(const char* event)> on_event);

bool is_elevated();

}  // namespace gpuoc::platform
