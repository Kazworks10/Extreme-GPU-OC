#include "backends/nvidia/nvidia_backend.hpp"

namespace gpuoc {

// Placeholder until the NVML/NVAPI backend lands (build step 7).
Result<std::shared_ptr<IGpuBackend>> make_nvidia_backend() {
  return make_error(ErrorCode::Unavailable, "NVIDIA backend not built yet");
}

nlohmann::json nvidia_diagnostics() { return nlohmann::json{{"nvml", {{"loaded", false}}}, {"nvapi", {{"loaded", false}}}}; }

}  // namespace gpuoc
