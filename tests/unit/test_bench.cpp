// Benchmarks, the deterministic kernel, the worker process, external commands.
#include <gtest/gtest.h>

#include "bench/benchmark.hpp"
#include "bench/kernel_reference.hpp"
#include "bench/process_workload.hpp"
#include "bench/vulkan_kernel.hpp"
#include "platform/process.hpp"
#include "stress/stress_runner.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

TEST(Stats, MedianAndCv) {
  EXPECT_DOUBLE_EQ(median_of({3, 1, 2}), 2);
  EXPECT_DOUBLE_EQ(median_of({4, 1, 2, 3}), 2.5);
  EXPECT_DOUBLE_EQ(cv_pct_of({10, 10, 10}), 0);
  EXPECT_NEAR(cv_pct_of({9, 10, 11}), 10.0, 1e-9);
}

// ---- Kernel reference -----------------------------------------------------------

TEST(KernelReference, DeterministicAndSensitive) {
  KernelParams p{1u << 12, 16, 3};
  EXPECT_EQ(reference_checksum(p), 0xed2caa0d84286b93ull);
  KernelParams q = p;
  q.iters = 17;
  EXPECT_NE(reference_checksum(q), reference_checksum(p));
  EXPECT_FALSE(validate(KernelParams{1000, 16, 3}).ok()) << "not a power of two";
  EXPECT_FALSE(validate(KernelParams{1u << 12, 0, 3}).ok());
  EXPECT_TRUE(validate(KernelParams{}).ok());
}

TEST(KernelReference, DefaultConstantMatchesCpuReference) {
#ifndef GPUOC_OPTIMIZED_BUILD
  if (platform::getenv_utf8("GPUOC_SLOW_TESTS") != "1")
    GTEST_SKIP() << "~10 s in Release; set GPUOC_SLOW_TESTS=1 to run in this build type";
#endif
  EXPECT_EQ(reference_checksum(KernelParams{}), kDefaultReferenceChecksum);
  EXPECT_EQ(known_checksum(KernelParams{}), kDefaultReferenceChecksum);
}

// ---- Vulkan kernel (runs on any Vulkan device, e.g. lavapipe; skipped without one)

TEST(VulkanKernel, MatchesCpuReference) {
  auto devs = VulkanKernel::list_devices();
  if (!devs || devs.value().empty()) GTEST_SKIP() << "no Vulkan device: " << (devs ? "none" : devs.error().message);
  KernelParams p{1u << 14, 32, 5};
  DeviceSelector sel;
  sel.index = 0;
  auto k = VulkanKernel::create(sel, p);
  ASSERT_TRUE(k.ok()) << k.error().message;
  const std::uint64_t want = reference_checksum(p);
  for (int i = 0; i < 3; ++i) {
    auto r = k.value()->run_once(std::chrono::seconds(60));
    ASSERT_EQ(r.status, KernelRunResult::Status::Ok) << r.detail;
    EXPECT_EQ(r.checksum, want);
  }
  EXPECT_FALSE(k.value()->device().uuid.empty());
}

TEST(VulkanKernel, SelectsByUuid) {
  auto devs = VulkanKernel::list_devices();
  if (!devs || devs.value().empty()) GTEST_SKIP() << "no Vulkan device";
  DeviceSelector sel;
  sel.uuid = devs.value()[0].uuid;
  auto k = VulkanKernel::create(sel, KernelParams{1u << 10, 4, 2});
  ASSERT_TRUE(k.ok()) << k.error().message;
  EXPECT_EQ(k.value()->device().uuid, sel.uuid);
  sel.uuid = "GPU-00000000-0000-0000-0000-000000000000";
  EXPECT_EQ(VulkanKernel::create(sel, KernelParams{1u << 10, 4, 2}).error().code, ErrorCode::NotFound);
}

TEST(VulkanKernel, UuidFormatMatchesNvml) {
  std::array<std::uint8_t, 16> b{};
  for (int i = 0; i < 16; ++i) b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(0xa0 + i);
  EXPECT_EQ(format_uuid(b), "GPU-a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf");
}

// ---- Worker process plumbing (no GPU needed: --simulate) -------------------------

WorkerOptions sim_worker(const std::string& mode) {
  WorkerOptions o;
  o.worker_path = GPUOC_WORKER_PATH;
  o.hang_timeout = Sec(2);
  o.startup_timeout = Sec(30);
  o.extra_args = {"--simulate", mode};
  return o;
}

IterationStatus run_until_failure(const std::string& mode, int max_iters = 10) {
  ProcessWorkload wl(GpuId{"GPU-test", "sim", 0}, sim_worker(mode));
  auto s = wl.start();
  EXPECT_TRUE(s.ok()) << s.error().message;
  if (!s) return IterationStatus::StartFailed;
  for (int i = 0; i < max_iters; ++i) {
    auto r = wl.run_iteration();
    if (r.status != IterationStatus::Ok) return r.status;
  }
  return IterationStatus::Ok;
}

