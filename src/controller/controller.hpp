// Autonomous per-GPU state machine:
//   Discover -> Baseline -> Tune -> Validate -> Commit -> Run
// Any state enters SafeRevert on a safety violation; SafeRevert moves to Idle
// and retries after a cooldown. Without autotune.enabled AND
// safety.acknowledge_risk the controller stays in MonitorOnly.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "controller/runtime_monitor.hpp"
#include "controller/state_store.hpp"
#include "core/config.hpp"
#include "core/lkg_store.hpp"
#include "gpuoc/gpu.hpp"
#include "tuner/tuner.hpp"

namespace gpuoc {

enum class ControllerState { Discover, Baseline, Tune, Validate, Commit, Run, SafeRevert, Idle, MonitorOnly, Disabled };
const char* to_string(ControllerState s) noexcept;

// What the controller keeps applied in Run.
struct ActiveTarget {
  std::string source;  // "lkg:<objective>", "profile:<name>", "manual"
  Settings settings;
  bool validated = false;
  std::optional<double> temp_target_c;  // profile override (can only tighten)
};

struct ControllerDeps {
  const Config* config = nullptr;
  IClock* clock = nullptr;
  LkgStore* lkg = nullptr;
  StateStore* state = nullptr;
  std::shared_ptr<IEventLogSource> events;
  std::function<std::unique_ptr<ITrialRunner>()> make_trial_runner;
  std::function<std::unique_ptr<TuningLog>(const GpuId&)> make_tuning_log;
  bool dry_run = false;
};

class GpuController {
 public:
  GpuController(Gpu& gpu, ControllerDeps deps);
  ~GpuController();

  // Restart path: revert to stock before anything else happens.
  Result<void> startup();
  // Advance the state machine by one step (a monitor tick in Run/MonitorOnly).
  // Long states (Tune, Validate) block until done or aborted.
  void step();
  // Restore stock synchronously (service stop / preshutdown / Ctrl+C).
  void shutdown(const std::string& reason);

  // Commands (thread-safe; take effect at the next step).
  Result<void> set_target(ActiveTarget target);  // manual / profile: applied now, then monitored
  void clear_target();                           // back to automatic (LKG for the objective)
  void set_objective(Objective o);
  void set_budget_override(std::optional<Sec> budget);
  void request_retune();
  Result<void> enable();                         // manual re-enable after disablement
  void abort_tuning();
  Result<void> revert_to_stock(const std::string& reason);

  [[nodiscard]] ControllerState state() const;
  [[nodiscard]] nlohmann::json status() const;
  [[nodiscard]] Gpu& gpu() noexcept { return gpu_; }
  [[nodiscard]] std::optional<TuneOutcome> last_outcome() const;
  [[nodiscard]] Ms poll_interval() const;

 private:
  void enter(ControllerState s, const std::string& why);
  // settings_applied: tuning trials count as applied settings.
  void on_violation(const std::string& kind, const std::string& reason, bool settings_applied = false);
  void step_discover();
  void step_tuning();  // Baseline -> Tune -> Validate -> Commit
  void step_run();
  void step_idle();
  void tune_failed(const std::string& why);
  bool apply_target(const ActiveTarget& t);

  Gpu& gpu_;
  ControllerDeps d_;
  std::unique_ptr<StockGuard> guard_;
  std::unique_ptr<RuntimeMonitor> monitor_;
  mutable std::mutex mu_;
  ControllerState state_ = ControllerState::Discover;
  std::string state_reason_;
  Objective objective_;
  std::optional<ActiveTarget> manual_;   // profile/manual override
  std::optional<ActiveTarget> applied_;  // what is on the GPU now
  bool retune_requested_ = false;
  std::atomic<bool> abort_{false};
  std::optional<TuneOutcome> outcome_;
  std::unique_ptr<ITrialRunner> runner_;
  std::unique_ptr<TuningLog> tlog_;
  std::unique_ptr<Tuner> tuner_;
  SteadyTime last_revalidate_check_{};
  std::optional<Sec> budget_override_;
  // Serializes state-machine work (outside long tuning) with commands from
  // the RPC thread.
  std::timed_mutex op_mu_;
};

}  // namespace gpuoc
