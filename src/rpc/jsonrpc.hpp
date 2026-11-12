// JSON-RPC 2.0 over the control pipe.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "gpuoc/result.hpp"
#include "platform/pipe.hpp"

namespace gpuoc::rpc {

using nlohmann::json;

// Standard codes plus application codes (-32001 ...) derived from ErrorCode.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;

int code_for(ErrorCode c) noexcept;
ErrorCode error_code_from(int rpc_code) noexcept;

json make_request(const std::string& method, const json& params, int id = 1);

class Dispatcher {
 public:
  using Method = std::function<Result<json>(const json& params, const platform::CallerInfo& caller)>;
  // write = true: only Administrators (elevated) may call it.
  void add(const std::string& name, bool write, Method m);
  std::string handle(const std::string& request, const platform::CallerInfo& caller) const;
  [[nodiscard]] bool has(const std::string& name) const { return methods_.count(name) != 0; }

 private:
  struct Entry {
    bool write;
    Method fn;
  };
  std::map<std::string, Entry> methods_;
};

// Client: call a method over the pipe. Errors in the response become Error.
Result<json> call(const std::string& pipe_name, const std::string& method, const json& params, Ms timeout);

}  // namespace gpuoc::rpc
