// Section 9 safety rules: limit precedence, gates, hard ceilings, fan floor.
#include <gtest/gtest.h>

#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

Capabilities caps_4080() {
  Capabilities c;
  c.core_offset = c.mem_offset = c.power_limit = c.fan_control = c.voltage_control = true;
  c.core_offset_mhz = {-500, 1000};
  c.mem_offset_mhz = {-1000, 3000};
  c.power_limit_w = {150, 400};
  c.stock_power_limit_w = 320;
  c.voltage_mv = {700, 1100};
  c.stock_voltage_mv = 1000;
  c.voltage_step_mv = 10;
  return c;
}

// ---- Limit precedence: effective = min(config, driver range, hard ceiling).

TEST(LimitPrecedence, TemperatureConfigCannotExceedHardCeiling) {
  SafetyConfig cfg;
  cfg.max_temp_c = 95;     // above the 90 C hard ceiling
  cfg.target_temp_c = 93;  // above max
  SafetyPolicy p(cfg);
  EXPECT_DOUBLE_EQ(p.config().max_temp_c, HardLimits::kMaxTempC);
  EXPECT_LT(p.config().target_temp_c, p.config().max_temp_c);
  EXPECT_EQ(p.normalization_notes().size(), 2u);
  EXPECT_DOUBLE_EQ(p.limits_for(caps_4080()).max_temp_c, 90.0);
}

TEST(LimitPrecedence, TemperatureConfigCanTighten) {
  SafetyConfig cfg;
  cfg.max_temp_c = 70;
  cfg.target_temp_c = 65;
  SafetyPolicy p(cfg);
  EXPECT_DOUBLE_EQ(p.limits_for(caps_4080()).max_temp_c, 70.0);
  EXPECT_DOUBLE_EQ(p.limits_for(caps_4080()).target_temp_c, 65.0);
}

TEST(LimitPrecedence, Defaults) {
  SafetyPolicy p(SafetyConfig{});
  auto lim = p.limits_for(caps_4080());
  EXPECT_DOUBLE_EQ(lim.max_temp_c, 83.0);
  EXPECT_DOUBLE_EQ(lim.target_temp_c, 75.0);
  EXPECT_EQ(lim.power_limit_w.max, 320) << "power defaults to stock TDP";
  EXPECT_FALSE(lim.voltage_allowed);
}

TEST(LimitPrecedence, PowerConfigBeyondDriverIsClamped) {
  SafetyConfig cfg = permissive_safety();
  cfg.max_power_w = 900;  // beyond the 400 W driver maximum
  SafetyPolicy p(cfg);
  EXPECT_EQ(p.limits_for(caps_4080()).power_limit_w.max, 400);
  cfg.max_power_w = 300;  // tighter than driver and stock
  EXPECT_EQ(SafetyPolicy(cfg).limits_for(caps_4080()).power_limit_w.max, 300);
}

TEST(LimitPrecedence, OffsetConfigBeyondDriverIsClamped) {
  SafetyConfig cfg;
  cfg.max_core_offset_mhz = 5000;
  cfg.max_mem_offset_mhz = 200;
  SafetyPolicy p(cfg);
  auto lim = p.limits_for(caps_4080());
  EXPECT_EQ(lim.core_offset_mhz.max, 1000);
  EXPECT_EQ(lim.mem_offset_mhz.max, 200);
}

TEST(LimitPrecedence, VoltageCeilingIsDriverMaxMinusOneStep) {
  SafetyConfig cfg = permissive_safety();
  cfg.max_voltage_mv = 1500;  // config tries to raise the ceiling
  SafetyPolicy p(cfg);
  auto lim = p.limits_for(caps_4080());
  EXPECT_TRUE(lim.voltage_allowed);
  EXPECT_EQ(lim.voltage_mv.max, 1090) << "1100 - 10 mV step; config cannot raise it";
  cfg.max_voltage_mv = 1050;
  EXPECT_EQ(SafetyPolicy(cfg).limits_for(caps_4080()).voltage_mv.max, 1050) << "config can lower it";
}

TEST(LimitPrecedence, VoltageStepFallbackWhenDriverReportsNone) {
  auto caps = caps_4080();
  caps.voltage_step_mv = 0;
  SafetyPolicy p(permissive_safety());
  EXPECT_EQ(p.limits_for(caps).voltage_mv.max, 1100 - HardLimits::kVoltageStepFallbackMv);
}

TEST(LimitPrecedence, CheckClampsAndRejects) {
  SafetyConfig cfg = permissive_safety();
  cfg.max_core_offset_mhz = 150;
  SafetyPolicy p(cfg);
  Settings s;
  s.core_offset_mhz = 400;
  s.voltage_mv = 1100;  // exactly the driver max, above the ceiling
  std::vector<std::string> notes;
  auto clamped = p.check(caps_4080(), s, ClampMode::Clamp, &notes);
  ASSERT_TRUE(clamped.ok());
  EXPECT_EQ(*clamped.value().core_offset_mhz, 150);
  EXPECT_EQ(*clamped.value().voltage_mv, 1090);
  EXPECT_EQ(notes.size(), 2u);
  auto rejected = p.check(caps_4080(), s, ClampMode::Reject);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, ErrorCode::OutOfRange);
}

