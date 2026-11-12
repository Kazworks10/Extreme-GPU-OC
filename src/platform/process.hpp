// Child processes with captured stdout. On Windows every child runs inside a
// job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so it dies if gpuoc
// exits for any reason.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gpuoc/clock.hpp"
#include "gpuoc/result.hpp"

namespace gpuoc::platform {

struct ExitInfo {
  int code = 0;           // exit code (Windows: DWORD reinterpreted)
  bool crashed = false;   // Windows: NTSTATUS error code; POSIX: killed by a signal
  bool killed = false;    // we terminated it
  std::string description;
};

class ChildProcess {
 public:
  // argv[0] is the executable path; arguments are quoted for the platform.
  static Result<std::unique_ptr<ChildProcess>> spawn(const std::vector<std::string>& argv);
  // A raw command line (Windows: passed to CreateProcessW as-is; POSIX: /bin/sh -c).
  static Result<std::unique_ptr<ChildProcess>> spawn_command_line(const std::string& command_line);

  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // Next complete stdout line (without the newline). nullopt at EOF.
  // ErrorCode::Timeout if no line arrives in time.
  Result<std::optional<std::string>> read_line(Ms timeout);
  // Wait for exit. nullopt if still running after the timeout.
  std::optional<ExitInfo> wait(Ms timeout);
  void kill();
  [[nodiscard]] bool running();

  struct Impl;

 private:
  explicit ChildProcess(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Quote one argument following the MSVC runtime / CommandLineToArgvW rules.
std::string quote_windows_arg(const std::string& arg);
std::string build_windows_command_line(const std::vector<std::string>& argv);

// Run to completion and capture stdout (external benchmarks). The child is
// killed on timeout.
struct CaptureResult {
  ExitInfo exit;
  std::string output;
  bool timed_out = false;
};
Result<CaptureResult> run_capture(const std::string& command_line, Ms timeout);

}  // namespace gpuoc::platform
