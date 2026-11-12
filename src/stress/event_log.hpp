// Windows Event Log source for NVIDIA driver failures (best effort).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gpuoc/clock.hpp"
#include "gpuoc/result.hpp"

namespace gpuoc {

struct DriverEvent {
  WallTime time{};
  std::string provider;  // "nvlddmkm", "Display"
  int event_id = 0;
  std::string message;
};

// Event IDs treated as NVIDIA driver failures. Best effort: verify these on
// the target system (docs/WINDOWS.md describes how).
struct DriverEventIds {
  // System log, provider "nvlddmkm".
  static constexpr int kNvlddmkmIds[] = {13, 14, 153};
  // System log, provider "Display": "Display driver nvlddmkm stopped
  // responding and has successfully recovered" (TDR).
  static constexpr int kDisplayTdrId = 4101;
};

bool is_driver_failure(const DriverEvent& e) noexcept;

class IEventLogSource {
 public:
  virtual ~IEventLogSource() = default;
  [[nodiscard]] virtual bool available() const = 0;
  virtual Result<std::vector<DriverEvent>> driver_events_since(WallTime since) = 0;
};

// The Windows implementation (wevtapi). On other platforms: unavailable.
std::unique_ptr<IEventLogSource> make_system_event_log();

// Build the XPath query used against the System log (exposed for tests).
std::string build_driver_event_query(WallTime since);

}  // namespace gpuoc
