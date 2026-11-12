// Autonomous controller, runtime monitor, violations, end-to-end run, and
// crash recovery.
#include <gtest/gtest.h>

#include "controller/controller.hpp"
#include "core/json_codec.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

struct ControllerRig {
  Config cfg;
  test::TempDir dir;
  test::MockRig rig;
  std::unique_ptr<LkgStore> lkg;
  std::unique_ptr<StateStore> state;
  std::unique_ptr<GpuController> ctl;

  explicit ControllerRig(std::uint64_t seed = 21, std::function<void(Config&)> tweak = {}, mock::MockGpuSpec spec = {},
                         std::string mock_state = {}, std::filesystem::path shared_dir = {}) {
    cfg.autotune.enabled = true;
    cfg.safety = permissive_safety();
    if (tweak) tweak(cfg);
    rig = make_rig(seed, cfg.safety, spec, std::move(mock_state));
    const auto base = shared_dir.empty() ? dir.path() : shared_dir;
    lkg = std::make_unique<LkgStore>(base / "lkg.json");
    (void)lkg->load();
    state = std::make_unique<StateStore>(base / "state.json");
    (void)state->load();
    ctl = make_controller(base);
  }

  std::unique_ptr<GpuController> make_controller(const std::filesystem::path& base) {
    ControllerDeps d;
    d.config = &cfg;
    d.clock = rig.clock.get();
    d.lkg = lkg.get();
    d.state = state.get();
    d.events = rig.backend->event_log();
    auto backend = rig.backend;
    auto clock = rig.clock;
    const Ms poll = cfg.monitor.poll;
    d.make_trial_runner = [backend, clock, poll]() -> std::unique_ptr<ITrialRunner> {
      auto stress = std::make_shared<StressRunner>(backend->workload_factory(), backend->event_log(), *clock);
      auto bench = std::make_shared<WorkloadBenchmark>(backend->workload_factory(), *clock,
                                                       BenchmarkProtocol{Sec(30), 3, Sec(10), 3.0, false});
      return std::make_unique<StressTrialRunner>(stress, bench, poll);
    };
    d.make_tuning_log = [base](const GpuId&) { return std::make_unique<TuningLog>(base / "tune.jsonl"); };
    return std::make_unique<GpuController>(*rig.gpu, d);
  }

  bool run_until(ControllerState want, int max_steps = 200) {
    for (int i = 0; i < max_steps; ++i) {
      if (ctl->state() == want) return true;
      ctl->step();
    }
    return ctl->state() == want;
  }
};

// ---- End-to-end: a full autonomous run on the mock with a simulated clock.

TEST(EndToEnd, AutonomousRunCommitsValidatedLkg) {
  ControllerRig r(21);
  ASSERT_TRUE(r.ctl->startup().ok());
  ASSERT_TRUE(r.run_until(ControllerState::Run)) << r.ctl->status().dump(2);
  auto outcome = r.ctl->last_outcome();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_TRUE(outcome->validated);
  EXPECT_EQ(outcome->validation_duration, Sec(1800));
  // The validated profile was committed to lkg.json ...
  LkgStore reload(r.dir.path() / "lkg.json");
  ASSERT_TRUE(reload.load().ok());
  auto e = reload.get(r.rig.id.uuid, Objective::MaxPerfPerWatt);
  ASSERT_TRUE(e.has_value());
  EXPECT_TRUE(e->validated);
  EXPECT_EQ(e->settings, outcome->settings);
  EXPECT_GT(e->score, e->baseline_score);
  // ... and is what the GPU runs now.
  const Settings raw = r.rig.backend->raw_settings(0);
  EXPECT_EQ(raw.core_offset_mhz, e->settings.core_offset_mhz);
  EXPECT_EQ(raw.mem_offset_mhz, e->settings.mem_offset_mhz);
  EXPECT_GT(*raw.core_offset_mhz, 0);
  EXPECT_EQ(r.ctl->status()["applied"]["validated"], true);
  // Several sim-hours went by, not wall-clock hours.
  EXPECT_GT(r.rig.clock->elapsed(), Sec(1800));
  // The run state keeps monitoring.
  for (int i = 0; i < 30; ++i) r.ctl->step();
  EXPECT_EQ(r.ctl->state(), ControllerState::Run);
}