// ---- Gates.

TEST(Gates, VoltageRefusedWithoutAllowVoltage) {
  SafetyConfig cfg = permissive_safety();
  cfg.allow_voltage = false;
  auto rig = make_rig(1, cfg);
  Settings s;
  s.voltage_mv = 1010;
  auto r = rig.gpu->apply(s, ClampMode::Clamp);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::SafetyRefusal);
  EXPECT_EQ(*rig.backend->raw_settings(0).voltage_mv, 1000);
}

TEST(Gates, VoltageRefusedWithoutBackendSupportEvenIfAllowed) {
  mock::MockGpuSpec spec;
  spec.voltage_control = false;
  auto rig = make_rig(1, permissive_safety(), spec);
  Settings s;
  s.voltage_mv = 1010;
  auto r = rig.gpu->apply(s, ClampMode::Clamp);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
  EXPECT_FALSE(rig.policy->limits_for(rig.gpu->capabilities().value()).voltage_allowed);
}

TEST(Gates, PowerAboveStockRefusedWithoutGate) {
  SafetyConfig cfg = permissive_safety();
  cfg.allow_power_above_stock = false;
  auto rig = make_rig(1, cfg);
  Settings s;
  s.power_limit_w = 350;  // stock is 320
  auto r = rig.gpu->apply(s, ClampMode::Clamp);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::SafetyRefusal);
  EXPECT_EQ(*rig.backend->raw_settings(0).power_limit_w, 320);
  s.power_limit_w = 300;
  EXPECT_TRUE(rig.gpu->apply(s).ok()) << "below stock is fine";
}

TEST(Gates, PowerAboveStockAllowedWithGate) {
  auto rig = make_rig(1, permissive_safety());
  Settings s;
  s.power_limit_w = 380;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  EXPECT_EQ(*rig.backend->raw_settings(0).power_limit_w, 380);
}

TEST(Gates, AutotuneRequiresBothFlags) {
  SafetyConfig cfg;
  EXPECT_FALSE(SafetyPolicy::autotune_permitted(true, cfg));
  cfg.acknowledge_risk = true;
  EXPECT_FALSE(SafetyPolicy::autotune_permitted(false, cfg));
  EXPECT_TRUE(SafetyPolicy::autotune_permitted(true, cfg));
}

TEST(Gates, AutonomousWritesRequireAcknowledgement) {
  EXPECT_FALSE(SafetyPolicy(SafetyConfig{}).autonomous_writes_permitted());
  EXPECT_TRUE(SafetyPolicy(permissive_safety()).autonomous_writes_permitted());
}

// ---- Fans: never below 30% while the GPU is above 60 C.

TEST(FanFloor, CurveBelowFloorWhenHotIsRefused) {
  SafetyPolicy p(SafetyConfig{});
  EXPECT_FALSE(p.check_fan_curve(FanCurve{{{40, 0}, {70, 20}, {90, 100}}}).ok());
  // Interpolation dipping below 30% just above 60 C is caught too.
  EXPECT_FALSE(p.check_fan_curve(FanCurve{{{55, 0}, {90, 100}}}).ok());
  EXPECT_TRUE(p.check_fan_curve(FanCurve{{{40, 0}, {60, 30}, {90, 100}}}).ok());
  EXPECT_TRUE(p.check_fan_curve(FanCurve::driver_auto()).ok());
}

TEST(FanFloor, ConfigCanOnlyRaiseFloor) {
  SafetyConfig cfg;
  cfg.min_fan_pct_hot = 10;
  SafetyPolicy low(cfg);
  EXPECT_EQ(low.config().min_fan_pct_hot, 30);
  EXPECT_EQ(low.enforce_fan_floor(10, 65), 30);
  EXPECT_EQ(low.enforce_fan_floor(10, 55), 10);
  cfg.min_fan_pct_hot = 45;
  SafetyPolicy high(cfg);
  EXPECT_EQ(high.enforce_fan_floor(35, 61), 45);
  EXPECT_FALSE(high.check_fan_curve(FanCurve{{{40, 30}, {60, 40}, {90, 100}}}).ok());
}

TEST(FanFloor, FacadeRefusesUnsafeCurve) {
  auto rig = make_rig(1, permissive_safety());
  Settings s;
  s.fan_curve = FanCurve{{{40, 10}, {80, 20}}};
  auto r = rig.gpu->apply(s, ClampMode::Clamp);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::SafetyRefusal);
  EXPECT_TRUE(rig.backend->raw_settings(0).fan_curve->automatic());
}

// ---- Runtime telemetry limits.

TEST(TelemetryLimits, TemperatureAndPower) {
  SafetyPolicy p(SafetyConfig{});
  auto lim = p.limits_for(caps_4080());
  Telemetry t;
  t.temp_c = 70;
  t.power_w = 300;
  EXPECT_FALSE(p.telemetry_violation(t, lim).has_value());
  t.temp_c = 83.5;
  EXPECT_TRUE(p.telemetry_violation(t, lim).has_value());
  t.temp_c = 70;
  t.power_w = 320 * 1.06;
  EXPECT_TRUE(p.telemetry_violation(t, lim).has_value());
}

}  // namespace
}  // namespace gpuoc
