// Local control channel: \\.\pipe\gpuoc carrying JSON-RPC 2.0, one request
// and one response per connection (message mode).
//
// Pipe DACL (SDDL): D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12018b;;;BU)
//   SYSTEM and Administrators: full access.
//   Users: read + write-data + write-attributes only, so they can send a
//   request and read the reply but cannot create pipe instances (no
//   FILE_CREATE_PIPE_INSTANCE). Remote clients are rejected.
// Write methods are additionally authorized per call: the server checks the
// caller's token for (elevated) Administrators membership.
//
// Non-Windows development hosts use a Unix domain socket instead.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "gpuoc/clock.hpp"
#include "gpuoc/result.hpp"

namespace gpuoc::platform {

inline constexpr const char* kDefaultPipeName = "gpuoc";
inline constexpr const char* kPipeSddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12018b;;;BU)";

struct CallerInfo {
  bool is_admin = false;
  unsigned long pid = 0;
};

class PipeServer {
 public:
  using Handler = std::function<std::string(const std::string& request, const CallerInfo& caller)>;
  // name: "gpuoc" -> \\.\pipe\gpuoc (Windows) or a socket file (other).
  static Result<std::unique_ptr<PipeServer>> start(const std::string& name, Handler handler);
  ~PipeServer();
  void stop();

  struct Impl;

 private:
  explicit PipeServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Send one request and wait for the response.
Result<std::string> pipe_call(const std::string& name, const std::string& request, Ms timeout);

// Full OS path of the pipe / socket (for messages and docs).
std::string pipe_path(const std::string& name);

}  // namespace gpuoc::platform
