// Runs the deterministic workload for a set duration and returns a verdict.
#pragma once

#include <atomic>
#include <memory>

#include "bench/workload.hpp"
#include "gpuoc/clock.hpp"
#include "gpuoc/gpu.hpp"
#include "stress/event_log.hpp"
#include "stress/verdict.hpp"

namespace gpuoc {

struct StressOptions {
  Sec duration{120};
  Ms poll{1000};
  // Consecutive failed telemetry polls that make the run inconclusive.
  int max_consecutive_telemetry_failures = 3;
};

struct StressReport {
  Verdict verdict;
  StressSignals signals;
  double score = 0;  // work / second over the run (informational)
};

class StressRunner {
 public:
  StressRunner(WorkloadFactory workloads, std::shared_ptr<IEventLogSource> events, IClock& clock);
  StressReport run(Gpu& gpu, const StressOptions& opts, const std::atomic<bool>* abort);

 private:
  WorkloadFactory workloads_;
  std::shared_ptr<IEventLogSource> events_;
  IClock& clock_;
};

}  // namespace gpuoc
