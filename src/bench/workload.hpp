// A GPU workload that can be run one iteration at a time. Used by both the
// benchmark (throughput) and the stress runner (correctness over time).
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "gpuoc/result.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc {

enum class IterationStatus {
  Ok,
  ChecksumMismatch,  // output differs from the reference: silent corruption
  DeviceLost,        // VK_ERROR_DEVICE_LOST / GPU fell off the bus / driver reset
  Crashed,           // worker process crashed
  ExitedNonZero,     // worker process exited with a non-zero code
  Hang,              // no progress within the hang timeout
  Interrupted,       // stopped by us (abort / shutdown)
  StartFailed,       // could not start (no Vulkan, worker missing, ...)
};

const char* to_string(IterationStatus s) noexcept;

struct IterationResult {
  IterationStatus status = IterationStatus::Ok;
  double seconds = 0;  // wall time of the iteration
  double work = 0;     // work units completed (unit: IWorkload::unit() x seconds)
  std::string detail;
};

class IWorkload {
 public:
  virtual ~IWorkload() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  // Unit of work / second, e.g. "Gop/s". The score of a run is work / seconds.
  [[nodiscard]] virtual std::string unit() const = 0;
  virtual Result<void> start() = 0;
  virtual IterationResult run_iteration() = 0;
  virtual void stop() = 0;
};

using WorkloadFactory = std::function<Result<std::unique_ptr<IWorkload>>(const GpuId&)>;

}  // namespace gpuoc
