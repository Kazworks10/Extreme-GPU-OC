// Auto-tuning search.
//
// Stages: 1 baseline, 2 memory offset, 3 core offset, 4 voltage (gated),
// 5 power limit and fan, 6 margin, 7 validation. Offsets are found with an
// exponential ramp (step x 2^n) to the first unstable point, then a binary
// search for the last stable point. Every trial is logged before the next one
// starts; resuming replays the log through the same deterministic algorithm.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "gpuoc/clock.hpp"
#include "gpuoc/gpu.hpp"
#include "tuner/trial_runner.hpp"
#include "tuner/tuning_log.hpp"

namespace gpuoc {

struct TunerOptions {
  TuningConfig tuning;
  Objective objective = Objective::MaxPerfPerWatt;
  Sec budget{4 * 3600};
  Sec bench_estimate{60};  // one benchmark (warm-up + runs), reserved in the budget
};

struct BaselineResult {
  double score = 0;
  double power_w = 0;
  double max_temp_c = 0;
  std::string unit;
  bool bench_valid = false;
};

struct SearchResult {
  int best_stable = 0;                  // last stable offset found
  std::optional<int> lowest_unstable;   // first failing offset (if any)
  bool budget_exhausted = false;
  int suspect_regions = 0;              // non-monotonic regions detected
  int retests = 0;                      // trials re-run because of them
};

struct TuneOutcome {
  bool validated = false;
  Settings settings;           // validated settings (empty if not validated)
  Settings candidate;          // what stage 6 produced
  BaselineResult baseline;
  double final_score = 0;
  double final_power_w = 0;
  double final_max_temp_c = 0;
  std::string unit;
  int mem_found_mhz = 0;
  int core_found_mhz = 0;
  std::optional<int> voltage_mv;
  int trials = 0;
  int replayed_trials = 0;
  bool resumed = false;
  bool budget_exhausted = false;
  int suspect_regions = 0;
  int retests = 0;
  double elapsed_s = 0;
  Sec validation_duration{0};
  std::vector<std::string> notes;
};

// Metric the objective maximizes, from a score and an average power.
double objective_metric(Objective o, double score, double power_w);

class Tuner {
 public:
  Tuner(Gpu& gpu, ITrialRunner& runner, TuningLog& log, IClock& clock, TunerOptions opts);

  // Start a new session or resume the last unfinished one from the log.
  Result<void> begin();
  Result<BaselineResult> baseline(const std::atomic<bool>* abort);
  Result<Settings> tune(const std::atomic<bool>* abort);     // stages 2-6
  Result<bool> validate(const std::atomic<bool>* abort);     // stage 7
  void finish(const std::string& reason);
  // Convenience: begin + all stages + finish.
  Result<TuneOutcome> run(const std::atomic<bool>* abort);

  [[nodiscard]] const TuneOutcome& outcome() const noexcept { return out_; }
  [[nodiscard]] double elapsed_s() const;
  [[nodiscard]] double budget_left_s() const;

  // Generic offset search, exposed for tests.
  struct Space {
    std::string stage;
    int step = 15;
    int lo = 0;     // known-stable starting point
    int max = 0;    // highest value that may be tested
    Sec duration{120};
    std::function<Settings(int)> settings_for;
  };
  Result<SearchResult> search(const Space& space, const std::atomic<bool>* abort);

 private:
  Result<TrialResult> trial(const std::string& stage, const Settings& s, Sec duration, const std::atomic<bool>* abort);
  Result<TrialResult> bench(const std::string& stage, const Settings& s, const std::atomic<bool>* abort);
  Result<TrialResult> run_logged(const std::string& stage, const std::string& kind, const Settings& s, Sec duration,
                                 const std::atomic<bool>* abort);
  Result<bool> decisive(const std::string& stage, const Settings& s, Sec duration, const std::atomic<bool>* abort);
  [[nodiscard]] bool budget_allows(Sec next, Sec reserve = Sec(0)) const;
  Settings base_settings() const;

  Result<void> stage_voltage(const std::atomic<bool>* abort);
  Result<void> stage_power_fan(const std::atomic<bool>* abort);

  Gpu& gpu_;
  ITrialRunner& runner_;
  TuningLog& log_;
  IClock& clock_;
  TunerOptions opts_;
  Capabilities caps_;
  EffectiveLimits lim_;
  std::string driver_;

  // Replay state.
  std::vector<TrialRecord> replay_;
  std::size_t replay_pos_ = 0;
  double replayed_elapsed_s_ = 0;
  SteadyTime live_start_{};
  int seq_ = 0;

  // Working state.
  int mem_ = 0, core_ = 0;
  std::optional<int> core_fail_;
  std::optional<int> voltage_;
  std::optional<int> power_;
  std::optional<FanCurve> fan_;
  double last_stress_max_temp_ = 0;
  std::uint32_t last_stress_flags_ = 0;
  TuneOutcome out_;
};

}  // namespace gpuoc
