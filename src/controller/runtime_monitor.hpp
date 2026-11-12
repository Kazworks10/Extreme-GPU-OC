// Runtime monitor: polls telemetry while a profile is applied.
//  - Hysteresis: above target temperature -> step the core offset down one
//    step; after 60 s more than 5 C below target -> step back up. Never above
//    the ceiling (the validated / requested value).
//  - Violations (temperature over max, power over max, driver failure event,
//    ECC error, GPU lost, telemetry gone) are reported to the controller,
//    which reverts to stock immediately.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/config.hpp"
#include "gpuoc/clock.hpp"
#include "gpuoc/gpu.hpp"
#include "stress/event_log.hpp"

namespace gpuoc {

struct MonitorTick {
  enum class Action { None, StepDown, StepUp, Violation } action = Action::None;
  std::string violation_kind;  // see ViolationRecord::kind
  std::string reason;
  int core_offset = 0;         // current core offset after the action
  std::optional<Telemetry> telemetry;
};

class RuntimeMonitor {
 public:
  RuntimeMonitor(Gpu& gpu, IClock& clock, MonitorConfig cfg, int core_step_mhz,
                 std::shared_ptr<IEventLogSource> events);

  // Begin monitoring `active` settings. If `adjust` is false (monitor-only
  // mode) the monitor never writes; it only reports.
  void start(const Settings& active, bool adjust, std::optional<double> temp_target = std::nullopt);
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_; }
  MonitorTick tick();
  [[nodiscard]] int current_core_offset() const noexcept { return core_; }
  [[nodiscard]] int ceiling() const noexcept { return ceiling_; }

 private:
  Gpu& gpu_;
  IClock& clock_;
  MonitorConfig cfg_;
  int step_;
  std::shared_ptr<IEventLogSource> events_;
  bool running_ = false;
  bool adjust_ = false;
  bool has_core_ = false;
  int ceiling_ = 0;
  int core_ = 0;
  std::uint64_t ecc_start_ = 0;
  WallTime events_since_{};
  std::optional<SteadyTime> cool_since_;
  int telemetry_failures_ = 0;
  std::optional<double> temp_target_;
};

}  // namespace gpuoc
