#include <algorithm>
#include <cmath>
#include <format>

#include "gpuoc/safety.hpp"

namespace gpuoc {

SafetyPolicy::SafetyPolicy(SafetyConfig cfg) : cfg_(std::move(cfg)) {
  // Temperature: config may only tighten the hard ceiling.
  if (!(cfg_.max_temp_c <= HardLimits::kMaxTempC)) {  // also catches NaN
    notes_.push_back(std::format("safety.max_temp_c {} exceeds hard ceiling {}; using {}", cfg_.max_temp_c,
                                 HardLimits::kMaxTempC, HardLimits::kMaxTempC));
    cfg_.max_temp_c = HardLimits::kMaxTempC;
  }
  if (!(cfg_.target_temp_c <= cfg_.max_temp_c)) {
    const double t = std::min(HardLimits::kDefaultTargetTempC, cfg_.max_temp_c - 5.0);
    notes_.push_back(std::format("safety.target_temp_c {} is not below max_temp_c {}; using {}", cfg_.target_temp_c,
                                 cfg_.max_temp_c, t));
    cfg_.target_temp_c = t;
  }
  // Fans: config may only raise the hot-fan floor.
  if (cfg_.min_fan_pct_hot < HardLimits::kMinFanPctWhenHot) {
    notes_.push_back(std::format("safety.min_fan_pct_hot {} is below the hard floor {}; using {}",
                                 cfg_.min_fan_pct_hot, HardLimits::kMinFanPctWhenHot, HardLimits::kMinFanPctWhenHot));
    cfg_.min_fan_pct_hot = HardLimits::kMinFanPctWhenHot;
  }
  if (cfg_.min_fan_pct_hot > 100) cfg_.min_fan_pct_hot = 100;
  if (cfg_.max_power_w && *cfg_.max_power_w <= 0) cfg_.max_power_w.reset();
  if (cfg_.max_voltage_mv && *cfg_.max_voltage_mv <= 0) cfg_.max_voltage_mv.reset();
}

EffectiveLimits SafetyPolicy::limits_for(const Capabilities& caps) const {
  EffectiveLimits lim;
  lim.max_temp_c = std::min(cfg_.max_temp_c, HardLimits::kMaxTempC);
  lim.target_temp_c = std::min(cfg_.target_temp_c, lim.max_temp_c);
  lim.min_fan_pct_hot = std::max(cfg_.min_fan_pct_hot, HardLimits::kMinFanPctWhenHot);

  // Clock offsets: driver range, upper end lowered by config.
  lim.core_offset_allowed = caps.core_offset && caps.core_offset_mhz.valid();
  lim.core_offset_mhz = caps.core_offset_mhz;
  if (cfg_.max_core_offset_mhz) lim.core_offset_mhz.max = std::min(lim.core_offset_mhz.max, *cfg_.max_core_offset_mhz);
  if (!lim.core_offset_mhz.valid()) lim.core_offset_allowed = false;

  lim.mem_offset_allowed = caps.mem_offset && caps.mem_offset_mhz.valid();
  lim.mem_offset_mhz = caps.mem_offset_mhz;
  if (cfg_.max_mem_offset_mhz) lim.mem_offset_mhz.max = std::min(lim.mem_offset_mhz.max, *cfg_.max_mem_offset_mhz);
  if (!lim.mem_offset_mhz.valid()) lim.mem_offset_allowed = false;

  // Power: ceiling is stock TDP unless allow_power_above_stock; always capped
  // by the driver maximum; config can only lower it.
  lim.stock_power_w = caps.stock_power_limit_w;
  lim.power_allowed = caps.power_limit && caps.power_limit_w.valid() && caps.stock_power_limit_w > 0;
  if (lim.power_allowed) {
    int ceiling = cfg_.allow_power_above_stock ? caps.power_limit_w.max
                                               : std::min(caps.stock_power_limit_w, caps.power_limit_w.max);
    if (cfg_.max_power_w) ceiling = std::min(ceiling, *cfg_.max_power_w);
    lim.power_limit_w = Range{caps.power_limit_w.min, ceiling};
    if (!lim.power_limit_w.valid()) lim.power_allowed = false;
  }

  // Voltage: capability AND gate; ceiling = driver max - one step.
  lim.voltage_step_mv = caps.voltage_step_mv > 0 ? caps.voltage_step_mv : HardLimits::kVoltageStepFallbackMv;
  lim.voltage_allowed = caps.voltage_control && cfg_.allow_voltage && caps.voltage_mv.valid();
  if (caps.voltage_control && caps.voltage_mv.valid()) {
    int ceiling = caps.voltage_mv.max - lim.voltage_step_mv;
    if (cfg_.max_voltage_mv) ceiling = std::min(ceiling, *cfg_.max_voltage_mv);
    lim.voltage_mv = Range{caps.voltage_mv.min, ceiling};
    if (!lim.voltage_mv.valid()) lim.voltage_allowed = false;
  }

  lim.fan_allowed = caps.fan_control;
  return lim;
}

Result<void> SafetyPolicy::check_fan_curve(const FanCurve& curve) const {
  if (curve.automatic()) return success();
  GPUOC_TRY(curve.validate());
  const int floor = std::max(cfg_.min_fan_pct_hot, HardLimits::kMinFanPctWhenHot);
  // Check every integer temperature above the threshold; interpolation between
  // a cool point and a hot point can otherwise dip below the floor.
  for (int t = static_cast<int>(HardLimits::kFanHotThresholdC) + 1; t <= 120; ++t) {
    const int pct = curve.evaluate(t);
    if (pct < floor) {
      return make_error(ErrorCode::SafetyRefusal,
                        std::format("fan curve gives {}% at {} C; fans may not go below {}% above {} C", pct, t,
                                    floor, HardLimits::kFanHotThresholdC));
    }
  }
  return success();
}

int SafetyPolicy::enforce_fan_floor(int pct, double temp_c) const noexcept {
  const int floor = std::max(cfg_.min_fan_pct_hot, HardLimits::kMinFanPctWhenHot);
  if (temp_c > HardLimits::kFanHotThresholdC && pct < floor) return floor;
  return std::clamp(pct, 0, 100);
}

namespace {

Result<int> limit_value(const char* what, int value, const Range& range, ClampMode mode,
                        std::vector<std::string>* notes, const char* unit) {
  if (range.contains(value)) return value;
  if (mode == ClampMode::Reject) {
    return make_error(ErrorCode::OutOfRange, std::format("{} {}{} is outside the allowed range [{}, {}]{}", what, value,
                                                         unit, range.min, range.max, unit));
  }
  const int clamped = range.clamp(value);
  if (notes) notes->push_back(std::format("{} {}{} clamped to {}{}", what, value, unit, clamped, unit));
  return clamped;
}

}  // namespace

Result<Settings> SafetyPolicy::check(const Capabilities& caps, const Settings& req, ClampMode mode,
                                     std::vector<std::string>* notes) const {
  const EffectiveLimits lim = limits_for(caps);
  Settings out;

  if (req.core_offset_mhz) {
    if (!caps.core_offset) return make_error(ErrorCode::Unsupported, "this GPU does not support core clock offsets");
    if (!lim.core_offset_allowed) return make_error(ErrorCode::OutOfRange, "no valid core offset range");
    GPUOC_TRY_ASSIGN(out.core_offset_mhz,
                     limit_value("core offset", *req.core_offset_mhz, lim.core_offset_mhz, mode, notes, " MHz"));
  }
  if (req.mem_offset_mhz) {
    if (!caps.mem_offset) return make_error(ErrorCode::Unsupported, "this GPU does not support memory clock offsets");
    if (!lim.mem_offset_allowed) return make_error(ErrorCode::OutOfRange, "no valid memory offset range");
    GPUOC_TRY_ASSIGN(out.mem_offset_mhz,
                     limit_value("memory offset", *req.mem_offset_mhz, lim.mem_offset_mhz, mode, notes, " MHz"));
  }
  if (req.power_limit_w) {
    if (!caps.power_limit) return make_error(ErrorCode::Unsupported, "this GPU does not support power limit control");
    if (!lim.power_allowed) return make_error(ErrorCode::Unsupported, "power limit range or stock TDP unknown");
    if (*req.power_limit_w > lim.stock_power_w && !cfg_.allow_power_above_stock) {
      return make_error(ErrorCode::SafetyRefusal,
                        std::format("power limit {} W is above stock TDP {} W; set safety.allow_power_above_stock = true",
                                    *req.power_limit_w, lim.stock_power_w));
    }
    GPUOC_TRY_ASSIGN(out.power_limit_w,
                     limit_value("power limit", *req.power_limit_w, lim.power_limit_w, mode, notes, " W"));
  }
  if (req.voltage_mv) {
    if (!caps.voltage_control) return make_error(ErrorCode::Unsupported, "this GPU/driver does not expose voltage control");
    if (!cfg_.allow_voltage)
      return make_error(ErrorCode::SafetyRefusal, "voltage changes require safety.allow_voltage = true");
    if (!lim.voltage_allowed) return make_error(ErrorCode::OutOfRange, "no valid voltage range");
    GPUOC_TRY_ASSIGN(out.voltage_mv, limit_value("voltage", *req.voltage_mv, lim.voltage_mv, mode, notes, " mV"));
  }
  if (req.fan_curve) {
    if (!caps.fan_control) return make_error(ErrorCode::Unsupported, "this GPU does not support fan control");
    GPUOC_TRY(check_fan_curve(*req.fan_curve));
    out.fan_curve = req.fan_curve;
  }
  return out;
}

std::optional<std::string> SafetyPolicy::telemetry_violation(const Telemetry& t, const EffectiveLimits& lim) const {
  if (t.temp_c > lim.max_temp_c)
    return std::format("temperature {:.1f} C exceeds max {:.1f} C", t.temp_c, lim.max_temp_c);
  if (lim.power_allowed && lim.power_limit_w.max > 0) {
    const double max_w = lim.power_limit_w.max * (1.0 + HardLimits::kPowerViolationTolerance);
    if (t.power_w > max_w)
      return std::format("power {:.1f} W exceeds max {} W (+{:.0f}% tolerance)", t.power_w, lim.power_limit_w.max,
                         HardLimits::kPowerViolationTolerance * 100);
  }
  // The hot-fan floor is enforced on every fan write we make (check_fan_curve,
  // enforce_fan_floor, Gpu::maintain_fan). Driver-automatic fan behavior is
  // not a violation.
  return std::nullopt;
}

}  // namespace gpuoc