TEST(Controller, MonitorOnlyWithoutBothGates) {
  for (int variant = 0; variant < 2; ++variant) {
    ControllerRig r(22, [&](Config& c) {
      if (variant == 0) c.autotune.enabled = false;
      if (variant == 1) c.safety.acknowledge_risk = false;
    });
    ASSERT_TRUE(r.ctl->startup().ok());
    for (int i = 0; i < 20; ++i) r.ctl->step();
    EXPECT_EQ(r.ctl->state(), ControllerState::MonitorOnly);
    EXPECT_EQ(r.rig.backend->apply_count(0), 0) << "monitor-only never writes";
    EXPECT_FALSE(r.ctl->last_outcome().has_value());
  }
}

TEST(Controller, StartupRevertsToStockFirst) {
  ControllerRig r(23);
  Settings oc;
  oc.core_offset_mhz = 150;
  ASSERT_TRUE(r.rig.gpu->apply(oc).ok());
  ASSERT_TRUE(r.ctl->startup().ok());
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
  EXPECT_EQ(r.ctl->state(), ControllerState::Discover);
}

TEST(Controller, ReappliesValidLkgOnlyWithAcknowledgement) {
  test::TempDir shared;
  {
    ControllerRig first(24, {}, {}, {}, shared.path());
    ASSERT_TRUE(first.ctl->startup().ok());
    ASSERT_TRUE(first.run_until(ControllerState::Run));
  }
  // Autotune off, risk acknowledged: the validated profile is re-applied.
  ControllerRig ack(24, [](Config& c) { c.autotune.enabled = false; }, {}, {}, shared.path());
  ASSERT_TRUE(ack.ctl->startup().ok());
  ASSERT_TRUE(ack.run_until(ControllerState::Run, 5));
  EXPECT_GT(*ack.rig.backend->raw_settings(0).core_offset_mhz, 0);
  // Without acknowledgement nothing is written.
  ControllerRig no_ack(24, [](Config& c) { c.safety.acknowledge_risk = false; }, {}, {}, shared.path());
  ASSERT_TRUE(no_ack.ctl->startup().ok());
  for (int i = 0; i < 5; ++i) no_ack.ctl->step();
  EXPECT_EQ(no_ack.ctl->state(), ControllerState::MonitorOnly);
  EXPECT_EQ(no_ack.rig.backend->apply_count(0), 0);
}

TEST(Controller, DriverChangeTriggersRetune) {
  test::TempDir shared;
  {
    ControllerRig first(25, {}, {}, {}, shared.path());
    ASSERT_TRUE(first.ctl->startup().ok());
    ASSERT_TRUE(first.run_until(ControllerState::Run));
  }
  ControllerRig second(25, {}, {}, {}, shared.path());
  second.rig.backend->set_driver_version("572.16");
  ASSERT_TRUE(second.ctl->startup().ok());
  second.ctl->step();  // Discover
  EXPECT_EQ(second.ctl->state(), ControllerState::Baseline);
  EXPECT_NE(second.ctl->status()["state_reason"].get<std::string>().find("driver"), std::string::npos);
}

// ---- Runtime violation reverts to stock within one poll interval.

