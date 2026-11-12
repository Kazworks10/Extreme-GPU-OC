// Mock backend: determinism, hidden stability model, physics, persistence.
#include <gtest/gtest.h>

#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

std::vector<IterationStatus> run_iterations(test::MockRig& rig, int n) {
  auto wl = rig.backend->workload_factory()(rig.id).value();
  EXPECT_TRUE(wl->start().ok());
  std::vector<IterationStatus> out;
  for (int i = 0; i < n; ++i) out.push_back(wl->run_iteration().status);
  wl->stop();
  return out;
}

TEST(Mock, DeterministicForSeed) {
  auto a = make_rig(42);
  auto b = make_rig(42);
  auto c = make_rig(43);
  EXPECT_EQ(a.backend->hidden_core_max(0), b.backend->hidden_core_max(0));
  EXPECT_EQ(a.backend->hidden_mem_max(0), b.backend->hidden_mem_max(0));
  EXPECT_EQ(a.id.uuid, b.id.uuid);
  EXPECT_NE(a.id.uuid, c.id.uuid);
  auto wa = a.backend->workload_factory()(a.id).value();
  auto wb = b.backend->workload_factory()(b.id).value();
  ASSERT_TRUE(wa->start().ok());
  ASSERT_TRUE(wb->start().ok());
  for (int i = 0; i < 20; ++i) EXPECT_DOUBLE_EQ(wa->run_iteration().work, wb->run_iteration().work);
}

TEST(Mock, StableAtOrBelowTrueMax) {
  auto rig = make_rig(7, permissive_safety());
  Settings s;
  s.core_offset_mhz = rig.backend->hidden_core_max(0);
  s.mem_offset_mhz = rig.backend->hidden_mem_max(0);
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  for (auto st : run_iterations(rig, 300)) ASSERT_EQ(st, IterationStatus::Ok);
}

TEST(Mock, ErrorsRiseAboveTrueMax) {
  auto rig = make_rig(7, permissive_safety());
  Settings s;
  s.core_offset_mhz = rig.backend->hidden_core_max(0) + 1;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  auto st = run_iterations(rig, 120);
  EXPECT_NE(std::find(st.begin(), st.end(), IterationStatus::ChecksumMismatch), st.end());
  s.core_offset_mhz = rig.backend->hidden_core_max(0) + 200;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  st = run_iterations(rig, 30);
  EXPECT_NE(std::find(st.begin(), st.end(), IterationStatus::DeviceLost), st.end());
}

TEST(Mock, VoltageExtendsStableCore) {
  auto rig = make_rig(9, permissive_safety());
  const int base = rig.backend->effective_core_max(0);
  Settings s;
  s.voltage_mv = 1030;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  EXPECT_EQ(rig.backend->effective_core_max(0), base + 24);  // 30 mV * 0.8 MHz/mV
}

TEST(Mock, ThermalAndPowerModel) {
  auto rig = make_rig(3, permissive_safety());
  const double idle_temp = rig.gpu->telemetry().value().temp_c;
  rig.backend->set_load(0, true);
  rig.clock->advance(Sec(300));
  auto t = rig.gpu->telemetry().value();
  EXPECT_GT(t.temp_c, idle_temp + 10);
  EXPECT_LT(t.temp_c, 75);
  EXPECT_GT(t.power_w, 200);
  EXPECT_LE(t.power_w, 320);
  // Power cap engages below demand.
  Settings s;
  s.power_limit_w = 150;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  rig.clock->advance(Sec(5));
  t = rig.gpu->telemetry().value();
  EXPECT_NEAR(t.power_w, 150, 1);
  EXPECT_TRUE(t.limit_flags & kLimitPower);
}

TEST(Mock, StatePersistsAcrossInstances) {
  test::TempDir dir;
  const std::string state = dir.str("mock-state.json");
  {
    auto rig = make_rig(5, permissive_safety(), {}, state);
    Settings s;
    s.core_offset_mhz = 120;
    s.fan_curve = FanCurve{{{40, 40}, {80, 100}}};
    ASSERT_TRUE(rig.gpu->apply(s).ok());
  }
  auto rig2 = make_rig(5, permissive_safety(), {}, state);
  EXPECT_EQ(*rig2.backend->raw_settings(0).core_offset_mhz, 120);
  EXPECT_EQ(rig2.backend->raw_settings(0).fan_curve->points.size(), 2u);
}

TEST(Mock, InjectedFaults) {
  auto rig = make_rig(5, permissive_safety());
  auto wl = rig.backend->workload_factory()(rig.id).value();
  ASSERT_TRUE(wl->start().ok());
  rig.backend->faults(0).crash_next = true;
  EXPECT_EQ(wl->run_iteration().status, IterationStatus::Crashed);
  rig.backend->faults(0).device_lost_next = true;
  EXPECT_EQ(wl->run_iteration().status, IterationStatus::DeviceLost);
  auto events = rig.backend->event_log()->driver_events_since(WallTime{}).value();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(is_driver_failure(events[0]));
  EXPECT_EQ(wl->run_iteration().status, IterationStatus::Ok);
  rig.backend->faults(0).telemetry_missing = true;
  EXPECT_FALSE(rig.gpu->telemetry().ok());
}

}  // namespace
}  // namespace gpuoc