TEST(WorkerProcess, Ok) { EXPECT_EQ(run_until_failure("ok"), IterationStatus::Ok); }
TEST(WorkerProcess, MismatchReported) { EXPECT_EQ(run_until_failure("mismatch"), IterationStatus::ChecksumMismatch); }
TEST(WorkerProcess, CrashDetected) {
  const auto st = run_until_failure("crash");
  if (test::running_under_wine() && st == IterationStatus::ExitedNonZero) GTEST_SKIP() << "wine exit-code semantics";
  EXPECT_EQ(st, IterationStatus::Crashed);
}
TEST(WorkerProcess, HangDetected) { EXPECT_EQ(run_until_failure("hang"), IterationStatus::Hang); }
TEST(WorkerProcess, NonZeroExitDetected) { EXPECT_EQ(run_until_failure("exit3"), IterationStatus::ExitedNonZero); }
TEST(WorkerProcess, DeviceLostDetected) { EXPECT_EQ(run_until_failure("device_lost"), IterationStatus::DeviceLost); }

TEST(WorkerProcess, MissingWorkerFailsToStart) {
  WorkerOptions o = sim_worker("ok");
  o.worker_path = "/nonexistent/gpuoc-worker-missing";
  ProcessWorkload wl(GpuId{}, o);
  EXPECT_FALSE(wl.start().ok());
}

TEST(WorkerProcess, StressRunnerMapsCrashToUnstable) {
  auto rig = make_rig(1, permissive_safety());
  StressRunner runner(make_worker_factory(sim_worker("crash")), nullptr, SystemClock::instance());
  StressOptions o;
  o.duration = Sec(20);
  auto rep = runner.run(*rig.gpu, o, nullptr);
  EXPECT_EQ(rep.verdict.kind, VerdictKind::Unstable);
  if (!test::running_under_wine()) {
    EXPECT_TRUE(rep.signals.crashed) << rep.verdict.to_string();
  }
}

TEST(WorkerProcess, RealKernelOnVulkanDevice) {
  auto devs = VulkanKernel::list_devices();
  if (!devs || devs.value().empty()) GTEST_SKIP() << "no Vulkan device";
  WorkerOptions o;
  o.worker_path = GPUOC_WORKER_PATH;
  o.params = KernelParams{1u << 12, 16, 3};
  o.extra_args = {"--index", "0", "--repeat", "2"};
  ProcessWorkload wl(GpuId{}, o);
  ASSERT_TRUE(wl.start().ok());
  for (int i = 0; i < 3; ++i) {
    auto r = wl.run_iteration();
    ASSERT_EQ(r.status, IterationStatus::Ok) << r.detail;
    EXPECT_GT(r.work, 0);
  }
}

TEST(WindowsCommandLine, Quoting) {
  EXPECT_EQ(platform::quote_windows_arg("plain"), "plain");
  EXPECT_EQ(platform::quote_windows_arg("two words"), "\"two words\"");
  EXPECT_EQ(platform::quote_windows_arg(""), "\"\"");
  EXPECT_EQ(platform::quote_windows_arg("a\"b"), "\"a\\\"b\"");
  EXPECT_EQ(platform::quote_windows_arg("C:\\dir with space\\"), "\"C:\\dir with space\\\\\"");
  EXPECT_EQ(platform::build_windows_command_line({"C:\\a b\\x.exe", "--uuid", "GPU-1"}),
            "\"C:\\a b\\x.exe\" --uuid GPU-1");
}

// ---- Benchmark protocol on the mock ---------------------------------------------

BenchmarkProtocol fast_protocol() { return BenchmarkProtocol{Sec(30), 3, Sec(10), 3.0, false}; }

TEST(Benchmark, MedianOfThreeRunsAfterWarmup) {
  auto rig = make_rig(4, permissive_safety());
  WorkloadBenchmark b(rig.backend->workload_factory(), *rig.clock, fast_protocol());
  const auto before = rig.clock->elapsed();
  auto r = b.run(*rig.gpu, nullptr);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_TRUE(r.value().valid) << r.value().invalid_reason;
  EXPECT_EQ(r.value().stats.run_scores.size(), 3u);
  EXPECT_GT(r.value().score, 0);
  EXPECT_LT(r.value().stats.cv_pct, 3.0);
  EXPECT_EQ(r.value().unit, "Gop/s");
  EXPECT_GT(r.value().avg_power_w, 0);
  EXPECT_GE(rig.clock->elapsed() - before, Sec(60)) << "30 s warm-up + 3 x 10 s";
}