TEST(RuntimeMonitor, ViolationRevertsWithinOnePollInterval) {
  ControllerRig r(26);
  ASSERT_TRUE(r.ctl->startup().ok());
  ASSERT_TRUE(r.run_until(ControllerState::Run));
  ASSERT_GT(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
  const int resets_before = r.rig.backend->reset_count(0);
  r.rig.backend->faults(0).temp_offset_c = 60;  // temperature spike
  const auto t0 = r.rig.clock->elapsed();
  r.ctl->step();  // one poll
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
  EXPECT_GT(r.rig.backend->reset_count(0), resets_before);
  EXPECT_LE(r.rig.clock->elapsed() - t0, r.cfg.monitor.poll);
  EXPECT_EQ(r.ctl->state(), ControllerState::SafeRevert);
  const auto st = r.state->get(r.rig.id.uuid);
  ASSERT_EQ(st.violations.size(), 1u);
  EXPECT_EQ(st.violations[0].kind, "temperature");
  ASSERT_TRUE(st.cooldown_until.has_value());
  EXPECT_EQ(*st.cooldown_until - r.rig.clock->wall_now() > std::chrono::minutes(29), true);
}

TEST(RuntimeMonitor, DriverEventAndEccAreViolations) {
  mock::MockGpuSpec spec;
  spec.ecc_supported = true;
  ControllerRig r(27, {}, spec);
  ASSERT_TRUE(r.ctl->startup().ok());
  ASSERT_TRUE(r.run_until(ControllerState::Run));
  r.rig.backend->push_event(14, "nvlddmkm", "graphics exception (test)");
  r.ctl->step();
  EXPECT_EQ(r.ctl->state(), ControllerState::SafeRevert);
  EXPECT_EQ(r.state->get(r.rig.id.uuid).violations.back().kind, "driver_event");
}

TEST(RuntimeMonitor, CooldownThenRetryAndDisableAfterThree) {
  ControllerRig r(28);
  ASSERT_TRUE(r.ctl->startup().ok());
  for (int round = 1; round <= 3; ++round) {
    ASSERT_TRUE(r.run_until(ControllerState::Run, 4000)) << "round " << round << ": " << r.ctl->status().dump();
    r.rig.backend->faults(0).temp_offset_c = 60;
    r.ctl->step();
    EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
    r.rig.backend->faults(0).temp_offset_c = 0;
    r.ctl->step();  // SafeRevert -> Idle / Disabled
    if (round < 3) {
      EXPECT_EQ(r.ctl->state(), ControllerState::Idle);
      // Still idle well before the 30 min cooldown ends.
      for (int i = 0; i < 60; ++i) r.ctl->step();
      EXPECT_EQ(r.ctl->state(), ControllerState::Idle);
    }
  }
  EXPECT_EQ(r.ctl->state(), ControllerState::Disabled);
  // Disablement survives a restart ...
  ControllerRig again(28, {}, {}, {}, r.dir.path());
  ASSERT_TRUE(again.ctl->startup().ok());
  again.ctl->step();
  EXPECT_EQ(again.ctl->state(), ControllerState::Disabled);
  EXPECT_EQ(again.rig.backend->apply_count(0), 0);
  // ... until manually re-enabled.
  ASSERT_TRUE(again.ctl->enable().ok());
  EXPECT_TRUE(again.run_until(ControllerState::Run, 5));
}

TEST(RuntimeMonitor, HysteresisStepsDownAndBackUpNeverAboveValidated) {
  ControllerRig r(29);
  ASSERT_TRUE(r.ctl->startup().ok());
  ASSERT_TRUE(r.run_until(ControllerState::Run));
  const int validated = *r.rig.backend->raw_settings(0).core_offset_mhz;
  ASSERT_GE(validated, 30);
  // Let the GPU cool down from tuning, then push the reading to ~78 C: above
  // the 75 C target but below the 83 C max.
  for (int i = 0; i < 300; ++i) r.ctl->step();
  ASSERT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, validated);
  const double idle = r.rig.gpu->telemetry().value().temp_c;
  r.rig.backend->faults(0).temp_offset_c = 78.0 - idle;
  r.ctl->step();
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, validated - 15);
  r.ctl->step();
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, validated - 30);
  EXPECT_EQ(r.ctl->state(), ControllerState::Run);
  // Cool again: needs 60 s more than 5 C below target per step back up.
  r.rig.backend->faults(0).temp_offset_c = 0;
  for (int i = 0; i < 30; ++i) r.ctl->step();
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, validated - 30);
  for (int i = 0; i < 40; ++i) r.ctl->step();
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, validated - 15);
  for (int i = 0; i < 300; ++i) r.ctl->step();
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, validated) << "never above the validated value";
}

TEST(Controller, SafetyViolationDuringTuningEntersSafeRevert) {
  ControllerRig r(30);
  ASSERT_TRUE(r.ctl->startup().ok());
  r.rig.backend->faults(0).temp_offset_c = 30;  // fine idle, over 83 C under load
  for (int i = 0; i < 10 && r.ctl->state() != ControllerState::SafeRevert; ++i) r.ctl->step();
  EXPECT_EQ(r.ctl->state(), ControllerState::SafeRevert);
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
  EXPECT_EQ(r.state->get(r.rig.id.uuid).violations.size(), 1u);
  EXPECT_FALSE(r.lkg->latest(r.rig.id.uuid).has_value());
}

TEST(Controller, ManualTargetAndReset) {
  ControllerRig r(31, [](Config& c) { c.autotune.enabled = false; });
  ASSERT_TRUE(r.ctl->startup().ok());
  r.ctl->step();
  Settings s;
  s.core_offset_mhz = 45;
  ASSERT_TRUE(r.ctl->set_target(ActiveTarget{"manual", s, false, std::nullopt}).ok());
  EXPECT_EQ(r.ctl->state(), ControllerState::Run);
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 45);
  r.ctl->step();
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 45);
  Settings bad;
  bad.voltage_mv = 2000;
  EXPECT_FALSE(r.ctl->set_target(ActiveTarget{"manual", bad, false, std::nullopt}).ok());
  ASSERT_TRUE(r.ctl->revert_to_stock("user reset").ok());
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
  r.ctl->shutdown("test");
  EXPECT_EQ(*r.rig.backend->raw_settings(0).core_offset_mhz, 0);
}

