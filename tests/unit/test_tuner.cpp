// Auto-tuning: search accuracy, non-monotonic handling, voltage, budget,
// validation back-off, resume from the tuning log, re-tune policy.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "core/json_codec.hpp"
#include "tuner/retune_policy.hpp"
#include "tuner/tuner.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

struct TunerRig {
  test::MockRig rig;
  test::TempDir dir;
  std::unique_ptr<TuningLog> log;
  std::unique_ptr<StressTrialRunner> runner;
  TunerOptions opts;

  explicit TunerRig(std::uint64_t seed, mock::MockGpuSpec spec = {}, SafetyConfig safety = permissive_safety())
      : rig(make_rig(seed, safety, spec)) {
    log = std::make_unique<TuningLog>(dir.path() / "tune.jsonl");
    auto stress = std::make_shared<StressRunner>(rig.backend->workload_factory(), rig.backend->event_log(), *rig.clock);
    auto bench = std::make_shared<WorkloadBenchmark>(rig.backend->workload_factory(), *rig.clock,
                                                     BenchmarkProtocol{Sec(30), 3, Sec(10), 3.0, false});
    runner = std::make_unique<StressTrialRunner>(stress, bench, Ms(1000));
    opts.objective = Objective::MaxPerfPerWatt;
  }
  Tuner make() { return Tuner(*rig.gpu, *runner, *log, *rig.clock, opts); }
  Result<TuneOutcome> run() {
    Tuner t = make();
    return t.run(nullptr);
  }
};

// ---- Search: finds the true max stable offset to within one step (20+ seeds).

TEST(TunerSearch, FindsTrueMaxWithinOneStepAcrossSeeds) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    TunerRig t(seed);
    auto r = t.run();
    ASSERT_TRUE(r.ok()) << "seed " << seed << ": " << r.error().message;
    const auto& o = r.value();
    const int core_true = t.rig.backend->hidden_core_max(0);
    const int mem_true = t.rig.backend->hidden_mem_max(0);
    EXPECT_LE(o.core_found_mhz, core_true) << "seed " << seed;
    EXPECT_GT(o.core_found_mhz, core_true - 15) << "seed " << seed;
    EXPECT_LE(o.mem_found_mhz, mem_true) << "seed " << seed;
    EXPECT_GT(o.mem_found_mhz, mem_true - 50) << "seed " << seed;
    EXPECT_TRUE(o.validated) << "seed " << seed;
    EXPECT_EQ(*o.settings.core_offset_mhz, std::max(0, o.core_found_mhz - 15)) << "one margin step";
    EXPECT_EQ(*o.settings.mem_offset_mhz, std::max(0, o.mem_found_mhz - 50));
    EXPECT_EQ(o.suspect_regions, 0) << "seed " << seed;
    EXPECT_GT(o.final_score, o.baseline.score) << "seed " << seed;
  }
}

TEST(TunerSearch, RampIsExponentialThenBinary) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 200;
  spec.true_mem_max_mhz = 0;
  TunerRig t(3, spec);
  t.opts.tuning.boundary_lookahead = false;
  auto tuner = t.make();
  ASSERT_TRUE(tuner.begin().ok());
  std::vector<int> tested;
  Tuner::Space sp;
  sp.stage = "core";
  sp.step = 15;
  sp.max = 1000;
  sp.settings_for = [&](int x) {
    tested.push_back(x);
    Settings s;
    s.core_offset_mhz = x;
    return s;
  };
  auto r = tuner.search(sp, nullptr);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(r.value().best_stable, 195);
  EXPECT_EQ(*r.value().lowest_unstable, 210);
  // 15, 30, 60, 120, 240 (ramp), then 180, 210, 195 (binary).
  EXPECT_EQ(tested, (std::vector<int>{15, 30, 60, 120, 240, 180, 210, 195}));
}

TEST(TunerSearch, VoltageExtendsStableCore) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 120;
  spec.volt_gain_mhz_per_mv = 2.0;  // one 10 mV step buys more than one 15 MHz core step
  TunerRig t(5, spec);
  t.opts.objective = Objective::MaxPerformance;
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  ASSERT_TRUE(r.value().voltage_mv.has_value());
  EXPECT_GT(*r.value().voltage_mv, 1000);
  EXPECT_LE(*r.value().voltage_mv, 1090) << "ceiling = driver max - one step";
  EXPECT_GT(r.value().core_found_mhz, 120) << "extra voltage extended the stable core offset";
  EXPECT_LE(r.value().core_found_mhz, 120 + 60) << "gain is capped by the mock at 60 MHz";
  EXPECT_TRUE(r.value().validated);
  EXPECT_EQ(*r.value().settings.voltage_mv, *r.value().voltage_mv);
}

