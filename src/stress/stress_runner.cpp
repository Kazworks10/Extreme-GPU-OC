#include "stress/stress_runner.hpp"

#include <algorithm>
#include <format>

#include "core/heartbeat.hpp"
#include "core/log.hpp"

namespace gpuoc {

const char* to_string(VerdictKind k) noexcept {
  switch (k) {
    case VerdictKind::Stable: return "stable";
    case VerdictKind::Unstable: return "unstable";
    case VerdictKind::Inconclusive: return "inconclusive";
  }
  return "?";
}

std::string Verdict::to_string() const {
  return reason.empty() ? std::string(gpuoc::to_string(kind)) : std::format("{} ({})", gpuoc::to_string(kind), reason);
}

Verdict evaluate_stress(const StressSignals& s) {
  auto detail = [&](const char* base) {
    for (const auto& d : s.details) {
      if (d.starts_with(base)) return d;
    }
    return std::string(base);
  };
  if (s.checksum_mismatch) return Verdict::unstable(detail("checksum mismatch"));
  if (s.crashed) return Verdict::unstable(detail("workload crashed"));
  if (s.exited_nonzero) return Verdict::unstable(detail("workload exited with an error"));
  if (s.device_lost) return Verdict::unstable(detail("GPU lost"));
  if (s.hang) return Verdict::unstable(detail("GPU hang"));
  if (s.driver_event) return Verdict::unstable(detail("driver reset"));
  if (s.ecc_increase) return Verdict::unstable(detail("uncorrected ECC errors"));
  if (s.temp_exceeded) return Verdict::unstable(detail("temperature limit"));
  if (s.power_exceeded) return Verdict::unstable(detail("power limit"));
  if (s.start_failed) return Verdict::inconclusive(detail("workload could not start"));
  if (s.telemetry_missing) return Verdict::inconclusive(detail("telemetry missing"));
  if (s.interrupted) return Verdict::inconclusive(detail("interrupted"));
  if (s.iterations == 0) return Verdict::inconclusive("no iterations completed");
  return Verdict::stable();
}

StressRunner::StressRunner(WorkloadFactory workloads, std::shared_ptr<IEventLogSource> events, IClock& clock)
    : workloads_(std::move(workloads)), events_(std::move(events)), clock_(clock) {}

StressReport StressRunner::run(Gpu& gpu, const StressOptions& opts, const std::atomic<bool>* abort) {
  StressReport rep;
  StressSignals& s = rep.signals;
  auto note = [&](std::string d) {
    log::debug("[{}] stress: {}", gpu.id().uuid, d);
    s.details.push_back(std::move(d));
  };

  const WallTime start_wall = clock_.wall_now();
  const SteadyTime start = clock_.now();
  auto limits = gpu.limits();
  if (!limits) {
    s.start_failed = true;
    note("workload could not start: " + limits.error().message);
    rep.verdict = evaluate_stress(s);
    return rep;
  }
  if (auto t = gpu.telemetry()) s.ecc_start = s.ecc_end = t.value().ecc_uncorrected;

  auto wl_res = workloads_(gpu.id());
  std::unique_ptr<IWorkload> wl;
  if (wl_res) wl = std::move(wl_res).value();
  Result<void> started = wl ? wl->start() : Result<void>(wl_res.error());
  if (!started) {
    s.start_failed = true;
    note("workload could not start: " + started.error().message);
    rep.verdict = evaluate_stress(s);
    return rep;
  }

  const SteadyTime end = start + opts.duration;
  SteadyTime next_poll = start;
  int consecutive_fail = 0;
  double power_sum = 0;
  bool stop = false;

  while (!stop && clock_.now() < end) {
    if (abort && abort->load()) {
      s.interrupted = true;
      note("interrupted by abort request");
      break;
    }
    const IterationResult it = wl->run_iteration();
    heartbeat::beat();
    switch (it.status) {
      case IterationStatus::Ok:
        ++s.iterations;
        s.work += it.work;
        s.seconds += it.seconds;
        break;
      case IterationStatus::ChecksumMismatch:
        s.checksum_mismatch = true;
        note("checksum mismatch: " + it.detail);
        stop = true;
        break;
      case IterationStatus::DeviceLost:
        s.device_lost = true;
        note("GPU lost: " + it.detail);
        stop = true;
        break;
      case IterationStatus::Crashed:
        s.crashed = true;
        note("workload crashed: " + it.detail);
        stop = true;
        break;
      case IterationStatus::ExitedNonZero:
        s.exited_nonzero = true;
        note("workload exited with an error: " + it.detail);
        stop = true;
        break;
      case IterationStatus::Hang:
        s.hang = true;
        note("GPU hang: " + it.detail);
        stop = true;
        break;
      case IterationStatus::Interrupted:
        s.interrupted = true;
        note("interrupted: " + it.detail);
        stop = true;
        break;
      case IterationStatus::StartFailed:
        s.start_failed = true;
        note("workload could not start: " + it.detail);
        stop = true;
        break;
    }
    if (stop) break;

    if (clock_.now() >= next_poll) {
      while (next_poll <= clock_.now()) next_poll += opts.poll;
      auto t = gpu.telemetry();
      if (!t) {
        ++s.telemetry_failures;
        if (t.error().code == ErrorCode::GpuLost) {
          s.device_lost = true;
          note("GPU lost: " + t.error().message);
          break;
        }
        if (++consecutive_fail >= opts.max_consecutive_telemetry_failures && !s.telemetry_missing) {
          s.telemetry_missing = true;
          note("telemetry missing: " + t.error().message);
          break;  // cannot verify temperature or power; stop loading the GPU
        }
        continue;
      }
      consecutive_fail = 0;
      const Telemetry& v = t.value();
      ++s.telemetry_samples;
      power_sum += v.power_w;
      s.max_temp_c = std::max(s.max_temp_c, v.temp_c);
      s.max_power_w = std::max(s.max_power_w, v.power_w);
      s.limit_flags |= v.limit_flags;
      s.ecc_end = v.ecc_uncorrected;
      if (v.ecc_uncorrected > s.ecc_start) {
        s.ecc_increase = true;
        note(std::format("uncorrected ECC errors: {} -> {}", s.ecc_start, v.ecc_uncorrected));
        break;
      }
      const EffectiveLimits& lim = limits.value();
      if (v.temp_c > lim.max_temp_c) {
        s.temp_exceeded = true;
        note(std::format("temperature limit: {:.1f} C > {:.1f} C", v.temp_c, lim.max_temp_c));
        break;
      }
      if (lim.power_allowed && v.power_w > lim.power_limit_w.max * (1.0 + HardLimits::kPowerViolationTolerance)) {
        s.power_exceeded = true;
        note(std::format("power limit: {:.1f} W > {} W", v.power_w, lim.power_limit_w.max));
        break;
      }
    }
  }
  wl->stop();
  if (s.telemetry_samples == 0 && !s.telemetry_missing && s.iterations > 0) {
    s.telemetry_missing = true;
    note("telemetry missing: no successful sample");
  }
  if (s.telemetry_samples > 0) s.avg_power_w = power_sum / s.telemetry_samples;

  if (events_ && events_->available()) {
    auto ev = events_->driver_events_since(start_wall);
    if (ev) {
      for (const auto& e : ev.value()) {
        if (is_driver_failure(e)) {
          s.driver_event = true;
          note(std::format("driver reset: {} event {} ({})", e.provider, e.event_id, e.message));
          break;
        }
      }
    } else {
      log::warn("[{}] event log query failed: {}", gpu.id().uuid, ev.error().message);
    }
  }

  rep.score = s.seconds > 0 ? s.work / s.seconds : 0;
  rep.verdict = evaluate_stress(s);
  log::info("[{}] stress {} s: {} after {} iterations (max {:.1f} C, {:.0f} W)", gpu.id().uuid, opts.duration.count(),
            rep.verdict.to_string(), s.iterations, s.max_temp_c, s.max_power_w);
  return rep;
}

}  // namespace gpuoc
