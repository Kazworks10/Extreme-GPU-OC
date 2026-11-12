// When to re-tune: new GPU (UUID), new NVIDIA driver, stale validation, or
// repeated instability recorded by the runtime monitor.
#pragma once

#include <optional>
#include <string>

#include "core/lkg_store.hpp"

namespace gpuoc {

struct RetuneInputs {
  std::optional<LkgEntry> lkg;      // current LKG entry for this GPU and objective
  std::string gpu_uuid;             // the GPU now present
  std::string driver_version;       // the driver now installed
  WallTime now{};
  int revalidate_days = 14;
  bool repeated_instability = false;  // runtime monitor
};

// Returns the reason a re-tune is needed, or nullopt if the LKG is current.
std::optional<std::string> retune_reason(const RetuneInputs& in);

}  // namespace gpuoc
