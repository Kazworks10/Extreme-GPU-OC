// Safety policy: hard ceilings, config-tightened limits, and gates.
//
// The effective limit for every quantity is min(config, driver range, hard
// ceiling). Configuration can tighten limits but never loosen them past the
// hard ceilings in this file. Nothing writes to a GPU without passing through
// SafetyPolicy::check (enforced by the Gpu facade).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "gpuoc/result.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc {

// Compiled-in hard limits. Not configurable.
struct HardLimits {
  static constexpr double kMaxTempC = 90.0;            // absolute temperature ceiling
  static constexpr double kDefaultRevertTempC = 83.0;  // default safety.max_temp_c
  static constexpr double kDefaultTargetTempC = 75.0;  // default safety.target_temp_c
  static constexpr double kFanHotThresholdC = 60.0;    // above this temperature ...
  static constexpr int kMinFanPctWhenHot = 30;         // ... fans never go below this
  static constexpr double kPowerViolationTolerance = 0.05;  // 5% sampling tolerance
  static constexpr int kVoltageStepFallbackMv = 6;     // when the driver reports no step
};

// [safety] section of gpuoc.toml, before normalization.
struct SafetyConfig {
  bool acknowledge_risk = false;
  bool allow_voltage = false;
  bool allow_power_above_stock = false;
  double max_temp_c = HardLimits::kDefaultRevertTempC;
  double target_temp_c = HardLimits::kDefaultTargetTempC;
  std::optional<int> max_power_w;          // nullopt: stock TDP (or driver max if allowed above stock)
  std::optional<int> max_voltage_mv;       // can only lower the voltage ceiling
  std::optional<int> max_core_offset_mhz;  // can only lower the driver maximum
  std::optional<int> max_mem_offset_mhz;
  int min_fan_pct_hot = HardLimits::kMinFanPctWhenHot;  // can only be raised
};

// Limits in force for one GPU after combining config, driver, and hard limits.
struct EffectiveLimits {
  double max_temp_c = HardLimits::kDefaultRevertTempC;
  double target_temp_c = HardLimits::kDefaultTargetTempC;
  bool core_offset_allowed = false;
  Range core_offset_mhz;
  bool mem_offset_allowed = false;
  Range mem_offset_mhz;
  bool power_allowed = false;
  Range power_limit_w;
  int stock_power_w = 0;
  bool voltage_allowed = false;  // capability AND safety.allow_voltage
  Range voltage_mv;              // max = min(config, driver max - one step)
  int voltage_step_mv = 0;
  bool fan_allowed = false;
  int min_fan_pct_hot = HardLimits::kMinFanPctWhenHot;
};

enum class ClampMode {
  Clamp,   // out-of-range values are clamped to the effective limit (and reported)
  Reject,  // out-of-range values are an OutOfRange error
};

class SafetyPolicy {
 public:
  // Normalizes the config: values beyond hard limits are tightened, and a
  // note is recorded for each adjustment.
  explicit SafetyPolicy(SafetyConfig cfg);

  [[nodiscard]] const SafetyConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const std::vector<std::string>& normalization_notes() const noexcept { return notes_; }

  [[nodiscard]] EffectiveLimits limits_for(const Capabilities& caps) const;

  // Validate a write request. Returns the settings that may be written.
  //  - unsupported field                     -> Unsupported
  //  - voltage without the voltage gate      -> SafetyRefusal
  //  - power above stock without the gate    -> SafetyRefusal
  //  - fan curve violating the hot-fan floor -> SafetyRefusal
  //  - out-of-range value                    -> clamped (Clamp) or OutOfRange (Reject)
  [[nodiscard]] Result<Settings> check(const Capabilities& caps, const Settings& request, ClampMode mode,
                                       std::vector<std::string>* notes = nullptr) const;

  // Fan rule: never below min_fan_pct_hot while temperature > 60 C.
  [[nodiscard]] Result<void> check_fan_curve(const FanCurve& curve) const;
  [[nodiscard]] int enforce_fan_floor(int pct, double temp_c) const noexcept;

  // Runtime check. Returns a human-readable reason if telemetry violates a limit.
  [[nodiscard]] std::optional<std::string> telemetry_violation(const Telemetry& t,
                                                               const EffectiveLimits& lim) const;

  // Gate: autonomous tuning requires both flags.
  [[nodiscard]] static bool autotune_permitted(bool autotune_enabled, const SafetyConfig& cfg) noexcept {
    return autotune_enabled && cfg.acknowledge_risk;
  }
  // Gate: any autonomous write (re-applying LKG, auto-switching) requires acknowledge_risk.
  [[nodiscard]] bool autonomous_writes_permitted() const noexcept { return cfg_.acknowledge_risk; }

 private:
  SafetyConfig cfg_;
  std::vector<std::string> notes_;
};

}  // namespace gpuoc
