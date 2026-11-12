// Append-only JSON-lines tuning log. Every trial is written before it starts
// (trial_start) and after it ends (trial_result), each line flushed to disk,
// so tuning can resume after a crash or reboot. A trial_start without a
// trial_result means the process died during that trial; it is replayed as
// Unstable.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "gpuoc/clock.hpp"
#include "gpuoc/types.hpp"
#include "stress/verdict.hpp"

namespace gpuoc {

struct TrialRecord {
  int seq = 0;
  std::string stage;
  std::string kind = "stress";  // "stress" | "bench"
  Settings settings;
  Sec duration{0};
  bool completed = false;  // false: started but never finished (crash/reboot)
  VerdictKind verdict = VerdictKind::Inconclusive;
  std::string reason;
  double score = 0;
  double avg_power_w = 0;
  double max_temp_c = 0;
  std::uint32_t limit_flags = 0;
  bool bench_valid = false;
  double elapsed_s = 0;  // wall time the trial took
};

struct TuneSessionInfo {
  std::string session_id;
  std::string gpu_uuid;
  std::string driver_version;
  Objective objective = Objective::MaxPerfPerWatt;
  std::string params_hash;  // tuning-relevant config; a change starts a new session
  WallTime started{};
};

struct ResumeState {
  TuneSessionInfo info;
  std::vector<TrialRecord> trials;  // in seq order
  double elapsed_s = 0;             // time already spent (for the budget)
};

class TuningLog {
 public:
  // read_only (dry-run): nothing is written.
  explicit TuningLog(std::filesystem::path file, bool read_only = false);

  // The most recent unfinished session for this GPU with matching driver and
  // parameters, if any.
  Result<std::optional<ResumeState>> find_resumable(const std::string& gpu_uuid, const std::string& driver_version,
                                                    const std::string& params_hash) const;

  Result<void> begin_session(const TuneSessionInfo& info);
  Result<void> resume_session(const TuneSessionInfo& info);
  Result<void> trial_start(const TrialRecord& t);
  Result<void> trial_result(const TrialRecord& t);
  Result<void> event(const std::string& type, nlohmann::json data = nlohmann::json::object());
  Result<void> end_session(const std::string& reason);

  // Summary for `gpuoc tune status`: last session state.
  Result<nlohmann::json> status_summary() const;

  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
  [[nodiscard]] const std::string& session_id() const noexcept { return session_; }

 private:
  Result<void> write(nlohmann::json j);

  std::filesystem::path file_;
  bool read_only_;
  std::string session_;
};

// Stable hash of the tuning-relevant configuration.
std::string tuning_params_hash(const TuningConfig& t, Objective objective, const SafetyConfig& s);

}  // namespace gpuoc
