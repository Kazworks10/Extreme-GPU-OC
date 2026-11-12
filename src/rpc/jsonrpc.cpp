#include "rpc/jsonrpc.hpp"

#include <format>

#include "core/log.hpp"

namespace gpuoc::rpc {

int code_for(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::InvalidArgument: return -32001;
    case ErrorCode::Unsupported: return -32002;
    case ErrorCode::SafetyRefusal: return -32003;
    case ErrorCode::HardwareError: return -32004;
    case ErrorCode::PermissionDenied: return -32005;
    case ErrorCode::NotFound: return -32006;
    case ErrorCode::Busy: return -32007;
    case ErrorCode::OutOfRange: return -32008;
    case ErrorCode::VerifyFailed: return -32009;
    case ErrorCode::GpuLost: return -32010;
    case ErrorCode::Timeout: return -32011;
    case ErrorCode::Aborted: return -32012;
    case ErrorCode::Unavailable: return -32013;
    case ErrorCode::ParseError: return -32014;
    case ErrorCode::IoError: return -32015;
    case ErrorCode::Internal: return kInternalError;
  }
  return kInternalError;
}

ErrorCode error_code_from(int rpc_code) noexcept {
  switch (rpc_code) {
    case -32001: return ErrorCode::InvalidArgument;
    case -32002: return ErrorCode::Unsupported;
    case -32003: return ErrorCode::SafetyRefusal;
    case -32004: return ErrorCode::HardwareError;
    case -32005: return ErrorCode::PermissionDenied;
    case -32006: return ErrorCode::NotFound;
    case -32007: return ErrorCode::Busy;
    case -32008: return ErrorCode::OutOfRange;
    case -32009: return ErrorCode::VerifyFailed;
    case -32010: return ErrorCode::GpuLost;
    case -32011: return ErrorCode::Timeout;
    case -32012: return ErrorCode::Aborted;
    case -32013: return ErrorCode::Unavailable;
    case -32014: return ErrorCode::ParseError;
    case -32015: return ErrorCode::IoError;
    case kInvalidParams: return ErrorCode::InvalidArgument;
    case kMethodNotFound: return ErrorCode::Unsupported;
    default: return ErrorCode::Internal;
  }
}

json make_request(const std::string& method, const json& params, int id) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

void Dispatcher::add(const std::string& name, bool write, Method m) { methods_[name] = Entry{write, std::move(m)}; }

std::string Dispatcher::handle(const std::string& request, const platform::CallerInfo& caller) const {
  auto error = [](const json& id, int code, const std::string& msg) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", json{{"code", code}, {"message", msg}}}}.dump();
  };
  json req = json::parse(request, nullptr, false);
  if (req.is_discarded()) return error(nullptr, kParseError, "parse error");
  if (!req.is_object() || req.value("jsonrpc", std::string()) != "2.0" || !req.contains("method") ||
      !req["method"].is_string())
    return error(req.is_object() && req.contains("id") ? req["id"] : json(nullptr), kInvalidRequest, "invalid request");
  const json id = req.contains("id") ? req["id"] : json(nullptr);
  const std::string method = req["method"].get<std::string>();
  auto it = methods_.find(method);
  if (it == methods_.end()) return error(id, kMethodNotFound, "method not found: " + method);
  if (it->second.write && !caller.is_admin) {
    log::warn("rpc: refused write method '{}' from non-administrator pid {}", method, caller.pid);
    return error(id, code_for(ErrorCode::PermissionDenied),
                 "'" + method + "' changes GPU settings and requires an elevated Administrator");
  }
  const json params = req.contains("params") ? req["params"] : json::object();
  if (!params.is_object() && !params.is_null()) return error(id, kInvalidParams, "params must be an object");
  Result<json> r = [&]() -> Result<json> {
    try {
      return it->second.fn(params.is_null() ? json::object() : params, caller);
    } catch (const std::exception& e) {
      return make_error(ErrorCode::Internal, std::string("internal error: ") + e.what());
    }
  }();
  if (!r) return error(id, code_for(r.error().code), r.error().message);
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", r.value()}}.dump();
}

Result<json> call(const std::string& pipe_name, const std::string& method, const json& params, Ms timeout) {
  GPUOC_TRY_ASSIGN(std::string resp, platform::pipe_call(pipe_name, make_request(method, params).dump(), timeout));
  json j = json::parse(resp, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return make_error(ErrorCode::ParseError, "malformed response from the service");
  if (j.contains("error")) {
    const json& e = j["error"];
    return make_error(error_code_from(e.value("code", kInternalError)), e.value("message", std::string("error")));
  }
  if (!j.contains("result")) return make_error(ErrorCode::ParseError, "response without result");
  return j["result"];
}

}  // namespace gpuoc::rpc
