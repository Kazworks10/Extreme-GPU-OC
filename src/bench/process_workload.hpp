// IWorkload backed by the gpuoc-worker child process.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "bench/kernel_reference.hpp"
#include "bench/workload.hpp"
#include "gpuoc/clock.hpp"
#include "platform/process.hpp"

namespace gpuoc {

struct WorkerOptions {
  std::string worker_path;  // default: gpuoc-worker(.exe) next to the running executable
  KernelParams params;
  Sec hang_timeout{30};
  Sec startup_timeout{180};
  std::vector<std::string> extra_args;  // e.g. {"--simulate", "crash"} in tests
};

std::string default_worker_path();

class ProcessWorkload final : public IWorkload {
 public:
  ProcessWorkload(GpuId gpu, WorkerOptions opts);
  ~ProcessWorkload() override;
  [[nodiscard]] std::string name() const override { return "vulkan-kernel"; }
  [[nodiscard]] std::string unit() const override { return "Gop/s"; }
  Result<void> start() override;
  IterationResult run_iteration() override;
  void stop() override;
  [[nodiscard]] const std::string& device_name() const noexcept { return device_; }

 private:
  IterationResult from_exit();

  GpuId gpu_;
  WorkerOptions opts_;
  std::unique_ptr<platform::ChildProcess> child_;
  std::string device_;
};

WorkloadFactory make_worker_factory(WorkerOptions opts);

}  // namespace gpuoc