// ---- Crash recovery: kill the process during a trial; on restart it reverts
// to stock and resumes from the tuning log.

class KillingRunner final : public ITrialRunner {
 public:
  KillingRunner(std::unique_ptr<ITrialRunner> inner, int kill_at) : inner_(std::move(inner)), kill_at_(kill_at) {}
  TrialResult stress(Gpu& gpu, const Settings& s, Sec d, const std::atomic<bool>* abort) override {
    if (++n_ == kill_at_) {
      (void)gpu.apply(s, ClampMode::Reject);  // the trial's settings are on the "hardware"
      std::fflush(nullptr);
      std::_Exit(42);  // hard kill: no destructors, no StockGuard
    }
    return inner_->stress(gpu, s, d, abort);
  }
  TrialResult benchmark(Gpu& gpu, const Settings& s, const std::atomic<bool>* abort) override {
    return inner_->benchmark(gpu, s, abort);
  }

 private:
  std::unique_ptr<ITrialRunner> inner_;
  int kill_at_;
  int n_ = 0;
};

// Child side of the crash-recovery test: runs the controller with a trial
// runner that hard-kills the process during the kill_at-th stress trial.
void run_child_until_killed(const std::filesystem::path& base, const std::string& mock_state,
                            const mock::MockGpuSpec& spec, int kill_at) {
  ControllerRig child(32, {}, spec, mock_state, base);
  ControllerDeps deps;
  deps.config = &child.cfg;
  deps.clock = child.rig.clock.get();
  deps.lkg = child.lkg.get();
  deps.state = child.state.get();
  deps.events = child.rig.backend->event_log();
  auto backend = child.rig.backend;
  auto clock = child.rig.clock;
  deps.make_trial_runner = [backend, clock, kill_at]() -> std::unique_ptr<ITrialRunner> {
    auto stress = std::make_shared<StressRunner>(backend->workload_factory(), backend->event_log(), *clock);
    auto bench = std::make_shared<WorkloadBenchmark>(backend->workload_factory(), *clock,
                                                     BenchmarkProtocol{Sec(30), 3, Sec(10), 3.0, false});
    return std::make_unique<KillingRunner>(std::make_unique<StressTrialRunner>(stress, bench, Ms(1000)), kill_at);
  };
  deps.make_tuning_log = [base](const GpuId&) { return std::make_unique<TuningLog>(base / "tune.jsonl"); };
  GpuController ctl(*child.rig.gpu, deps);
  (void)ctl.startup();
  for (int i = 0; i < 50; ++i) ctl.step();
  std::_Exit(0);  // not reached
}

TEST(CrashRecoveryDeathTest, KilledDuringTrialRevertsAndResumes) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  test::SharedDeathDir shared("GPUOC_TEST_CRASH_DIR");
  const std::string mock_state = platform::path_utf8(shared.path() / "mock-hw.json");
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 200;
  spec.true_mem_max_mhz = 1000;
  const int kill_at = 6;  // the 6th stress trial (a memory search point)

  EXPECT_EXIT(run_child_until_killed(shared.path(), mock_state, spec, kill_at), ::testing::ExitedWithCode(42), "");

  // The "hardware" still holds the trial's overclock.
  {
    mock::MockOptions o;
    o.seed = 32;
    o.specs = {spec};
    o.state_file = mock_state;
    mock::MockBackend hw(o);
    ASSERT_GT(*hw.raw_settings(0).mem_offset_mhz, 0) << "the killed process left settings applied";
  }

  // Restart: stock first, then resume from the tuning log.
  ControllerRig restarted(32, {}, spec, mock_state, shared.path());
  ASSERT_GT(*restarted.rig.backend->raw_settings(0).mem_offset_mhz, 0);
  ASSERT_TRUE(restarted.ctl->startup().ok());
  EXPECT_EQ(*restarted.rig.backend->raw_settings(0).mem_offset_mhz, 0) << "reverted before any tuning resumes";
  EXPECT_EQ(*restarted.rig.backend->raw_settings(0).core_offset_mhz, 0);
  ASSERT_TRUE(restarted.run_until(ControllerState::Run));
  auto o = restarted.ctl->last_outcome();
  ASSERT_TRUE(o.has_value());
  EXPECT_TRUE(o->resumed);
  EXPECT_GE(o->replayed_trials, kill_at) << "completed trials were not re-run";
  EXPECT_TRUE(o->validated);
  bool crash_noted = false;
  for (const auto& n : o->notes) crash_noted |= n.find("crash") != std::string::npos;
  EXPECT_TRUE(crash_noted);
}

}  // namespace
}  // namespace gpuoc
