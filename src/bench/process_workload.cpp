#include "bench/process_workload.hpp"

#include <format>
#include <nlohmann/json.hpp>

#include "core/heartbeat.hpp"
#include "core/log.hpp"
#include "platform/dynlib.hpp"

namespace gpuoc {

const char* to_string(IterationStatus s) noexcept {
  switch (s) {
    case IterationStatus::Ok: return "ok";
    case IterationStatus::ChecksumMismatch: return "checksum_mismatch";
    case IterationStatus::DeviceLost: return "device_lost";
    case IterationStatus::Crashed: return "crashed";
    case IterationStatus::ExitedNonZero: return "exited_nonzero";
    case IterationStatus::Hang: return "hang";
    case IterationStatus::Interrupted: return "interrupted";
    case IterationStatus::StartFailed: return "start_failed";
  }
  return "?";
}

std::string default_worker_path() {
#ifdef _WIN32
  return platform::current_exe_dir() + "\\gpuoc-worker.exe";
#else
  return platform::current_exe_dir() + "/gpuoc-worker";
#endif
}

ProcessWorkload::ProcessWorkload(GpuId gpu, WorkerOptions opts) : gpu_(std::move(gpu)), opts_(std::move(opts)) {
  if (opts_.worker_path.empty()) opts_.worker_path = default_worker_path();
}

ProcessWorkload::~ProcessWorkload() { stop(); }

Result<void> ProcessWorkload::start() {
  stop();
  std::vector<std::string> argv = {opts_.worker_path,
                                   "--elements", std::to_string(opts_.params.elements),
                                   "--iters", std::to_string(opts_.params.iters),
                                   "--passes", std::to_string(opts_.params.passes),
                                   "--timeout-ms", std::to_string(std::chrono::duration_cast<Ms>(opts_.hang_timeout).count())};
  if (!gpu_.uuid.empty()) {
    argv.push_back("--uuid");
    argv.push_back(gpu_.uuid);
  }
  for (const auto& a : opts_.extra_args) argv.push_back(a);
  GPUOC_TRY_ASSIGN(child_, platform::ChildProcess::spawn(argv));
  const auto deadline = std::chrono::steady_clock::now() + opts_.startup_timeout;
  for (;;) {
    // Wait in short slices so the service watchdog keeps being fed while the
    // worker compiles its pipeline or computes a CPU reference.
    auto line = child_->read_line(Ms(1000));
    heartbeat::beat();
    if (!line) {
      if (std::chrono::steady_clock::now() < deadline) continue;
      child_->kill();
      return make_error(ErrorCode::Timeout, "worker did not become ready in time");
    }
    if (!line.value()) {
      auto e = child_->wait(Ms(5000));
      return make_error(ErrorCode::Unavailable,
                        std::format("worker exited during startup ({})", e ? e->description : "still running"));
    }
    auto j = nlohmann::json::parse(*line.value(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;
    const std::string ev = j.value("event", std::string());
    if (ev == "ready") {
      device_ = j.value("device", std::string());
      log::debug("[{}] worker ready on '{}' reference {}", gpu_.uuid, device_, j.value("reference", std::string()));
      return success();
    }
    if (ev == "error") {
      child_->kill();
      return make_error(ErrorCode::Unavailable, "worker: " + j.value("message", std::string("startup failed")));
    }
  }
}

IterationResult ProcessWorkload::from_exit() {
  IterationResult r;
  auto e = child_->wait(Ms(5000));
  if (!e) {
    child_->kill();
    r.status = IterationStatus::Hang;
    r.detail = "worker closed its output but did not exit";
    return r;
  }
  if (e->crashed) {
    r.status = IterationStatus::Crashed;
  } else if (e->code == 4) {
    r.status = IterationStatus::DeviceLost;
  } else if (e->code == 5) {
    r.status = IterationStatus::Hang;
  } else {
    r.status = IterationStatus::ExitedNonZero;
  }
  r.detail = "worker " + e->description;
  return r;
}

IterationResult ProcessWorkload::run_iteration() {
  IterationResult r;
  if (!child_) {
    r.status = IterationStatus::StartFailed;
    r.detail = "worker not started";
    return r;
  }
  for (;;) {
    auto line = child_->read_line(std::chrono::duration_cast<Ms>(opts_.hang_timeout));
    if (!line) {
      child_->kill();
      r.status = IterationStatus::Hang;
      r.detail = std::format("no progress from worker for {} s", opts_.hang_timeout.count());
      return r;
    }
    if (!line.value()) return from_exit();
    auto j = nlohmann::json::parse(*line.value(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;
    const std::string ev = j.value("event", std::string());
    if (ev == "iteration") {
      r.seconds = j.value("seconds", 0.0);
      r.work = j.value("work", 0.0);
      if (!j.value("ok", false)) {
        r.status = IterationStatus::ChecksumMismatch;
        r.detail = std::format("checksum {} != reference {}", j.value("checksum", std::string("?")),
                               j.value("expected", std::string("?")));
      }
      return r;
    }
    if (ev == "error") {
      const std::string kind = j.value("kind", std::string());
      r.status = kind == "device_lost" ? IterationStatus::DeviceLost
                 : kind == "hang"      ? IterationStatus::Hang
                                       : IterationStatus::ExitedNonZero;
      r.detail = "worker: " + j.value("message", std::string(kind));
      return r;
    }
  }
}

void ProcessWorkload::stop() {
  if (child_) {
    child_->kill();
    (void)child_->wait(Ms(5000));
    child_.reset();
  }
}

WorkloadFactory make_worker_factory(WorkerOptions opts) {
  return [opts](const GpuId& id) -> Result<std::unique_ptr<IWorkload>> {
    return std::unique_ptr<IWorkload>(std::make_unique<ProcessWorkload>(id, opts));
  };
}

}  // namespace gpuoc
