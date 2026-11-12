// NvidiaBackend internals, exposed for unit tests with fake NVML/NVAPI.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "backends/nvidia/nvapi_api.hpp"
#include "backends/nvidia/nvml_api.hpp"
#include "gpuoc/backend.hpp"

namespace gpuoc::nvidia {

// Facts gathered from NVML for one device.
struct NvmlFacts {
  bool power_ok = false;
  unsigned power_min_mw = 0, power_max_mw = 0, power_default_mw = 0;
  bool fans_ok = false;  // count > 0, min/max known, set + default functions present
  unsigned fan_count = 0;
  unsigned fan_min_pct = 0, fan_max_pct = 100;
  bool ecc_ok = false;
  bool locked_clocks_fn = false;
  std::string driver_version;
};

// Pure capability derivation (tested).
Capabilities derive_capabilities(const NvmlFacts& f, const std::optional<Pstates20>& p, bool can_set_pstates);

class NvidiaBackend final : public IGpuBackend {
 public:
  NvidiaBackend(std::unique_ptr<INvmlApi> nvml, std::unique_ptr<INvapiApi> nvapi);
  // NVML init, enumeration, NVML <-> NVAPI mapping by PCI bus.
  Result<void> init();

  Result<std::vector<GpuId>> enumerate() override;
  Result<Capabilities> capabilities(const GpuId&) override;
  Result<Telemetry> read_telemetry(const GpuId&) override;
  Result<Settings> read_settings(const GpuId&) override;
  Result<void> apply(const GpuId&, const Settings&) override;
  Result<void> reset_to_stock(const GpuId&) override;
  [[nodiscard]] const char* name() const noexcept override { return "nvidia"; }

  [[nodiscard]] INvmlApi& nvml() { return *nvml_; }
  [[nodiscard]] INvapiApi* nvapi() { return nvapi_.get(); }

 private:
  struct Device {
    NvmlDeviceInfo info;
    std::optional<int> nvapi;  // NVAPI handle index, if mapped
    NvmlFacts facts;
    FanCurve fan;              // software curve; empty = driver automatic
  };
  Result<Device*> find(const GpuId& id);
  NvmlFacts gather(unsigned index);
  std::optional<Pstates20> pstates(const Device& d);
  Result<void> set_fans(Device& d, const FanCurve& curve);

  std::unique_ptr<INvmlApi> nvml_;
  std::unique_ptr<INvapiApi> nvapi_;
  std::string driver_;
  std::vector<Device> devices_;
  std::recursive_mutex mu_;
};

}  // namespace gpuoc::nvidia