TEST(TunerSearch, VoltageStageStopsWhenGainBelowOneCoreStep) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 120;
  spec.volt_gain_mhz_per_mv = 0.8;  // 10 mV -> 8 MHz < 15 MHz core step
  TunerRig t(5, spec);
  t.opts.objective = Objective::MaxPerformance;
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_FALSE(r.value().voltage_mv.has_value());
  bool noted = false;
  for (const auto& n : r.value().notes) noted |= n.find("less than one core step") != std::string::npos;
  EXPECT_TRUE(noted);
}

TEST(TunerSearch, VoltageStageSkippedWithoutGate) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 120;
  spec.volt_gain_mhz_per_mv = 2.0;
  SafetyConfig safety = permissive_safety();
  safety.allow_voltage = false;
  TunerRig t(5, spec, safety);
  t.opts.objective = Objective::MaxPerformance;
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_FALSE(r.value().voltage_mv.has_value());
  EXPECT_FALSE(r.value().settings.voltage_mv.has_value());
  EXPECT_EQ(*t.rig.backend->raw_settings(0).voltage_mv, 1000) << "voltage never written";
}

// ---- Non-monotonic results are detected and re-tested.

TEST(TunerNonMonotonic, TransientFailureIsDetectedAndRetested) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 200;
  TunerRig t(8, spec);
  t.rig.backend->faults(0).transient_core_failures = {120};  // fails once
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_GE(r.value().suspect_regions, 1);
  EXPECT_GE(r.value().retests, 2);
  EXPECT_EQ(r.value().core_found_mhz, 195) << "re-test cleared the transient; full headroom found";
}

TEST(TunerNonMonotonic, PersistentHoleCapsBelowIt) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 200;
  TunerRig t(8, spec);
  t.rig.backend->faults(0).unstable_core_hole = Range{115, 125};
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_GE(r.value().suspect_regions, 1);
  EXPECT_GE(r.value().retests, 2);
  EXPECT_LT(r.value().core_found_mhz, 115) << "conservative: never accept a value above an unresolved failure";
  bool noted = false;
  for (const auto& n : r.value().notes) noted |= n.find("non-monotonic") != std::string::npos;
  EXPECT_TRUE(noted);
}

// ---- Validation back-off and budget.

TEST(TunerValidation, BacksOffWhenLongStressFails) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 200;
  spec.true_mem_max_mhz = 1000;
  TunerRig t(9, spec);
  // Errors appear only after 300 s of continuous load at core >= 180.
  t.rig.backend->faults(0).slow_failure = mock::MockFaults::SlowFailure{180, 300};
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_TRUE(r.value().validated);
  EXPECT_EQ(*r.value().candidate.core_offset_mhz, 180);
  EXPECT_EQ(*r.value().settings.core_offset_mhz, 165) << "one extra step after the failed validation";
  EXPECT_EQ(*r.value().settings.mem_offset_mhz, *r.value().candidate.mem_offset_mhz - 50);
}

TEST(TunerBudget, KeepsBestValidatedWhenCapReached) {
  TunerRig t(10);
  t.opts.budget = Sec(3200);
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_TRUE(r.value().budget_exhausted);
  EXPECT_TRUE(r.value().validated) << "search stopped early but the best point so far was validated";
  EXPECT_LE(r.value().elapsed_s, 3200 + 1);
}

TEST(TunerBudget, NothingValidatedWhenBudgetTooSmall) {
  TunerRig t(10);
  t.opts.budget = Sec(900);
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_TRUE(r.value().budget_exhausted);
  EXPECT_FALSE(r.value().validated);
  EXPECT_TRUE(r.value().settings.empty());
}

TEST(TunerSafety, RefusesWhenUnstableAtStock) {
  TunerRig t(11);
  t.rig.backend->faults(0).transient_core_failures = {0};
  auto r = t.run();
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("stock"), std::string::npos);
}

TEST(TunerSafety, NeverExceedsEffectiveLimits) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 1000;  // would go to the driver max
  SafetyConfig safety = permissive_safety();
  safety.max_core_offset_mhz = 100;
  TunerRig t(12, spec, safety);
  auto r = t.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_LE(r.value().core_found_mhz, 100);
}