TEST(Benchmark, HigherClocksScoreHigher) {
  auto rig = make_rig(4, permissive_safety());
  WorkloadBenchmark b(rig.backend->workload_factory(), *rig.clock, fast_protocol());
  const double stock = b.run(*rig.gpu, nullptr).value().score;
  Settings s;
  s.core_offset_mhz = 150;
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  const double oc = b.run(*rig.gpu, nullptr).value().score;
  EXPECT_GT(oc, stock * 1.02);
}

TEST(Benchmark, InvalidWhenCvTooHigh) {
  mock::MockGpuSpec spec;
  spec.score_noise_pct = 40;
  auto rig = make_rig(4, permissive_safety(), spec);
  WorkloadBenchmark b(rig.backend->workload_factory(), *rig.clock, fast_protocol());
  auto r = b.run(*rig.gpu, nullptr).value();
  EXPECT_FALSE(r.valid);
  EXPECT_NE(r.invalid_reason.find("CV"), std::string::npos);
}

TEST(Benchmark, InvalidWhenGpuHitsALimit) {
  mock::MockGpuSpec spec;
  spec.ambient_c = 70;  // runs into thermal throttling
  auto rig = make_rig(4, permissive_safety(), spec);
  WorkloadBenchmark b(rig.backend->workload_factory(), *rig.clock, fast_protocol());
  auto r = b.run(*rig.gpu, nullptr).value();
  EXPECT_FALSE(r.valid);
  EXPECT_FALSE(r.invalid_reason.empty());
}

TEST(Benchmark, InvalidOnPowerCapWhenConfigured) {
  auto rig = make_rig(4, permissive_safety());
  Settings s;
  s.power_limit_w = 160;  // well below demand
  ASSERT_TRUE(rig.gpu->apply(s).ok());
  auto p = fast_protocol();
  WorkloadBenchmark lenient(rig.backend->workload_factory(), *rig.clock, p);
  EXPECT_TRUE(lenient.run(*rig.gpu, nullptr).value().valid);
  p.invalid_on_power_cap = true;
  WorkloadBenchmark strict(rig.backend->workload_factory(), *rig.clock, p);
  auto r = strict.run(*rig.gpu, nullptr).value();
  EXPECT_FALSE(r.valid);
  EXPECT_NE(r.invalid_reason.find("power"), std::string::npos);
}

TEST(Benchmark, WorkloadFailureIsAnError) {
  auto rig = make_rig(4, permissive_safety());
  rig.backend->faults(0).crash_next = true;
  WorkloadBenchmark b(rig.backend->workload_factory(), *rig.clock, fast_protocol());
  EXPECT_FALSE(b.run(*rig.gpu, nullptr).ok());
}

// ---- External benchmark ----------------------------------------------------------

TEST(ExternalBenchmark, ParseScore) {
  EXPECT_DOUBLE_EQ(ExternalBenchmark::parse_score("fps: 143.25\n", R"(fps:\s*([0-9.]+))").value(), 143.25);
  EXPECT_DOUBLE_EQ(ExternalBenchmark::parse_score("Score 9001 points", R"(\d+)").value(), 9001);
  EXPECT_EQ(ExternalBenchmark::parse_score("nothing", R"(fps: (\d+))").error().code, ErrorCode::ParseError);
  EXPECT_EQ(ExternalBenchmark::parse_score("x", "(unclosed").error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(ExternalBenchmark::parse_score("fps: abc", R"(fps: (\w+))").error().code, ErrorCode::ParseError);
}

std::string echo_command(const std::string& text) {
#ifdef _WIN32
  return "cmd.exe /c echo " + text;
#else
  return "echo '" + text + "'";
#endif
}

TEST(ExternalBenchmark, RunsCommandAndParses) {
  auto rig = make_rig(4, permissive_safety());
  ExternalBenchmark b(echo_command("result score=321.5 fps"), R"(score=([0-9.]+))", "fps", Sec(60),
                      SystemClock::instance(), BenchmarkProtocol{Sec(0), 3, Sec(1), 3.0, false});
  auto r = b.run(*rig.gpu, nullptr);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_DOUBLE_EQ(r.value().score, 321.5);
  EXPECT_EQ(r.value().stats.run_scores.size(), 3u);
  EXPECT_TRUE(r.value().valid);
  EXPECT_EQ(r.value().unit, "fps");
}

TEST(ExternalBenchmark, NonZeroExitIsAnError) {
  auto rig = make_rig(4, permissive_safety());
#ifdef _WIN32
  const std::string cmd = "cmd.exe /c exit 7";
#else
  const std::string cmd = "exit 7";
#endif
  ExternalBenchmark b(cmd, R"(\d+)", "x", Sec(30), SystemClock::instance(),
                      BenchmarkProtocol{Sec(0), 1, Sec(1), 3.0, false});
  EXPECT_FALSE(b.run(*rig.gpu, nullptr).ok());
}

}  // namespace
}  // namespace gpuoc
