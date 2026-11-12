// Narrow, fake-able view of NVML. The real implementation loads nvml.dll at
// runtime (LoadLibraryW + GetProcAddress); tests substitute a fake.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpuoc/result.hpp"

namespace gpuoc::nvidia {

struct NvmlDeviceInfo {
  unsigned index = 0;
  std::string uuid;
  std::string name;
  unsigned pci_domain = 0;
  unsigned pci_bus = 0;
  unsigned pci_device = 0;
};

enum class NvmlClock { Graphics, Memory };

class INvmlApi {
 public:
  virtual ~INvmlApi() = default;
  virtual Result<void> init() = 0;
  virtual void shutdown() = 0;
  virtual Result<std::string> driver_version() = 0;
  virtual Result<unsigned> device_count() = 0;
  virtual Result<NvmlDeviceInfo> device_info(unsigned i) = 0;
  virtual Result<unsigned> clock_mhz(unsigned i, NvmlClock c) = 0;
  virtual Result<unsigned> temperature_c(unsigned i) = 0;
  virtual Result<unsigned> power_mw(unsigned i) = 0;
  virtual Result<unsigned> utilization_pct(unsigned i) = 0;
  virtual Result<unsigned> fan_count(unsigned i) = 0;
  virtual Result<unsigned> fan_speed_pct(unsigned i, unsigned fan) = 0;
  virtual Result<std::pair<unsigned, unsigned>> fan_min_max_pct(unsigned i) = 0;
  virtual Result<void> set_fan_speed_pct(unsigned i, unsigned fan, unsigned pct) = 0;
  virtual Result<void> set_fan_default(unsigned i, unsigned fan) = 0;
  // Unsupported error when ECC is not available.
  virtual Result<std::uint64_t> ecc_uncorrected(unsigned i) = 0;
  virtual Result<std::pair<unsigned, unsigned>> power_limit_constraints_mw(unsigned i) = 0;
  virtual Result<unsigned> power_limit_default_mw(unsigned i) = 0;
  virtual Result<unsigned> power_limit_mw(unsigned i) = 0;
  virtual Result<void> set_power_limit_mw(unsigned i, unsigned mw) = 0;
  virtual Result<std::uint64_t> clock_event_reasons(unsigned i) = 0;
  // Whether a named NVML entry point was found (capability checks, doctor).
  [[nodiscard]] virtual bool has(const char* function) const = 0;
  [[nodiscard]] virtual std::string library_path() const { return {}; }
  [[nodiscard]] virtual std::string library_version() const { return {}; }
};

// NVML status code -> Error (pure; tested).
Error nvml_error(int status, const char* function);
// NVML clock-event (throttle) reasons -> gpuoc LimitFlags (pure; tested).
std::uint32_t limit_flags_from_reasons(std::uint64_t reasons) noexcept;

// Load nvml.dll (Windows) / libnvidia-ml.so.1 (other). Unavailable if absent.
Result<std::unique_ptr<INvmlApi>> load_nvml();

}  // namespace gpuoc::nvidia
