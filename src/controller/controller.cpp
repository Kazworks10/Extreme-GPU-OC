#include "controller/controller.hpp"

#include <format>

#include "core/heartbeat.hpp"
#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "tuner/retune_policy.hpp"

namespace gpuoc {

using nlohmann::json;

const char* to_string(ControllerState s) noexcept {
  switch (s) {
    case ControllerState::Discover: return "Discover";
    case ControllerState::Baseline: return "Baseline";
    case ControllerState::Tune: return "Tune";
    case ControllerState::Validate: return "Validate";
    case ControllerState::Commit: return "Commit";
    case ControllerState::Run: return "Run";
    case ControllerState::SafeRevert: return "SafeRevert";
    case ControllerState::Idle: return "Idle";
    case ControllerState::MonitorOnly: return "MonitorOnly";
    case ControllerState::Disabled: return "Disabled";
  }
  return "?";
}

namespace {
bool is_tuning(ControllerState s) {
  return s == ControllerState::Baseline || s == ControllerState::Tune || s == ControllerState::Validate ||
         s == ControllerState::Commit;
}
constexpr auto kDay = std::chrono::hours(24);
}  // namespace

GpuController::GpuController(Gpu& gpu, ControllerDeps deps)
    : gpu_(gpu), d_(std::move(deps)), objective_(d_.config->autotune.objective) {
  monitor_ = std::make_unique<RuntimeMonitor>(gpu_, *d_.clock, d_.config->monitor, d_.config->tuning.core_step_mhz,
                                              d_.events);
}

GpuController::~GpuController() = default;

Ms GpuController::poll_interval() const { return d_.config->monitor.poll; }

ControllerState GpuController::state() const {
  std::lock_guard lk(mu_);
  return state_;
}

std::optional<TuneOutcome> GpuController::last_outcome() const {
  std::lock_guard lk(mu_);
  return outcome_;
}

void GpuController::enter(ControllerState s, const std::string& why) {
  {
    std::lock_guard lk(mu_);
    if (state_ == s && state_reason_ == why) return;
    state_ = s;
    state_reason_ = why;
  }
  log::info("[{}] state -> {}{}", gpu_.id().uuid, to_string(s), why.empty() ? "" : " (" + why + ")");
}

Result<void> GpuController::startup() {
  guard_ = std::make_unique<StockGuard>(gpu_);
  // The restart path always reverts to stock before anything else: the GPU
  // may still hold settings from a process that crashed or was killed.
  auto r = gpu_.reset_to_stock();
  if (!r) log::error("[{}] startup stock restore failed: {}", gpu_.id().uuid, r.error().message);
  {
    std::lock_guard lk(mu_);
    applied_.reset();
  }
  enter(ControllerState::Discover, "startup");
  return r;
}

void GpuController::shutdown(const std::string& reason) {
  abort_.store(true);
  monitor_->stop();
  log::info("[{}] shutdown ({}): restoring stock", gpu_.id().uuid, reason);
  Result<void> r = guard_ ? guard_->restore() : gpu_.reset_to_stock();
  if (!r) log::error("[{}] stock restore on shutdown failed: {}", gpu_.id().uuid, r.error().message);
  std::lock_guard lk(mu_);
  applied_.reset();
}

bool GpuController::apply_target(const ActiveTarget& t) {
  auto r = gpu_.apply(t.settings, ClampMode::Reject);
  if (!r) {
    log::error("[{}] applying {} failed: {}", gpu_.id().uuid, t.source, r.error().message);
    (void)gpu_.reset_to_stock();
    std::lock_guard lk(mu_);
    applied_.reset();
    return false;
  }
  {
    std::lock_guard lk(mu_);
    applied_ = t;
  }
  monitor_->start(t.settings, /*adjust=*/true, t.temp_target_c);
  return true;
}

void GpuController::on_violation(const std::string& kind, const std::string& reason, bool settings_applied) {
  monitor_->stop();
  bool had_settings = settings_applied;
  {
    std::lock_guard lk(mu_);
    had_settings = had_settings || applied_.has_value();
  }
  if (auto r = gpu_.reset_to_stock(); !r) {
    log::error("[{}] revert after violation failed: {}; retrying via guard", gpu_.id().uuid, r.error().message);
    if (guard_) (void)guard_->restore();
  }
  {
    std::lock_guard lk(mu_);
    applied_.reset();
  }
  if (!had_settings) {
    // At stock there is nothing to revert; the driver's own limits apply.
    log::warn("[{}] limit exceeded at stock settings: {}", gpu_.id().uuid, reason);
    enter(ControllerState::MonitorOnly, "at stock: " + reason);
    monitor_->start(Settings{}, false);
    return;
  }
  log::error("[{}] runtime violation ({}): {}; reverted to stock", gpu_.id().uuid, kind, reason);
  const WallTime now = d_.clock->wall_now();
  const auto& mc = d_.config->monitor;
  bool disabled = false;
  bool revoke = false;
  (void)d_.state->update(gpu_.id().uuid, [&](GpuRuntimeState& s) {
    s.violations.push_back(ViolationRecord{now, kind, reason});
    if (s.instability_since(now - kDay) >= mc.instability_retune_threshold) revoke = true;
    if (s.violations_since(now - kDay) >= mc.max_violations_24h) {
      s.disabled = true;
      s.disabled_reason = std::format("{} violations in 24 h (last: {})", s.violations_since(now - kDay), reason);
      disabled = true;
    } else {
      s.cooldown_until = now + mc.cooldown;
    }
  });
  if (revoke) {
    log::warn("[{}] repeated instability: revoking the validated profile (forces a re-tune)", gpu_.id().uuid);
    (void)d_.lkg->revoke(gpu_.id().uuid, "repeated runtime instability");
  }
  enter(ControllerState::SafeRevert, reason);
  if (disabled) log::error("[{}] auto-overclock disabled until manually re-enabled", gpu_.id().uuid);
}

void GpuController::tune_failed(const std::string& why) {
  log::error("[{}] tuning did not produce a validated profile: {}", gpu_.id().uuid, why);
  (void)gpu_.reset_to_stock();
  (void)d_.state->update(gpu_.id().uuid, [&](GpuRuntimeState& s) {
    s.tune_failed_at = d_.clock->wall_now();
    s.tune_failure = why;
  });
  {
    std::lock_guard lk(mu_);
    applied_.reset();
  }
  monitor_->start(Settings{}, false);
  enter(ControllerState::MonitorOnly, "tuning failed: " + why);
}

void GpuController::step_discover() {
  const std::string uuid = gpu_.id().uuid;
  const GpuRuntimeState st = d_.state->get(uuid);
  const WallTime now = d_.clock->wall_now();
  if (st.disabled) {
    enter(ControllerState::Disabled, st.disabled_reason);
    return;
  }
  if (st.cooldown_until && now < *st.cooldown_until) {
    enter(ControllerState::Idle, "cooldown after violation");
    return;
  }
  std::optional<ActiveTarget> manual;
  bool retune = false;
  Objective objective;
  {
    std::lock_guard lk(mu_);
    manual = manual_;
    retune = retune_requested_;
    objective = objective_;
  }
  if (manual) {
    if (apply_target(*manual)) {
      enter(ControllerState::Run, manual->source);
    } else {
      enter(ControllerState::MonitorOnly, "could not apply " + manual->source);
      monitor_->start(Settings{}, false);
    }
    return;
  }

  auto caps = gpu_.capabilities();
  if (!caps) {
    enter(ControllerState::MonitorOnly, "capabilities unavailable: " + caps.error().message);
    monitor_->start(Settings{}, false);
    return;
  }
  const auto lkg = d_.lkg->get(uuid, objective);
  RetuneInputs in;
  in.lkg = lkg;
  in.gpu_uuid = uuid;
  in.driver_version = caps.value().driver_version;
  in.now = now;
  in.revalidate_days = d_.config->autotune.revalidate_days;
  in.repeated_instability = st.instability_since(now - kDay) >= d_.config->monitor.instability_retune_threshold;
  std::optional<std::string> reason = retune_reason(in);
  if (retune) reason = "re-tune requested";

  const bool ack = gpu_.policy().autonomous_writes_permitted();
  if (!reason && lkg) {
    if (ack && apply_target(ActiveTarget{std::string("lkg:") + to_string(objective), lkg->settings, true, std::nullopt})) {
      enter(ControllerState::Run, std::string("validated profile ") + to_string(objective));
    } else {
      enter(ControllerState::MonitorOnly, ack ? "could not apply the validated profile"
                                               : "safety.acknowledge_risk is not set");
      monitor_->start(Settings{}, false);
    }
    return;
  }
  if (!SafetyPolicy::autotune_permitted(d_.config->autotune.enabled, gpu_.policy().config())) {
    enter(ControllerState::MonitorOnly, "autotune needs autotune.enabled = true and safety.acknowledge_risk = true");
    monitor_->start(Settings{}, false);
    return;
  }
  if (!retune && st.tune_failed_at && now - *st.tune_failed_at < kDay) {
    enter(ControllerState::MonitorOnly, "last tuning failed less than 24 h ago (" + st.tune_failure +
                                            "); run `gpuoc tune` to retry");
    monitor_->start(Settings{}, false);
    return;
  }
  {
    std::lock_guard lk(mu_);
    retune_requested_ = false;
  }
  enter(ControllerState::Baseline, *reason);
}

void GpuController::step_tuning() {
  abort_.store(false);
  monitor_->stop();
  {
    std::lock_guard lk(mu_);
    applied_.reset();
  }
  runner_ = d_.make_trial_runner();
  tlog_ = d_.make_tuning_log(gpu_.id());
  TunerOptions o;
  o.tuning = d_.config->tuning;
  o.objective = objective_;
  o.budget = budget_override_.value_or(d_.config->autotune.budget);
  o.bench_estimate = d_.config->benchmark.warmup + d_.config->benchmark.run_duration * d_.config->benchmark.runs;
  tuner_ = std::make_unique<Tuner>(gpu_, *runner_, *tlog_, *d_.clock, o);

  auto fail = [&](const Error& e) {
    {
      std::lock_guard lk(mu_);
      outcome_ = tuner_->outcome();
    }
    if (e.code == ErrorCode::Aborted) {
      (void)gpu_.reset_to_stock();
      if (abort_.load()) {
        // Shutdown or `gpuoc tune abort`: the session stays resumable.
        (void)d_.state->update(gpu_.id().uuid, [&](GpuRuntimeState& s) {
          s.tune_failed_at = d_.clock->wall_now();
          s.tune_failure = "aborted";
        });
        monitor_->start(Settings{}, false);
        enter(ControllerState::MonitorOnly, "tuning aborted");
      }
      return;
    }
    tuner_->finish("error: " + e.message);
    if (e.code == ErrorCode::SafetyRefusal) {
      // A safety limit was exceeded during a trial: SafeRevert like any
      // runtime violation (trial settings were on the GPU).
      on_violation(e.message.find("temperature") != std::string::npos ? "temperature" : "power", e.message, true);
      return;
    }
    tune_failed(e.message);
  };

  if (auto r = tuner_->begin(); !r) return fail(r.error());
  enter(ControllerState::Baseline, "measuring stock");
  if (auto r = tuner_->baseline(&abort_); !r) return fail(r.error());
  enter(ControllerState::Tune, "searching");
  if (auto r = tuner_->tune(&abort_); !r) return fail(r.error());
  enter(ControllerState::Validate, "long stress");
  auto v = tuner_->validate(&abort_);
  if (!v) return fail(v.error());
  const TuneOutcome out = tuner_->outcome();
  {
    std::lock_guard lk(mu_);
    outcome_ = out;
  }
  if (!v.value()) {
    tuner_->finish(out.budget_exhausted ? "budget_exhausted" : "not_validated");
    return tune_failed(out.budget_exhausted ? "tuning budget exhausted before validation" : "validation failed");
  }
  enter(ControllerState::Commit, "validated");
  auto caps = gpu_.capabilities();
  LkgEntry e;
  e.gpu_uuid = gpu_.id().uuid;
  e.gpu_name = gpu_.id().name;
  e.driver_version = caps ? caps.value().driver_version : std::string();
  e.objective = objective_;
  e.settings = out.settings;
  e.validated = out.validated;
  e.validated_at = d_.clock->wall_now();
  e.validation_duration = out.validation_duration;
  e.score = out.final_score;
  e.baseline_score = out.baseline.score;
  e.score_unit = out.unit;
  e.power_w = out.final_power_w;
  e.max_temp_c = out.final_max_temp_c;
  if (auto c = d_.lkg->commit(e); !c) {
    tuner_->finish("commit failed");
    return tune_failed("could not commit the validated profile: " + c.error().message);
  }
  tuner_->finish("validated");
  log::info("[{}] committed validated profile ({}): {} — score {:.2f} -> {:.2f} {}", gpu_.id().uuid,
            to_string(objective_), out.settings.to_string(), out.baseline.score, out.final_score, out.unit);
  if (apply_target(ActiveTarget{std::string("lkg:") + to_string(objective_), out.settings, true, std::nullopt})) {
    enter(ControllerState::Run, "validated profile");
  } else {
    tune_failed("could not apply the validated profile");
  }
}

void GpuController::step_run() {
  const bool adjust = state() == ControllerState::Run;
  if (!monitor_->running()) {
    std::optional<ActiveTarget> a;
    {
      std::lock_guard lk(mu_);
      a = applied_;
    }
    monitor_->start(a ? a->settings : Settings{}, adjust && a.has_value(), a ? a->temp_target_c : std::nullopt);
  }
  const MonitorTick t = monitor_->tick();
  if (t.action == MonitorTick::Action::Violation) {
    on_violation(t.violation_kind, t.reason);
    return;
  }
  // Re-validate stale validated profiles (checked hourly).
  if (adjust && d_.clock->now() - last_revalidate_check_ >= std::chrono::hours(1)) {
    last_revalidate_check_ = d_.clock->now();
    std::optional<ActiveTarget> a;
    {
      std::lock_guard lk(mu_);
      a = applied_;
    }
    if (a && a->validated) {
      const auto lkg = d_.lkg->get(gpu_.id().uuid, objective_);
      if (!lkg || d_.clock->wall_now() - lkg->validated_at > kDay * d_.config->autotune.revalidate_days) {
        log::info("[{}] validated profile is stale; re-tuning", gpu_.id().uuid);
        (void)gpu_.reset_to_stock();
        monitor_->stop();
        {
          std::lock_guard lk(mu_);
          applied_.reset();
        }
        enter(ControllerState::Discover, "revalidation due");
      }
    }
  }
}

void GpuController::step_idle() {
  const GpuRuntimeState st = d_.state->get(gpu_.id().uuid);
  if (st.disabled) {
    enter(ControllerState::Disabled, st.disabled_reason);
    return;
  }
  if (!st.cooldown_until || d_.clock->wall_now() >= *st.cooldown_until) enter(ControllerState::Discover, "cooldown over");
}

void GpuController::step() {
  heartbeat::beat();
  const ControllerState s = state();
  if (is_tuning(s)) {
    // Long-running: commands see the tuning state and answer Busy.
    step_tuning();
    return;
  }
  {
    std::unique_lock op(op_mu_);
    switch (state()) {
      case ControllerState::Discover:
        step_discover();
        return;  // no sleep: act on the decision immediately
      case ControllerState::SafeRevert: {
        const GpuRuntimeState st = d_.state->get(gpu_.id().uuid);
        enter(st.disabled ? ControllerState::Disabled : ControllerState::Idle, st.disabled ? st.disabled_reason : "cooldown");
        return;
      }
      case ControllerState::Run:
      case ControllerState::MonitorOnly:
        step_run();
        break;
      case ControllerState::Idle:
        step_idle();
        break;
      default:
        break;
    }
  }
  d_.clock->sleep_for(poll_interval());
}

// ---- commands ---------------------------------------------------------------------

Result<void> GpuController::set_target(ActiveTarget target) {
  std::unique_lock op(op_mu_, std::defer_lock);
  if (!op.try_lock_for(std::chrono::seconds(10))) return make_error(ErrorCode::Busy, "controller busy");
  const ControllerState s = state();
  if (is_tuning(s)) return make_error(ErrorCode::Busy, "tuning is in progress; run `gpuoc tune abort` first");
  if (s == ControllerState::Disabled)
    return make_error(ErrorCode::SafetyRefusal, "auto-overclock is disabled for this GPU; re-enable it first");
  auto r = gpu_.apply(target.settings, ClampMode::Reject);
  if (!r) return r.error();
  {
    std::lock_guard lk(mu_);
    manual_ = target;
    applied_ = target;
  }
  monitor_->stop();  // the controller thread restarts it with the new ceiling
  enter(ControllerState::Run, target.source);
  return success();
}

void GpuController::clear_target() {
  std::unique_lock op(op_mu_, std::defer_lock);
  (void)op.try_lock_for(std::chrono::seconds(10));
  {
    std::lock_guard lk(mu_);
    manual_.reset();
  }
  if (!is_tuning(state())) {
    monitor_->stop();
    (void)gpu_.reset_to_stock();
    {
      std::lock_guard lk(mu_);
      applied_.reset();
    }
    enter(ControllerState::Discover, "automatic mode");
  }
}

void GpuController::set_budget_override(std::optional<Sec> budget) {
  std::lock_guard lk(mu_);
  budget_override_ = budget;
}

void GpuController::set_objective(Objective o) {
  std::unique_lock op(op_mu_, std::defer_lock);
  (void)op.try_lock_for(std::chrono::seconds(10));
  bool changed = false;
  {
    std::lock_guard lk(mu_);
    changed = objective_ != o;
    objective_ = o;
  }
  const ControllerState s = state();
  if (changed && (s == ControllerState::Run || s == ControllerState::MonitorOnly)) {
    bool manual = false;
    {
      std::lock_guard lk(mu_);
      manual = manual_.has_value();
    }
    if (!manual) {
      monitor_->stop();
      (void)gpu_.reset_to_stock();
      {
        std::lock_guard lk(mu_);
        applied_.reset();
      }
      enter(ControllerState::Discover, std::string("objective ") + to_string(o));
    }
  }
}

void GpuController::request_retune() {
  std::unique_lock op(op_mu_, std::defer_lock);
  (void)op.try_lock_for(std::chrono::seconds(10));
  {
    std::lock_guard lk(mu_);
    retune_requested_ = true;
    manual_.reset();
  }
  (void)d_.state->update(gpu_.id().uuid, [](GpuRuntimeState& s) { s.tune_failed_at.reset(); });
  const ControllerState s = state();
  if (s == ControllerState::Run || s == ControllerState::MonitorOnly || s == ControllerState::Idle) {
    monitor_->stop();
    (void)gpu_.reset_to_stock();
    {
      std::lock_guard lk(mu_);
      applied_.reset();
    }
    enter(ControllerState::Discover, "re-tune requested");
  }
}

Result<void> GpuController::enable() {
  std::unique_lock op(op_mu_, std::defer_lock);
  (void)op.try_lock_for(std::chrono::seconds(10));
  GPUOC_TRY(d_.state->update(gpu_.id().uuid, [](GpuRuntimeState& s) {
    s.disabled = false;
    s.disabled_reason.clear();
    s.cooldown_until.reset();
    s.violations.clear();
  }));
  if (!is_tuning(state())) enter(ControllerState::Discover, "re-enabled");
  return success();
}

void GpuController::abort_tuning() { abort_.store(true); }

Result<void> GpuController::revert_to_stock(const std::string& reason) {
  std::unique_lock op(op_mu_, std::defer_lock);
  if (!op.try_lock_for(std::chrono::seconds(10))) return make_error(ErrorCode::Busy, "controller busy");
  if (is_tuning(state())) {
    abort_.store(true);
    return make_error(ErrorCode::Busy, "tuning aborted; stock will be restored when it stops");
  }
  GPUOC_TRY_ASSIGN(Settings stock, gpu_.stock_settings());
  monitor_->stop();
  GPUOC_TRY(gpu_.reset_to_stock());
  {
    std::lock_guard lk(mu_);
    manual_ = ActiveTarget{"manual:stock", stock, false, std::nullopt};
    applied_.reset();
  }
  enter(ControllerState::MonitorOnly, reason);
  return success();
}

json GpuController::status() const {
  std::lock_guard lk(mu_);
  json j{{"gpu", to_json(gpu_.id())},
         {"state", to_string(state_)},
         {"state_reason", state_reason_},
         {"objective", to_string(objective_)}};
  if (applied_) {
    j["applied"] = json{{"source", applied_->source},
                        {"settings", to_json(applied_->settings)},
                        {"validated", applied_->validated},
                        {"core_offset_now", monitor_->current_core_offset()}};
  } else {
    j["applied"] = nullptr;
  }
  const GpuRuntimeState st = d_.state->get(gpu_.id().uuid);
  const WallTime now = d_.clock->wall_now();
  j["disabled"] = st.disabled;
  j["disabled_reason"] = st.disabled_reason;
  j["violations_24h"] = st.violations_since(now - kDay);
  j["cooldown_until"] = st.cooldown_until ? json(format_iso8601(*st.cooldown_until)) : json(nullptr);
  if (outcome_) {
    j["last_tune"] = json{{"validated", outcome_->validated},
                          {"settings", to_json(outcome_->settings)},
                          {"baseline_score", outcome_->baseline.score},
                          {"final_score", outcome_->final_score},
                          {"unit", outcome_->unit},
                          {"trials", outcome_->trials},
                          {"elapsed_s", outcome_->elapsed_s}};
  }
  return j;
}

}  // namespace gpuoc
