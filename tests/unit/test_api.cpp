// Overclocking API: Result, device model, Gpu facade (clamping, rejection,
// capability checks, read-back verification), StockGuard, dry-run.
#include <gtest/gtest.h>

#include "backends/dry_run_backend.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

TEST(Result, ValueAndError) {
  Result<int> a = 5;
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.value(), 5);
  Result<int> b = make_error(ErrorCode::NotFound, "nope");
  ASSERT_FALSE(b.ok());
  EXPECT_EQ(b.error().code, ErrorCode::NotFound);
  EXPECT_EQ(b.value_or(7), 7);
  Result<void> v = success();
  EXPECT_TRUE(v.ok());
  Result<void> e = make_error(ErrorCode::Aborted, "x");
  EXPECT_FALSE(e.ok());
  EXPECT_EQ(e.error().to_string(), "aborted: x");
}

Result<int> doubled(Result<int> in) {
  GPUOC_TRY_ASSIGN(int v, std::move(in));
  return v * 2;
}

TEST(Result, TryMacros) {
  EXPECT_EQ(doubled(21).value(), 42);
  EXPECT_EQ(doubled(make_error(ErrorCode::Timeout, "t")).error().code, ErrorCode::Timeout);
}

TEST(Range, ClampContainsIntersect) {
  Range r{-100, 200};
  EXPECT_TRUE(r.contains(-100));
  EXPECT_FALSE(r.contains(201));
  EXPECT_EQ(r.clamp(500), 200);
  EXPECT_EQ(r.clamp(-500), -100);
  EXPECT_EQ(intersect(r, Range{0, 1000}), (Range{0, 200}));
  EXPECT_FALSE(intersect(Range{0, 1}, Range{5, 6}).valid());
}

TEST(FanCurve, EvaluateAndValidate) {
  FanCurve c{{{40, 30}, {60, 50}, {80, 100}}};
  ASSERT_TRUE(c.validate().ok());
  EXPECT_EQ(c.evaluate(20), 30);
  EXPECT_EQ(c.evaluate(50), 40);
  EXPECT_EQ(c.evaluate(70), 75);
  EXPECT_EQ(c.evaluate(95), 100);
  EXPECT_TRUE(FanCurve::driver_auto().automatic());
  EXPECT_FALSE((FanCurve{{{60, 50}, {50, 60}}}).validate().ok());
  EXPECT_FALSE((FanCurve{{{40, 130}, {50, 60}}}).validate().ok());
  EXPECT_FALSE((FanCurve{{{40, 30}}}).validate().ok());
}

TEST(Settings, MergeAndString) {
  Settings a;
  a.core_offset_mhz = 10;
  Settings b;
  b.mem_offset_mhz = 100;
  b.core_offset_mhz = 20;
  const Settings m = a.merged(b);
  EXPECT_EQ(*m.core_offset_mhz, 20);
  EXPECT_EQ(*m.mem_offset_mhz, 100);
  EXPECT_EQ(m.to_string(), "core=+20MHz mem=+100MHz");
  EXPECT_TRUE(Settings{}.empty());
}

// ---- Clamping: requests outside the driver range are clamped or rejected, never passed through.

TEST(GpuFacade, RejectsOutOfDriverRange) {
  auto rig = make_rig(1, permissive_safety());
  Settings s;
  s.core_offset_mhz = 5000;  // driver range is [-500, 1000]
  auto r = rig.gpu->apply(s, ClampMode::Reject);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::OutOfRange);
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0) << "rejected value must not reach the driver";
  EXPECT_EQ(rig.backend->apply_count(0), 0);
}

TEST(GpuFacade, ClampsToDriverRange) {
  auto rig = make_rig(1, permissive_safety());
  Settings s;
  s.core_offset_mhz = 5000;
  s.mem_offset_mhz = -9000;
  auto r = rig.gpu->apply(s, ClampMode::Clamp);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(*r.value().applied.core_offset_mhz, 1000);
  EXPECT_EQ(*r.value().applied.mem_offset_mhz, -1000);
  EXPECT_EQ(r.value().notes.size(), 2u);
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 1000);
  EXPECT_EQ(*rig.backend->raw_settings(0).mem_offset_mhz, -1000);
}

