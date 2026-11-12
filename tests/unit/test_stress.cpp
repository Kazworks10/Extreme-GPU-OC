// Stress runner and stability verdicts.
#include <gtest/gtest.h>

#include "stress/stress_runner.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

// ---- Pure verdict mapping ------------------------------------------------------

TEST(Verdicts, EachUnstableSignalMapsToUnstable) {
  const std::vector<bool StressSignals::*> unstable = {
      &StressSignals::checksum_mismatch, &StressSignals::crashed,      &StressSignals::exited_nonzero,
      &StressSignals::device_lost,       &StressSignals::hang,         &StressSignals::driver_event,
      &StressSignals::ecc_increase,      &StressSignals::temp_exceeded, &StressSignals::power_exceeded};
  for (auto field : unstable) {
    StressSignals s;
    s.iterations = 10;
    s.*field = true;
    EXPECT_EQ(evaluate_stress(s).kind, VerdictKind::Unstable);
    s.interrupted = true;  // unstable evidence wins over interruption ...
    s.telemetry_missing = true;  // ... and over missing telemetry
    EXPECT_EQ(evaluate_stress(s).kind, VerdictKind::Unstable);
  }
}

TEST(Verdicts, MissingTelemetryIsInconclusive) {
  StressSignals s;
  s.iterations = 100;
  s.telemetry_missing = true;
  EXPECT_EQ(evaluate_stress(s).kind, VerdictKind::Inconclusive);
  EXPECT_FALSE(evaluate_stress(s).is_stable());
}

TEST(Verdicts, InterruptedIsInconclusive) {
  StressSignals s;
  s.iterations = 3;
  s.interrupted = true;
  EXPECT_EQ(evaluate_stress(s).kind, VerdictKind::Inconclusive);
}

TEST(Verdicts, CleanRunIsStable) {
  StressSignals s;
  s.iterations = 120;
  EXPECT_TRUE(evaluate_stress(s).is_stable());
  StressSignals none;
  EXPECT_EQ(evaluate_stress(none).kind, VerdictKind::Inconclusive) << "zero iterations is not evidence of stability";
}

// ---- Stress runner against the mock -------------------------------------------

struct StressRig {
  test::MockRig rig;
  std::unique_ptr<StressRunner> runner;
  explicit StressRig(std::uint64_t seed = 11, SafetyConfig safety = permissive_safety(), mock::MockGpuSpec spec = {})
      : rig(make_rig(seed, safety, spec)) {
    runner = std::make_unique<StressRunner>(rig.backend->workload_factory(), rig.backend->event_log(), *rig.clock);
  }
  StressReport run(int seconds = 120) {
    StressOptions o;
    o.duration = Sec(seconds);
    return runner->run(*rig.gpu, o, nullptr);
  }
};

TEST(StressRunner, StableAtStock) {
  StressRig r;
  auto rep = r.run(120);
  EXPECT_TRUE(rep.verdict.is_stable()) << rep.verdict.to_string();
  EXPECT_GE(rep.signals.iterations, 119);
  EXPECT_GT(rep.signals.telemetry_samples, 100);
  EXPECT_GT(rep.score, 0);
  EXPECT_FALSE(r.rig.backend->load_active(0)) << "workload stopped";
}

TEST(StressRunner, ChecksumMismatchIsUnstable) {
  StressRig r;
  Settings s;
  s.core_offset_mhz = r.rig.backend->hidden_core_max(0) + 30;
  ASSERT_TRUE(r.rig.gpu->apply(s).ok());
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.checksum_mismatch);
}

TEST(StressRunner, CrashIsUnstable) {
  StressRig r;
  r.rig.backend->faults(0).crash_next = true;
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.crashed);
}

TEST(StressRunner, DriverResetIsUnstable) {
  StressRig r;
  r.rig.backend->faults(0).device_lost_next = true;
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.device_lost);
  EXPECT_TRUE(rep.signals.driver_event) << "TDR event found in the event log";
}

TEST(StressRunner, DriverEventAloneIsUnstable) {
  StressRig r;
  r.rig.backend->faults(0).driver_event_next = true;
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.driver_event);
  EXPECT_FALSE(rep.signals.device_lost);
}

TEST(StressRunner, EccIncreaseIsUnstable) {
  mock::MockGpuSpec spec;
  spec.ecc_supported = true;
  StressRig r(11, permissive_safety(), spec);
  r.rig.backend->faults(0).ecc_errors_next = 2;
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.ecc_increase);
}

TEST(StressRunner, TemperatureViolationIsUnstable) {
  StressRig r;
  r.rig.backend->faults(0).temp_offset_c = 40;  // pushes readings past 83 C
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.temp_exceeded);
}

TEST(StressRunner, PowerViolationIsUnstable) {
  StressRig r;
  r.rig.backend->faults(0).extra_power_w = 200;
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  EXPECT_TRUE(rep.signals.power_exceeded);
}

TEST(StressRunner, MissingTelemetryIsInconclusive) {
  StressRig r;
  r.rig.backend->faults(0).telemetry_missing = true;
  auto rep = r.run();
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Inconclusive);
  EXPECT_TRUE(rep.signals.telemetry_missing);
}

TEST(StressRunner, AbortIsInconclusive) {
  StressRig r;
  std::atomic<bool> abort{true};
  StressOptions o;
  auto rep = r.runner->run(*r.rig.gpu, o, &abort);
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Inconclusive);
}

TEST(StressRunner, DurationFollowsTheClock) {
  StressRig r;
  const auto before = r.rig.clock->elapsed();
  auto rep = r.run(1800);
  EXPECT_TRUE(rep.verdict.is_stable());
  EXPECT_GE(r.rig.clock->elapsed() - before, Sec(1800));
  EXPECT_LE(r.rig.clock->elapsed() - before, Sec(1802));
}

}  // namespace
}  // namespace gpuoc
