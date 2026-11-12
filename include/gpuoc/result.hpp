// Result<T>: a value or an Error. Nothing in the public API throws.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace gpuoc {

enum class ErrorCode {
  InvalidArgument,   // bad input from the caller (usage error)
  Unsupported,       // the GPU, driver, or platform does not support this
  OutOfRange,        // value outside the driver range or effective limit
  SafetyRefusal,     // refused by a safety gate or limit
  HardwareError,     // the driver or GPU reported a failure
  GpuLost,           // the GPU fell off the bus / driver reset
  VerifyFailed,      // read-back after a write did not match the request
  NotFound,          // no such GPU, profile, file, ...
  PermissionDenied,  // insufficient privileges
  IoError,           // file system or pipe I/O failure
  ParseError,        // malformed TOML, JSON, or command output
  Timeout,           // operation exceeded its deadline
  Aborted,           // cancelled by the user or by shutdown
  Unavailable,       // a library (nvml.dll, nvapi64.dll, vulkan-1.dll) is missing
  Busy,              // another operation is already running
  Internal,          // a bug
};

const char* to_string(ErrorCode code) noexcept;

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;

  [[nodiscard]] std::string to_string() const;
};

inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

namespace detail {
[[noreturn]] inline void bad_result_access(const Error& e) {
  std::fprintf(stderr, "gpuoc: fatal: value() called on an error Result: %s\n", e.message.c_str());
  std::abort();
}
}  // namespace detail

template <class T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is not allowed");

 public:
  Result(const T& value) : v_(value) {}                                    // NOLINT(google-explicit-constructor)
  Result(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)      // NOLINT(google-explicit-constructor)
      : v_(std::move(value)) {}
  Result(Error error) : v_(std::move(error)) {}                            // NOLINT(google-explicit-constructor)

  template <class U, class = std::enable_if_t<std::is_constructible_v<T, U&&> &&
                                              !std::is_same_v<std::decay_t<U>, T> &&
                                              !std::is_same_v<std::decay_t<U>, Error> &&
                                              !std::is_same_v<std::decay_t<U>, Result<T>>>>
  Result(U&& value) : v_(T(std::forward<U>(value))) {}                     // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & {
    if (!ok()) detail::bad_result_access(std::get<1>(v_));
    return std::get<0>(v_);
  }
  [[nodiscard]] const T& value() const& {
    if (!ok()) detail::bad_result_access(std::get<1>(v_));
    return std::get<0>(v_);
  }
  [[nodiscard]] T&& value() && {
    if (!ok()) detail::bad_result_access(std::get<1>(v_));
    return std::get<0>(std::move(v_));
  }
  [[nodiscard]] T value_or(T fallback) const& { return ok() ? std::get<0>(v_) : std::move(fallback); }

  [[nodiscard]] const Error& error() const& {
    if (ok()) {
      static const Error none{ErrorCode::Internal, "error() called on a successful Result"};
      return none;
    }
    return std::get<1>(v_);
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

 private:
  std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : e_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return !e_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const& {
    if (ok()) {
      static const Error none{ErrorCode::Internal, "error() called on a successful Result"};
      return none;
    }
    return *e_;
  }

 private:
  std::optional<Error> e_;
};

inline Result<void> success() { return {}; }

}  // namespace gpuoc

// Propagate an error from an expression returning Result<...>.
#define GPUOC_CONCAT_INNER(a, b) a##b
#define GPUOC_CONCAT(a, b) GPUOC_CONCAT_INNER(a, b)

#define GPUOC_TRY(expr)                                  \
  do {                                                   \
    auto&& gpuoc_try_result_ = (expr);                   \
    if (!gpuoc_try_result_) return gpuoc_try_result_.error(); \
  } while (0)

#define GPUOC_TRY_ASSIGN_IMPL(tmp, decl, expr) \
  auto tmp = (expr);                           \
  if (!tmp) return tmp.error();                \
  decl = std::move(tmp).value()

#define GPUOC_TRY_ASSIGN(decl, expr) \
  GPUOC_TRY_ASSIGN_IMPL(GPUOC_CONCAT(gpuoc_try_tmp_, __LINE__), decl, expr)
