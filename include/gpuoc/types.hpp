// Backend-agnostic device model for the gpuoc overclocking API.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gpuoc/result.hpp"

namespace gpuoc {

// Closed integer interval [min, max].
struct Range {
  int min = 0;
  int max = 0;

  [[nodiscard]] bool valid() const noexcept { return min <= max; }
  [[nodiscard]] bool contains(int v) const noexcept { return v >= min && v <= max; }
  [[nodiscard]] int clamp(int v) const noexcept { return v < min ? min : (v > max ? max : v); }
  bool operator==(const Range&) const = default;
};

// Intersection of two ranges. Result may be invalid (min > max) if disjoint.
Range intersect(const Range& a, const Range& b) noexcept;

struct GpuId {
  std::string uuid;   // driver-reported UUID, e.g. "GPU-1a2b..."
  std::string name;   // marketing name
  int index = -1;     // enumeration index
  bool operator==(const GpuId&) const = default;
};

struct Capabilities {
  bool core_offset = false;
  bool mem_offset = false;
  bool power_limit = false;
  bool fan_control = false;
  bool voltage_control = false;
  bool locked_clocks = false;  // reported only; gpuoc never emulates offsets with locked clocks

  Range core_offset_mhz;  // min/max as reported by the driver
  Range mem_offset_mhz;
  Range power_limit_w;
  Range voltage_mv;       // only meaningful if voltage_control

  // Extensions to the minimum model (see docs/API.md).
  int stock_power_limit_w = 0;  // driver default limit (stock TDP)
  int stock_voltage_mv = 0;     // nominal voltage when voltage_control
  int voltage_step_mv = 0;      // driver-reported step, or the documented fallback
  Range fan_pct{0, 100};        // driver min/max manual fan speed
  bool ecc_supported = false;
  std::string driver_version;
};

struct FanPoint {
  int temp_c = 0;
  int pct = 0;
  bool operator==(const FanPoint&) const = default;
};

// Piecewise-linear fan curve. An empty curve means "driver automatic".
struct FanCurve {
  std::vector<FanPoint> points;  // strictly increasing temp_c

  [[nodiscard]] bool automatic() const noexcept { return points.empty(); }
  // Fan percentage at a temperature (flat beyond the end points). 0 if automatic.
  [[nodiscard]] int evaluate(double temp_c) const noexcept;
  // Structural validation only (ordering, 0..100). Safety rules live in SafetyPolicy.
  [[nodiscard]] Result<void> validate() const;
  bool operator==(const FanCurve&) const = default;

  static FanCurve driver_auto() { return {}; }
};

// Reasons the GPU limited itself during a sample.
enum LimitFlags : std::uint32_t {
  kLimitNone = 0,
  kLimitPower = 1u << 0,         // power cap (normal steady-state boost behavior)
  kLimitThermal = 1u << 1,       // software or hardware thermal slowdown
  kLimitHwSlowdown = 1u << 2,    // hardware slowdown (power brake, etc.)
  kLimitReliability = 1u << 3,   // reliability voltage limit
};

struct Telemetry {
  double core_mhz = 0;
  double mem_mhz = 0;
  double temp_c = 0;
  double power_w = 0;
  double fan_pct = 0;
  double util_pct = 0;
  std::optional<double> voltage_mv;
  std::uint64_t ecc_uncorrected = 0;  // 0 if unsupported
  std::uint32_t limit_flags = kLimitNone;
  std::chrono::steady_clock::time_point ts{};
};

// Every field optional: nullopt = leave unchanged.
struct Settings {
  std::optional<int> core_offset_mhz;
  std::optional<int> mem_offset_mhz;
  std::optional<int> power_limit_w;
  std::optional<FanCurve> fan_curve;
  std::optional<int> voltage_mv;  // only when voltage_control is true

  [[nodiscard]] bool empty() const noexcept {
    return !core_offset_mhz && !mem_offset_mhz && !power_limit_w && !fan_curve && !voltage_mv;
  }
  // Fields set in `over` replace fields in *this.
  [[nodiscard]] Settings merged(const Settings& over) const;
  [[nodiscard]] std::string to_string() const;
  bool operator==(const Settings&) const = default;
};

// Stock settings implied by a capability set: zero offsets, default power
// limit, automatic fan, nominal voltage. Only fields the GPU supports are set.
Settings stock_settings_for(const Capabilities& caps);

}  // namespace gpuoc
