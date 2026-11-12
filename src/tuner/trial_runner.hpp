// A trial applies full settings, then stress-tests (or benchmarks) them.
#pragma once

#include <atomic>
#include <memory>

#include "bench/benchmark.hpp"
#include "gpuoc/gpu.hpp"
#include "stress/stress_runner.hpp"

namespace gpuoc {

struct TrialResult {
  Verdict verdict;
  double score = 0;
  double avg_power_w = 0;
  double max_temp_c = 0;
  std::uint32_t limit_flags = 0;
  bool bench_valid = false;
  bool safety_violation = false;  // temperature or power limit exceeded during the trial
  std::string unit;
};

class ITrialRunner {
 public:
  virtual ~ITrialRunner() = default;
  virtual TrialResult stress(Gpu& gpu, const Settings& settings, Sec duration, const std::atomic<bool>* abort) = 0;
  virtual TrialResult benchmark(Gpu& gpu, const Settings& settings, const std::atomic<bool>* abort) = 0;
};

// Production runner: StressRunner + IBenchmark.
class StressTrialRunner final : public ITrialRunner {
 public:
  StressTrialRunner(std::shared_ptr<StressRunner> stress, std::shared_ptr<IBenchmark> bench, Ms poll);
  TrialResult stress(Gpu& gpu, const Settings& settings, Sec duration, const std::atomic<bool>* abort) override;
  TrialResult benchmark(Gpu& gpu, const Settings& settings, const std::atomic<bool>* abort) override;

 private:
  std::shared_ptr<StressRunner> stress_;
  std::shared_ptr<IBenchmark> bench_;
  Ms poll_;
};

}  // namespace gpuoc
