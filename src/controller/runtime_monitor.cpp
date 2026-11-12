#include "controller/runtime_monitor.hpp"

#include <algorithm>
#include <format>

#include "core/heartbeat.hpp"
#include "core/log.hpp"

namespace gpuoc {

RuntimeMonitor::RuntimeMonitor(Gpu& gpu, IClock& clock, MonitorConfig cfg, int core_step_mhz,
                               std::shared_ptr<IEventLogSource> events)
    : gpu_(gpu), clock_(clock), cfg_(cfg), step_(std::max(1, core_step_mhz)), events_(std::move(events)) {}

void RuntimeMonitor::start(const Settings& active, bool adjust, std::optional<double> temp_target) {
  running_ = true;
  temp_target_ = temp_target;
  adjust_ = adjust;
  has_core_ = active.core_offset_mhz.has_value();
  ceiling_ = active.core_offset_mhz.value_or(0);
  core_ = ceiling_;
  cool_since_.reset();
  telemetry_failures_ = 0;
  events_since_ = clock_.wall_now();
  if (auto t = gpu_.telemetry()) ecc_start_ = t.value().ecc_uncorrected;
}

void RuntimeMonitor::stop() { running_ = false; }

MonitorTick RuntimeMonitor::tick() {
  heartbeat::beat();
  MonitorTick out;
  out.core_offset = core_;
  if (!running_) return out;

  auto violation = [&](std::string kind, std::string reason) {
    out.action = MonitorTick::Action::Violation;
    out.violation_kind = std::move(kind);
    out.reason = std::move(reason);
    return out;
  };

  auto t = gpu_.telemetry();
  if (!t) {
    if (t.error().code == ErrorCode::GpuLost) return violation("gpu_lost", t.error().message);
    if (++telemetry_failures_ >= 3) return violation("telemetry", "telemetry unavailable: " + t.error().message);
    return out;
  }
  telemetry_failures_ = 0;
  const Telemetry& v = t.value();
  out.telemetry = v;

  auto lim = gpu_.limits();
  if (!lim) return violation("telemetry", "limits unavailable: " + lim.error().message);
  if (auto why = gpu_.policy().telemetry_violation(v, lim.value())) {
    return violation(v.temp_c > lim.value().max_temp_c ? "temperature" : "power", *why);
  }
  if (v.ecc_uncorrected > ecc_start_)
    return violation("ecc", std::format("uncorrected ECC errors {} -> {}", ecc_start_, v.ecc_uncorrected));
  if (events_ && events_->available()) {
    const WallTime now = clock_.wall_now();
    if (auto ev = events_->driver_events_since(events_since_)) {
      for (const auto& e : ev.value()) {
        if (is_driver_failure(e)) return violation("driver_event", std::format("{} event {}", e.provider, e.event_id));
      }
      events_since_ = now;
    }
  }

  // Software fan curve upkeep (NVML exposes fixed speeds only).
  if (adjust_) {
    if (auto f = gpu_.maintain_fan(); !f) log::warn("[{}] fan upkeep: {}", gpu_.id().uuid, f.error().message);
  }

  // Hysteresis on the core offset.
  if (!adjust_ || !has_core_) return out;
  const double target = temp_target_ ? std::min(*temp_target_, lim.value().target_temp_c) : lim.value().target_temp_c;
  const int floor = std::min(0, ceiling_);
  if (v.temp_c > target) {
    cool_since_.reset();
    if (core_ > floor) {
      const int next = std::max(floor, core_ - step_);
      Settings s;
      s.core_offset_mhz = next;
      if (auto r = gpu_.apply(s); r) {
        log::info("[{}] {:.1f} C above target {:.1f} C: core {:+} -> {:+} MHz", gpu_.id().uuid, v.temp_c, target, core_, next);
        core_ = next;
        out.action = MonitorTick::Action::StepDown;
      } else {
        return violation("telemetry", "step-down write failed: " + r.error().message);
      }
    }
  } else if (v.temp_c < target - cfg_.hysteresis_delta_c) {
    if (!cool_since_) cool_since_ = clock_.now();
    if (core_ < ceiling_ && clock_.now() - *cool_since_ >= cfg_.hysteresis_hold) {
      const int next = std::min(ceiling_, core_ + step_);
      Settings s;
      s.core_offset_mhz = next;
      if (auto r = gpu_.apply(s); r) {
        log::info("[{}] cool for {} s: core {:+} -> {:+} MHz", gpu_.id().uuid, cfg_.hysteresis_hold.count(), core_, next);
        core_ = next;
        out.action = MonitorTick::Action::StepUp;
        cool_since_ = clock_.now();  // the next step needs another full hold period
      }
    }
  } else {
    cool_since_.reset();
  }
  out.core_offset = core_;
  return out;
}

}  // namespace gpuoc
