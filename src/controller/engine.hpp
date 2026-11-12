// Engine: everything the daemon (and the CLI's local mode) needs, wired
// together: backend, GPUs, safety policy, stores, workloads, controllers,
// profile auto-switching, and the JSON-RPC methods.
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "bench/benchmark.hpp"
#include "bench/kernel_reference.hpp"
#include "controller/controller.hpp"
#include "controller/state_store.hpp"
#include "core/config.hpp"
#include "core/lkg_store.hpp"
#include "platform/paths.hpp"
#include "profile/auto_switch.hpp"
#include "profile/profile_store.hpp"
#include "rpc/jsonrpc.hpp"

namespace gpuoc {

struct EngineOptions {
  Config config;
  platform::Paths paths;
  bool dry_run = false;
  std::string backend = "auto";  // auto | nvidia | mock
  std::shared_ptr<IClock> clock;  // default: SystemClock
  // Test seams.
  std::shared_ptr<IGpuBackend> backend_override;
  WorkloadFactory workloads_override;
  std::shared_ptr<IEventLogSource> events_override;
  std::shared_ptr<IProcessLister> processes_override;
  KernelParams kernel;
  std::string worker_path;
};

// Backend by name; wraps in DryRunBackend when dry_run is set. "auto" uses
// NVIDIA and, only in dry-run mode, falls back to the mock when no NVIDIA
// GPU/driver is present.
Result<std::shared_ptr<IGpuBackend>> make_backend(const std::string& name, bool dry_run);

// Settings that never raise clocks, power, or voltage above stock (safe for
// autonomous application without validation).
bool is_conservative(const Settings& s, const Capabilities& caps);

class Engine {
 public:
  static Result<std::unique_ptr<Engine>> create(EngineOptions opts);
  ~Engine();

  // ---- service mode -------------------------------------------------------------
  // Build controllers; every GPU is reverted to stock first.
  Result<void> start_controllers();
  // Controller threads + auto-switch loop; returns when `stop` is set.
  void run(const std::atomic<bool>& stop);
  // Abort tuning, stop threads, restore stock on every GPU. Synchronous.
  void shutdown(const std::string& reason);
  rpc::Dispatcher make_dispatcher();

  // ---- shared operations ----------------------------------------------------------
  [[nodiscard]] std::vector<Gpu*> gpus();
  Result<std::vector<Gpu*>> select(const std::string& selector);  // "" = all, index, or UUID (prefix)
  nlohmann::json gpu_json(Gpu& g, bool with_telemetry);
  nlohmann::json service_info() const;
  Result<nlohmann::json> profile_apply(const std::string& name, const std::string& selector, bool autonomous);
  Result<nlohmann::json> profile_activate(const std::string& name);
  [[nodiscard]] std::string default_profile() const;
  // One auto-switch poll (run() calls this every profiles.poll_interval_ms).
  void poll_auto_switch();

  // Factories for tuning and workloads (also used by the CLI).
  [[nodiscard]] WorkloadFactory workloads() const { return workloads_; }
  [[nodiscard]] std::shared_ptr<IEventLogSource> events() const { return events_; }
  std::unique_ptr<ITrialRunner> make_trial_runner();
  std::unique_ptr<TuningLog> make_tuning_log(const GpuId& id);
  std::unique_ptr<IBenchmark> make_benchmark();

  [[nodiscard]] const Config& config() const noexcept { return o_.config; }
  [[nodiscard]] const platform::Paths& paths() const noexcept { return o_.paths; }
  [[nodiscard]] IGpuBackend& backend() noexcept { return *backend_; }
  [[nodiscard]] std::shared_ptr<const SafetyPolicy> policy() const noexcept { return policy_; }
  [[nodiscard]] IClock& clock() noexcept { return *clock_; }
  [[nodiscard]] LkgStore& lkg() noexcept { return *lkg_; }
  [[nodiscard]] StateStore& state() noexcept { return *state_; }
  [[nodiscard]] ProfileStore& profiles() noexcept { return *profiles_; }
  [[nodiscard]] bool dry_run() const noexcept { return o_.dry_run; }
  GpuController* controller_for(const Gpu* g);

 private:
  explicit Engine(EngineOptions o);
  Result<void> init();

  EngineOptions o_;
  std::shared_ptr<IClock> clock_;
  std::shared_ptr<IGpuBackend> backend_;
  std::shared_ptr<const SafetyPolicy> policy_;
  std::vector<std::unique_ptr<Gpu>> gpus_;
  std::unique_ptr<LkgStore> lkg_;
  std::unique_ptr<StateStore> state_;
  std::unique_ptr<ProfileStore> profiles_;
  WorkloadFactory workloads_;
  std::shared_ptr<IEventLogSource> events_;
  std::shared_ptr<IProcessLister> processes_;
  std::unique_ptr<AutoSwitcher> switcher_;
  std::vector<std::unique_ptr<GpuController>> controllers_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_threads_{false};
  bool shut_down_ = false;
};

}  // namespace gpuoc
