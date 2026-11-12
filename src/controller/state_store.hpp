// Persistent per-GPU runtime state (state.json): violations, cooldowns,
// disablement, and tuning failures. Survives restarts so "disabled until
// manually re-enabled" holds across reboots.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gpuoc/clock.hpp"
#include "gpuoc/result.hpp"

namespace gpuoc {

struct ViolationRecord {
  WallTime time{};
  std::string kind;  // "temperature", "power", "driver_event", "ecc", "gpu_lost", "telemetry"
  std::string reason;
  [[nodiscard]] bool is_instability() const {
    return kind == "driver_event" || kind == "ecc" || kind == "gpu_lost";
  }
};

struct GpuRuntimeState {
  std::vector<ViolationRecord> violations;
  std::optional<WallTime> cooldown_until;
  bool disabled = false;  // until manually re-enabled
  std::string disabled_reason;
  std::optional<WallTime> tune_failed_at;
  std::string tune_failure;

  [[nodiscard]] int violations_since(WallTime since) const;
  [[nodiscard]] int instability_since(WallTime since) const;
};

class StateStore {
 public:
  explicit StateStore(std::filesystem::path file, bool read_only = false);
  Result<void> load();
  [[nodiscard]] GpuRuntimeState get(const std::string& uuid) const;
  Result<void> update(const std::string& uuid, const std::function<void(GpuRuntimeState&)>& fn);
  // Global values (e.g. "active_profile").
  [[nodiscard]] std::string get_global(const std::string& key) const;
  Result<void> set_global(const std::string& key, const std::string& value);

 private:
  Result<void> save_locked() const;
  std::filesystem::path file_;
  bool read_only_;
  mutable std::mutex mu_;
  std::map<std::string, GpuRuntimeState> gpus_;
  std::map<std::string, std::string> globals_;
};

}  // namespace gpuoc
