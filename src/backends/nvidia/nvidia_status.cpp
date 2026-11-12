// Status-code conversion at the backend boundary (pure functions).
#include <format>
#include <nvml.h>

#include "backends/nvidia/nvapi_api.hpp"
#include "backends/nvidia/nvml_api.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc::nvidia {

Error nvml_error(int status, const char* function) {
  ErrorCode code = ErrorCode::HardwareError;
  const char* what = "error";
  switch (static_cast<nvmlReturn_t>(status)) {
    case NVML_SUCCESS: code = ErrorCode::Internal; what = "success reported as an error"; break;
    case NVML_ERROR_NOT_SUPPORTED: code = ErrorCode::Unsupported; what = "not supported"; break;
    case NVML_ERROR_NO_PERMISSION: code = ErrorCode::PermissionDenied; what = "insufficient permissions"; break;
    case NVML_ERROR_INVALID_ARGUMENT: code = ErrorCode::InvalidArgument; what = "invalid argument"; break;
    case NVML_ERROR_GPU_IS_LOST: code = ErrorCode::GpuLost; what = "GPU is lost"; break;
    case NVML_ERROR_RESET_REQUIRED: code = ErrorCode::GpuLost; what = "GPU reset required"; break;
    case NVML_ERROR_UNINITIALIZED: code = ErrorCode::Internal; what = "NVML not initialized"; break;
    case NVML_ERROR_DRIVER_NOT_LOADED: code = ErrorCode::Unavailable; what = "NVIDIA driver not loaded"; break;
    case NVML_ERROR_LIBRARY_NOT_FOUND: code = ErrorCode::Unavailable; what = "NVML library not found"; break;
    case NVML_ERROR_FUNCTION_NOT_FOUND: code = ErrorCode::Unsupported; what = "function not implemented by this NVML"; break;
    case NVML_ERROR_NOT_FOUND: code = ErrorCode::NotFound; what = "not found"; break;
    case NVML_ERROR_TIMEOUT: code = ErrorCode::Timeout; what = "timeout"; break;
    case NVML_ERROR_INSUFFICIENT_POWER: code = ErrorCode::HardwareError; what = "external power cables not attached"; break;
    case NVML_ERROR_IN_USE: code = ErrorCode::Busy; what = "GPU in use"; break;
    case NVML_ERROR_LIB_RM_VERSION_MISMATCH: code = ErrorCode::Unavailable; what = "driver/library version mismatch"; break;
    case NVML_ERROR_GPU_NOT_FOUND: code = ErrorCode::NotFound; what = "no GPUs found"; break;
    default: break;
  }
  return make_error(code, std::format("{}: {} (NVML status {})", function, what, status));
}

std::uint32_t limit_flags_from_reasons(std::uint64_t r) noexcept {
  std::uint32_t f = kLimitNone;
  if (r & nvmlClocksEventReasonSwPowerCap) f |= kLimitPower;
  if (r & (nvmlClocksEventReasonSwThermalSlowdown | nvmlClocksThrottleReasonHwThermalSlowdown)) f |= kLimitThermal;
  if (r & (nvmlClocksThrottleReasonHwSlowdown | nvmlClocksThrottleReasonHwPowerBrakeSlowdown)) f |= kLimitHwSlowdown;
  return f;
}

Error nvapi_error(int status, const char* function) {
  ErrorCode code = ErrorCode::HardwareError;
  const char* what = "error";
  switch (status) {
    case 0: code = ErrorCode::Internal; what = "success reported as an error"; break;          // NVAPI_OK
    case -2: code = ErrorCode::Unavailable; what = "NVAPI library not found"; break;            // NVAPI_LIBRARY_NOT_FOUND
    case -3: code = ErrorCode::Unsupported; what = "not implemented by this driver"; break;    // NVAPI_NO_IMPLEMENTATION
    case -4: code = ErrorCode::Internal; what = "NVAPI not initialized"; break;                // NVAPI_API_NOT_INITIALIZED
    case -5: code = ErrorCode::InvalidArgument; what = "invalid argument"; break;              // NVAPI_INVALID_ARGUMENT
    case -6: code = ErrorCode::Unavailable; what = "no NVIDIA device"; break;                  // NVAPI_NVIDIA_DEVICE_NOT_FOUND
    case -8: code = ErrorCode::InvalidArgument; what = "invalid handle"; break;                // NVAPI_INVALID_HANDLE
    case -9: code = ErrorCode::Unsupported; what = "incompatible struct version"; break;       // NVAPI_INCOMPATIBLE_STRUCT_VERSION
    case -10: code = ErrorCode::GpuLost; what = "handle invalidated (GPU reconfigured)"; break; // NVAPI_HANDLE_INVALIDATED
    case -104: code = ErrorCode::Unsupported; what = "not supported by this GPU"; break;       // NVAPI_NOT_SUPPORTED
    case -108: code = ErrorCode::Busy; what = "device busy"; break;                            // NVAPI_DEVICE_BUSY
    case -134: code = ErrorCode::InvalidArgument; what = "invalid call"; break;                // NVAPI_INVALID_CALL
    case -137: code = ErrorCode::PermissionDenied; what = "administrator rights required"; break;  // NVAPI_INVALID_USER_PRIVILEGE
    case -158: code = ErrorCode::SafetyRefusal; what = "setting not allowed by the driver"; break;  // NVAPI_SET_NOT_ALLOWED
    case -191: code = ErrorCode::Timeout; what = "timeout"; break;                             // NVAPI_TIMEOUT
    case -220: code = ErrorCode::Unavailable; what = "GPU not powered"; break;                 // NVAPI_GPU_NOT_POWERED
    default: break;
  }
  return make_error(code, std::format("{}: {} (NvAPI status {})", function, what, status));
}

}  // namespace gpuoc::nvidia