TEST(TunerAbort, AbortLeavesSessionResumable) {
  TunerRig t(13);
  std::atomic<bool> abort{true};
  auto tuner = t.make();
  auto r = tuner.run(&abort);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Aborted);
  auto resumable = t.log->find_resumable(t.rig.id.uuid, "566.36 (mock)",
                                         tuning_params_hash(t.opts.tuning, t.opts.objective, t.rig.policy->config()));
  ASSERT_TRUE(resumable.ok());
  EXPECT_TRUE(resumable.value().has_value());
}

// ---- Resume: a crash mid-trial leaves trial_start without trial_result.

TEST(TunerResume, ReplaysCompletedTrialsAndTreatsTornTrialAsUnstable) {
  mock::MockGpuSpec spec;
  spec.true_core_max_mhz = 200;
  spec.true_mem_max_mhz = 1000;
  TunerRig first(14, spec);
  auto full = first.run();
  ASSERT_TRUE(full.ok());
  // Rebuild the log as if the process died during the 9th trial.
  auto text = platform::read_file(first.log->file()).value();
  std::istringstream in(text);
  std::string line, cut;
  int starts = 0;
  while (std::getline(in, line)) {
    if (line.find("\"trial_start\"") != std::string::npos && ++starts == 9) {
      cut += line + "\n";
      break;
    }
    cut += line + "\n";
  }
  test::TempDir dir;
  const auto log2 = dir.path() / "tune.jsonl";
  ASSERT_TRUE(platform::write_file_atomic(log2, cut + "{\"type\":\"trial_res").ok()) << "torn last line";

  TunerRig second(14, spec);
  second.log = std::make_unique<TuningLog>(log2);
  auto r = second.run();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_TRUE(r.value().resumed);
  EXPECT_EQ(r.value().replayed_trials, 9) << "8 completed + 1 torn";
  bool crash_noted = false;
  for (const auto& n : r.value().notes) crash_noted |= n.find("crash") != std::string::npos;
  EXPECT_TRUE(crash_noted);
  EXPECT_TRUE(r.value().validated);
  // A finished session is not resumed again.
  auto again = second.log->find_resumable(second.rig.id.uuid, "566.36 (mock)",
                                          tuning_params_hash(second.opts.tuning, second.opts.objective,
                                                             second.rig.policy->config()));
  EXPECT_FALSE(again.value().has_value());
}

TEST(TunerResume, DriverChangeStartsANewSession) {
  TunerRig t(15);
  auto tuner = t.make();
  ASSERT_TRUE(tuner.begin().ok());
  const auto hash = tuning_params_hash(t.opts.tuning, t.opts.objective, t.rig.policy->config());
  EXPECT_TRUE(t.log->find_resumable(t.rig.id.uuid, "566.36 (mock)", hash).value().has_value());
  EXPECT_FALSE(t.log->find_resumable(t.rig.id.uuid, "570.00", hash).value().has_value());
  EXPECT_FALSE(t.log->find_resumable(t.rig.id.uuid, "566.36 (mock)", "other").value().has_value());
}

// ---- Re-tune policy.

TEST(RetunePolicy, Triggers) {
  LkgEntry e;
  e.gpu_uuid = "GPU-a";
  e.driver_version = "566.36";
  e.validated = true;
  e.validated_at = from_epoch_ms(1'800'000'000'000LL);
  RetuneInputs in;
  in.lkg = e;
  in.gpu_uuid = "GPU-a";
  in.driver_version = "566.36";
  in.now = e.validated_at + std::chrono::hours(24 * 3);
  EXPECT_FALSE(retune_reason(in).has_value());
  auto t = in;
  t.lkg.reset();
  EXPECT_TRUE(retune_reason(t).has_value());
  t = in;
  t.gpu_uuid = "GPU-b";
  EXPECT_TRUE(retune_reason(t).has_value());
  t = in;
  t.driver_version = "570.10";
  EXPECT_NE(retune_reason(t)->find("driver"), std::string::npos);
  t = in;
  t.now = e.validated_at + std::chrono::hours(24 * 15);
  EXPECT_NE(retune_reason(t)->find("14 days"), std::string::npos);
  t.revalidate_days = 30;
  EXPECT_FALSE(retune_reason(t).has_value());
  t = in;
  t.repeated_instability = true;
  EXPECT_TRUE(retune_reason(t).has_value());
  t = in;
  t.lkg->revoked = true;
  EXPECT_TRUE(retune_reason(t).has_value());
}

}  // namespace
}  // namespace gpuoc
