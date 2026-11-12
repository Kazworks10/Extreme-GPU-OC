// gpuoc.toml: program configuration.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "gpuoc/clock.hpp"
#include "gpuoc/result.hpp"
#include "gpuoc/safety.hpp"

namespace gpuoc {

enum class Objective { MaxPerformance, MaxPerfPerWatt, Balanced };

const char* to_string(Objective o) noexcept;
Result<Objective> parse_objective(std::string_view s);

struct AutotuneConfig {
  bool enabled = false;
  Objective objective = Objective::MaxPerfPerWatt;
  Sec budget{4 * 3600};
  int revalidate_days = 14;
};

struct TuningConfig {
  int core_step_mhz = 15;
  int mem_step_mhz = 50;
  int margin_steps = 1;
  Sec short_stress{120};
  Sec long_stress{1800};
  int validation_retries = 3;
  bool boundary_lookahead = true;  // one extra trial above the boundary (non-monotonic check)
  int max_retest_rounds = 2;
  int power_step_w = 10;
  int max_power_steps = 5;
  bool tune_fan = true;
  Sec hang_timeout{30};
};

struct BenchmarkConfig {
  Sec warmup{30};
  int runs = 3;
  Sec run_duration{10};
  double max_cv_pct = 3.0;
  bool invalid_on_power_cap = false;
  std::string workload = "builtin";  // "builtin" | "external"
  std::string external_command;
  std::string external_score_regex;
  std::string external_unit = "score";
  Sec external_timeout{600};
};

struct MonitorConfig {
  Ms poll{1000};
  Sec cooldown{1800};
  Sec hysteresis_hold{60};
  double hysteresis_delta_c = 5.0;
  int max_violations_24h = 3;
  int instability_retune_threshold = 2;
};

struct ProfilesConfig {
  std::string default_profile;  // empty: automatic (autotune.objective)
  bool auto_switch = true;
  Ms poll{2000};
};

struct ServiceConfig {
  Sec watchdog_timeout{120};  // must exceed tuning.hang_timeout_s comfortably
};

struct Config {
  AutotuneConfig autotune;
  SafetyConfig safety;
  TuningConfig tuning;
  BenchmarkConfig benchmark;
  MonitorConfig monitor;
  ProfilesConfig profiles;
  ServiceConfig service;
  std::string source;                 // file path or "<defaults>"
  std::vector<std::string> warnings;  // e.g. file missing
};

// Parse TOML text. Unknown keys and wrong types are errors (a typo in a
// safety key must not be silently ignored).
Result<Config> parse_config(std::string_view toml_text, const std::string& source_name);

// Load from a file. A missing file yields defaults plus a warning.
Result<Config> load_config(const std::filesystem::path& path);

}  // namespace gpuoc
