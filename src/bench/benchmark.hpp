// Benchmarks: a score, its unit, and time statistics.
// Protocol: warm-up, then N measured runs; report the median and the
// coefficient of variation (CV). Invalid if CV exceeds the limit or the GPU
// hit a limit during the run.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "bench/workload.hpp"
#include "core/config.hpp"
#include "gpuoc/clock.hpp"
#include "gpuoc/gpu.hpp"

namespace gpuoc {

struct LimitTracker;

struct BenchmarkStats {
  std::vector<double> run_scores;
  double median = 0;
  double mean = 0;
  double cv_pct = 0;
  double min = 0;
  double max = 0;
  double iter_ms_mean = 0;  // per iteration (frame) time
  double iter_ms_p99 = 0;
};

struct BenchmarkResult {
  std::string name;
  std::string unit;
  bool valid = false;
  std::string invalid_reason;
  double score = 0;  // median of the measured runs
  BenchmarkStats stats;
  double avg_power_w = 0;
  double max_temp_c = 0;
  std::uint32_t limit_flags = 0;
  double perf_per_watt = 0;  // score / avg_power_w
};

struct BenchmarkProtocol {
  Sec warmup{30};
  int runs = 3;
  Sec run_duration{10};
  double max_cv_pct = 3.0;
  bool invalid_on_power_cap = false;
  static BenchmarkProtocol from(const BenchmarkConfig& c) {
    return BenchmarkProtocol{c.warmup, c.runs, c.run_duration, c.max_cv_pct, c.invalid_on_power_cap};
  }
};

class IBenchmark {
 public:
  virtual ~IBenchmark() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  virtual Result<BenchmarkResult> run(Gpu& gpu, const std::atomic<bool>* abort) = 0;
};

// Built-in: throughput of an IWorkload (the deterministic Vulkan kernel, or
// the mock kernel in tests).
class WorkloadBenchmark final : public IBenchmark {
 public:
  WorkloadBenchmark(WorkloadFactory factory, IClock& clock, BenchmarkProtocol protocol);
  [[nodiscard]] std::string name() const override { return "builtin"; }
  Result<BenchmarkResult> run(Gpu& gpu, const std::atomic<bool>* abort) override;

 private:
  WorkloadFactory factory_;
  IClock& clock_;
  BenchmarkProtocol p_;
};

// External: any command; the score is parsed from stdout with a regex
// (first capture group if present, else the whole match).
class ExternalBenchmark final : public IBenchmark {
 public:
  ExternalBenchmark(std::string command, std::string score_regex, std::string unit, Sec timeout, IClock& clock,
                    BenchmarkProtocol protocol);
  [[nodiscard]] std::string name() const override { return "external"; }
  Result<BenchmarkResult> run(Gpu& gpu, const std::atomic<bool>* abort) override;
  static Result<double> parse_score(const std::string& output, const std::string& score_regex);

 private:
  struct RunOutput {
    double score = 0;
    double seconds = 0;
  };
  Result<RunOutput> run_once(Gpu& gpu, LimitTracker& tracker);

  std::string command_;
  std::string regex_;
  std::string unit_;
  Sec timeout_;
  IClock& clock_;
  BenchmarkProtocol p_;
};

std::unique_ptr<IBenchmark> make_benchmark(const BenchmarkConfig& cfg, WorkloadFactory builtin, IClock& clock);

double median_of(std::vector<double> v);
double cv_pct_of(const std::vector<double>& v);

}  // namespace gpuoc
