// The backend interface. Implementations: NVIDIA (NVML + NVAPI), mock, dry-run.
#pragma once

#include <vector>

#include "gpuoc/result.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc {

class IGpuBackend {
 public:
  virtual Result<std::vector<GpuId>> enumerate() = 0;
  virtual Result<Capabilities> capabilities(const GpuId&) = 0;
  virtual Result<Telemetry> read_telemetry(const GpuId&) = 0;
  virtual Result<Settings> read_settings(const GpuId&) = 0;
  virtual Result<void> apply(const GpuId&, const Settings&) = 0;
  virtual Result<void> reset_to_stock(const GpuId&) = 0;
  virtual ~IGpuBackend() = default;

  // Short backend name for logs ("nvidia", "mock", "dry-run(nvidia)").
  [[nodiscard]] virtual const char* name() const noexcept = 0;
};

}  // namespace gpuoc
