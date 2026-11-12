// Process exit codes shared by gpuoc.exe and gpuocd.exe.
#pragma once

#include "gpuoc/result.hpp"

namespace gpuoc::cli {

enum ExitCode : int {
  kExitOk = 0,
  kExitUsage = 1,
  kExitUnsupported = 2,
  kExitSafety = 3,
  kExitHardware = 4,
  kExitPrivileges = 5,
};

inline int exit_code_for(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::ParseError:
    case ErrorCode::NotFound:
      return kExitUsage;
    case ErrorCode::Unsupported:
    case ErrorCode::Unavailable:
      return kExitUnsupported;
    case ErrorCode::SafetyRefusal:
    case ErrorCode::OutOfRange:
      return kExitSafety;
    case ErrorCode::PermissionDenied:
      return kExitPrivileges;
    case ErrorCode::HardwareError:
    case ErrorCode::GpuLost:
    case ErrorCode::VerifyFailed:
    case ErrorCode::Timeout:
    case ErrorCode::IoError:
    case ErrorCode::Busy:
    case ErrorCode::Aborted:
    case ErrorCode::Internal:
      return kExitHardware;
  }
  return kExitHardware;
}

}  // namespace gpuoc::cli
