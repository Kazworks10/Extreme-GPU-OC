// Last-known-good (LKG) store: lkg.json. Only validated settings may enter.
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "gpuoc/clock.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc {

struct LkgEntry {
  std::string gpu_uuid;
  std::string gpu_name;
  std::string driver_version;
  Objective objective = Objective::MaxPerfPerWatt;
  Settings settings;
  bool validated = false;           // must be true to be stored
  WallTime validated_at{};
  Sec validation_duration{0};
  double score = 0;
  double baseline_score = 0;
  std::string score_unit;
  double power_w = 0;
  double max_temp_c = 0;
  bool revoked = false;
  std::string revoke_reason;
};

class LkgStore {
 public:
  // read_only: dry-run mode; commits and revocations are logged, not written.
  explicit LkgStore(std::filesystem::path file, bool read_only = false);

  Result<void> load();
  // Rejects unvalidated entries (SafetyRefusal). Replaces any entry for the
  // same GPU and objective.
  Result<void> commit(const LkgEntry& entry);
  Result<void> revoke(const std::string& gpu_uuid, const std::string& reason);

  [[nodiscard]] std::optional<LkgEntry> get(const std::string& gpu_uuid, Objective objective) const;
  // Most recently validated, non-revoked entry for a GPU.
  [[nodiscard]] std::optional<LkgEntry> latest(const std::string& gpu_uuid) const;
  [[nodiscard]] std::vector<LkgEntry> all() const;
  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

 private:
  Result<void> save_locked() const;

  std::filesystem::path file_;
  bool read_only_;
  mutable std::mutex mu_;
  std::vector<LkgEntry> entries_;
};

}  // namespace gpuoc
