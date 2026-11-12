// NVIDIA backend: NVML (nvml.dll) for enumeration, telemetry, power limits
// and fans; NVAPI (nvapi64.dll) for clock offsets and voltage through the
// P-state 2.0 interface. Both libraries are loaded at runtime; a missing
// library or function makes the matching capability unsupported.
#pragma once

#include <memory>

#include <nlohmann/json.hpp>

#include "gpuoc/backend.hpp"

namespace gpuoc {

Result<std::shared_ptr<IGpuBackend>> make_nvidia_backend();

// For `gpuoc doctor`: which libraries loaded (path, file version), the driver
// version, and which NVML/NVAPI functions are available. Never writes.
nlohmann::json nvidia_diagnostics();

}  // namespace gpuoc