TEST(GpuFacade, RejectsUnsupportedFields) {
  mock::MockGpuSpec spec;
  spec.voltage_control = false;
  spec.fan_control = false;
  auto rig = make_rig(1, permissive_safety(), spec);
  Settings v;
  v.voltage_mv = 1000;
  auto r = rig.gpu->apply(v);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
  Settings f;
  f.fan_curve = FanCurve{{{40, 40}, {80, 100}}};
  auto r2 = rig.gpu->apply(f);
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(r2.error().code, ErrorCode::Unsupported);
  EXPECT_EQ(rig.backend->apply_count(0), 0);
}

TEST(GpuFacade, ReadbackMismatchIsAnErrorAndRollsBack) {
  auto rig = make_rig(1, permissive_safety());
  rig.backend->faults(0).readback_mismatch = true;
  Settings s;
  s.core_offset_mhz = 30;
  auto r = rig.gpu->apply(s);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::VerifyFailed);
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0) << "partial apply must be rolled back";
}

TEST(GpuFacade, ApplyFailureRestoresAutomaticFan) {
  auto rig = make_rig(1, permissive_safety());
  rig.backend->faults(0).fail_fan_apply = true;
  Settings s;
  s.fan_curve = FanCurve{{{40, 40}, {60, 60}, {80, 100}}};
  auto r = rig.gpu->apply(s);
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(rig.backend->raw_settings(0).fan_curve->automatic());
}

TEST(GpuFacade, ResetToStockVerifies) {
  auto rig = make_rig(1, permissive_safety());
  Settings s;
  s.core_offset_mhz = 60;
  s.mem_offset_mhz = 200;
  s.power_limit_w = 300;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  ASSERT_TRUE(rig.gpu->reset_to_stock().ok());
  const Settings raw = rig.backend->raw_settings(0);
  EXPECT_EQ(*raw.core_offset_mhz, 0);
  EXPECT_EQ(*raw.mem_offset_mhz, 0);
  EXPECT_EQ(*raw.power_limit_w, 320);
}

// ---- StockGuard: restores stock on destruction, including on error paths.

Result<void> failing_operation(Gpu& gpu) {
  StockGuard guard(gpu);
  Settings s;
  s.core_offset_mhz = 90;
  GPUOC_TRY(gpu.apply(s));
  return make_error(ErrorCode::HardwareError, "simulated failure after the write");
}

TEST(StockGuard, RestoresOnErrorPath) {
  auto rig = make_rig(1, permissive_safety());
  auto r = failing_operation(*rig.gpu);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0);
  EXPECT_GE(rig.backend->reset_count(0), 1);
}

TEST(StockGuard, RestoresOnNormalCompletionAndRegisters) {
  auto rig = make_rig(1, permissive_safety());
  {
    StockGuard guard(*rig.gpu);
    EXPECT_EQ(StockRegistry::instance().size(), 1u);
    Settings s;
    s.mem_offset_mhz = 400;
    ASSERT_TRUE(rig.gpu->apply(s).ok());
    ASSERT_TRUE(guard.stock_snapshot().has_value());
    EXPECT_EQ(*guard.stock_snapshot()->mem_offset_mhz, 0);
  }
  EXPECT_EQ(StockRegistry::instance().size(), 0u);
  EXPECT_EQ(*rig.backend->raw_settings(0).mem_offset_mhz, 0);
}

TEST(StockRegistry, RestoreAllRevertsEveryGuardedGpu) {
  auto rig = make_rig(1, permissive_safety());
  StockGuard guard(*rig.gpu);
  Settings s;
  s.core_offset_mhz = 45;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  EXPECT_EQ(StockRegistry::instance().restore_all("test"), 0);
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0);
}

// ---- Dry-run: every write logged, nothing applied.

TEST(DryRun, LogsWritesAndAppliesNothing) {
  auto rig = make_rig(1, permissive_safety());
  auto dry = std::make_shared<DryRunBackend>(rig.backend);
  Gpu gpu(dry, rig.id, rig.policy);
  Settings s;
  s.core_offset_mhz = 75;
  s.power_limit_w = 300;
  auto r = gpu.apply(s);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0);
  EXPECT_EQ(rig.backend->apply_count(0), 0);
  ASSERT_EQ(dry->journal().size(), 1u);
  EXPECT_NE(dry->journal()[0].find("core=+75MHz"), std::string::npos);
  EXPECT_NE(dry->journal()[0].find("power=300W"), std::string::npos);
  EXPECT_EQ(*gpu.settings().value().core_offset_mhz, 75) << "shadow read-back";
  ASSERT_TRUE(gpu.reset_to_stock().ok());
  EXPECT_EQ(rig.backend->reset_count(0), 0);
  EXPECT_EQ(dry->journal().size(), 2u);
}

}  // namespace
}  // namespace gpuoc
