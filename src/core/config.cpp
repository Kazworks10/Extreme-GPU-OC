#include "core/config.hpp"

#include <format>

#include "core/toml_reader.hpp"
#include "platform/fs.hpp"

namespace gpuoc {

const char* to_string(Objective o) noexcept {
  switch (o) {
    case Objective::MaxPerformance: return "max_performance";
    case Objective::MaxPerfPerWatt: return "max_perf_per_watt";
    case Objective::Balanced: return "balanced";
  }
  return "?";
}

Result<Objective> parse_objective(std::string_view s) {
  if (s == "max_performance") return Objective::MaxPerformance;
  if (s == "max_perf_per_watt") return Objective::MaxPerfPerWatt;
  if (s == "balanced") return Objective::Balanced;
  return make_error(ErrorCode::InvalidArgument,
                    std::format("unknown objective '{}' (expected max_performance, max_perf_per_watt, balanced)", s));
}

namespace {

constexpr long long kDay = 24 * 3600;

Result<void> read_seconds(TomlTableReader& r, std::string_view key, Sec& out, long long min, long long max) {
  int v = static_cast<int>(out.count());
  GPUOC_TRY(r.get_int(key, v, min, max));
  out = Sec(v);
  return success();
}

Result<void> read_ms(TomlTableReader& r, std::string_view key, Ms& out, long long min, long long max) {
  int v = static_cast<int>(out.count());
  GPUOC_TRY(r.get_int(key, v, min, max));
  out = Ms(v);
  return success();
}

}  // namespace

Result<Config> parse_config(std::string_view text, const std::string& source_name) {
  GPUOC_TRY_ASSIGN(toml::table root, parse_toml(text, source_name));
  Config c;
  c.source = source_name;
  TomlTableReader top(&root, "");

  {
    TomlTableReader r(top.sub_table("autotune"), "autotune");
    GPUOC_TRY(r.get_bool("enabled", c.autotune.enabled));
    std::string obj = to_string(c.autotune.objective);
    GPUOC_TRY(r.get_string("objective", obj));
    GPUOC_TRY_ASSIGN(c.autotune.objective, parse_objective(obj));
    GPUOC_TRY(read_seconds(r, "budget_s", c.autotune.budget, 60, 7 * kDay));
    GPUOC_TRY(r.get_int("revalidate_days", c.autotune.revalidate_days, 1, 365));
    GPUOC_TRY(r.finish());
  }
  {
    TomlTableReader r(top.sub_table("safety"), "safety");
    auto& s = c.safety;
    GPUOC_TRY(r.get_bool("acknowledge_risk", s.acknowledge_risk));
    GPUOC_TRY(r.get_bool("allow_voltage", s.allow_voltage));
    GPUOC_TRY(r.get_bool("allow_power_above_stock", s.allow_power_above_stock));
    // Ranges here are syntactic sanity only; SafetyPolicy tightens values
    // beyond the hard ceilings (e.g. max_temp_c = 95 becomes 90).
    GPUOC_TRY(r.get_double("max_temp_c", s.max_temp_c, 30, 150));
    GPUOC_TRY(r.get_double("target_temp_c", s.target_temp_c, 30, 150));
    GPUOC_TRY(r.get_opt_int("max_power_w", s.max_power_w, 1, 2000));
    GPUOC_TRY(r.get_opt_int("max_voltage_mv", s.max_voltage_mv, 1, 2000));
    GPUOC_TRY(r.get_opt_int("max_core_offset_mhz", s.max_core_offset_mhz, -2000, 5000));
    GPUOC_TRY(r.get_opt_int("max_mem_offset_mhz", s.max_mem_offset_mhz, -5000, 10000));
    GPUOC_TRY(r.get_int("min_fan_pct_hot", s.min_fan_pct_hot, 0, 100));
    GPUOC_TRY(r.finish());
  }
  {
    TomlTableReader r(top.sub_table("tuning"), "tuning");
    auto& t = c.tuning;
    GPUOC_TRY(r.get_int("core_step_mhz", t.core_step_mhz, 1, 200));
    GPUOC_TRY(r.get_int("mem_step_mhz", t.mem_step_mhz, 1, 1000));
    GPUOC_TRY(r.get_int("margin_steps", t.margin_steps, 0, 10));
    GPUOC_TRY(read_seconds(r, "short_stress_s", t.short_stress, 5, kDay));
    GPUOC_TRY(read_seconds(r, "long_stress_s", t.long_stress, 10, 7 * kDay));
    GPUOC_TRY(r.get_int("validation_retries", t.validation_retries, 0, 20));
    GPUOC_TRY(r.get_bool("boundary_lookahead", t.boundary_lookahead));
    GPUOC_TRY(r.get_int("max_retest_rounds", t.max_retest_rounds, 0, 10));
    GPUOC_TRY(r.get_int("power_step_w", t.power_step_w, 1, 100));
    GPUOC_TRY(r.get_int("max_power_steps", t.max_power_steps, 0, 50));
    GPUOC_TRY(r.get_bool("tune_fan", t.tune_fan));
    GPUOC_TRY(read_seconds(r, "hang_timeout_s", t.hang_timeout, 2, 3600));
    GPUOC_TRY(r.finish());
  }
  {
    TomlTableReader r(top.sub_table("benchmark"), "benchmark");
    auto& b = c.benchmark;
    GPUOC_TRY(read_seconds(r, "warmup_s", b.warmup, 0, 3600));
    GPUOC_TRY(r.get_int("runs", b.runs, 1, 100));
    GPUOC_TRY(read_seconds(r, "run_s", b.run_duration, 1, 3600));
    GPUOC_TRY(r.get_double("max_cv_pct", b.max_cv_pct, 0.01, 100));
    GPUOC_TRY(r.get_bool("invalid_on_power_cap", b.invalid_on_power_cap));
    GPUOC_TRY(r.get_string("workload", b.workload));
    if (b.workload != "builtin" && b.workload != "external")
      return make_error(ErrorCode::ParseError, "benchmark.workload must be \"builtin\" or \"external\"");
    GPUOC_TRY(r.get_string("external_command", b.external_command));
    GPUOC_TRY(r.get_string("external_score_regex", b.external_score_regex));
    GPUOC_TRY(r.get_string("external_unit", b.external_unit));
    GPUOC_TRY(read_seconds(r, "external_timeout_s", b.external_timeout, 1, kDay));
    if (b.workload == "external" && (b.external_command.empty() || b.external_score_regex.empty()))
      return make_error(ErrorCode::ParseError,
                        "benchmark.workload = \"external\" needs external_command and external_score_regex");
    GPUOC_TRY(r.finish());
  }
  {
    TomlTableReader r(top.sub_table("monitor"), "monitor");
    auto& m = c.monitor;
    GPUOC_TRY(read_ms(r, "poll_interval_ms", m.poll, 100, 60000));
    GPUOC_TRY(read_seconds(r, "cooldown_s", m.cooldown, 1, 7 * kDay));
    GPUOC_TRY(read_seconds(r, "hysteresis_hold_s", m.hysteresis_hold, 1, 3600));
    GPUOC_TRY(r.get_double("hysteresis_delta_c", m.hysteresis_delta_c, 0.5, 30));
    GPUOC_TRY(r.get_int("max_violations_24h", m.max_violations_24h, 1, 100));
    GPUOC_TRY(r.get_int("instability_retune_threshold", m.instability_retune_threshold, 1, 100));
    GPUOC_TRY(r.finish());
  }
  {
    TomlTableReader r(top.sub_table("profiles"), "profiles");
    auto& p = c.profiles;
    GPUOC_TRY(r.get_string("default", p.default_profile));
    GPUOC_TRY(r.get_bool("auto_switch", p.auto_switch));
    GPUOC_TRY(read_ms(r, "poll_interval_ms", p.poll, 250, 60000));
    GPUOC_TRY(r.finish());
  }
  {
    TomlTableReader r(top.sub_table("service"), "service");
    GPUOC_TRY(read_seconds(r, "watchdog_timeout_s", c.service.watchdog_timeout, 5, 3600));
    GPUOC_TRY(r.finish());
  }
  GPUOC_TRY(top.finish());

  if (c.service.watchdog_timeout < c.tuning.hang_timeout * 2)
    return make_error(ErrorCode::ParseError, "service.watchdog_timeout_s must be at least twice tuning.hang_timeout_s");
  if (c.tuning.long_stress < c.tuning.short_stress)
    return make_error(ErrorCode::ParseError, "tuning.long_stress_s must be >= tuning.short_stress_s");
  return c;
}

Result<Config> load_config(const std::filesystem::path& path) {
  auto text = platform::read_file(path);
  if (!text) {
    if (text.error().code == ErrorCode::NotFound) {
      Config c;
      c.source = "<defaults>";
      c.warnings.push_back(std::format("config file {} not found; using defaults (monitor-only)",
                                       platform::path_utf8(path)));
      return c;
    }
    return text.error();
  }
  return parse_config(text.value(), platform::path_utf8(path));
}

}  // namespace gpuoc
