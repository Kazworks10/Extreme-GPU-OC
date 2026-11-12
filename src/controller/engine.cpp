#include "controller/engine.hpp"

#include <algorithm>
#include <format>

#include "backends/dry_run_backend.hpp"
#include "backends/mock/mock_backend.hpp"
#include "backends/nvidia/nvidia_backend.hpp"
#include "bench/process_workload.hpp"
#include "core/heartbeat.hpp"
#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"

namespace gpuoc {

using nlohmann::json;

namespace {

std::shared_ptr<mock::MockBackend> make_mock_from_env(std::shared_ptr<IClock> clock) {
  mock::MockOptions o;
  const std::string seed = platform::getenv_utf8("GPUOC_MOCK_SEED");
  const std::string count = platform::getenv_utf8("GPUOC_MOCK_GPUS");
  try {
    if (!seed.empty()) o.seed = std::stoull(seed);
    if (!count.empty()) o.gpu_count = std::clamp(std::stoi(count), 0, 8);
  } catch (...) {
  }
  o.state_file = platform::getenv_utf8("GPUOC_MOCK_STATE");
  o.clock = std::move(clock);
  return std::make_shared<mock::MockBackend>(o);
}

}  // namespace

Result<std::shared_ptr<IGpuBackend>> make_backend(const std::string& name, bool dry_run) {
  std::shared_ptr<IGpuBackend> b;
  if (name == "mock") {
    b = make_mock_from_env(nullptr);
  } else if (name == "nvidia" || name == "auto") {
    auto nv = make_nvidia_backend();
    if (nv) {
      b = nv.value();
    } else if (name == "auto" && dry_run) {
      log::warn("NVIDIA backend unavailable ({}); dry-run continues with the simulated GPU", nv.error().message);
      b = make_mock_from_env(nullptr);
    } else {
      return nv.error();
    }
  } else {
    return make_error(ErrorCode::InvalidArgument, "unknown backend '" + name + "' (expected auto, nvidia, mock)");
  }
  if (dry_run) b = std::make_shared<DryRunBackend>(b);
  return b;
}

bool is_conservative(const Settings& s, const Capabilities& caps) {
  if (s.core_offset_mhz && *s.core_offset_mhz > 0) return false;
  if (s.mem_offset_mhz && *s.mem_offset_mhz > 0) return false;
  if (s.power_limit_w && caps.stock_power_limit_w > 0 && *s.power_limit_w > caps.stock_power_limit_w) return false;
  if (s.voltage_mv && caps.stock_voltage_mv > 0 && *s.voltage_mv > caps.stock_voltage_mv) return false;
  return true;
}

Engine::Engine(EngineOptions o) : o_(std::move(o)) {}

Engine::~Engine() {
  if (!shut_down_ && !controllers_.empty()) shutdown("engine destroyed");
}

Result<std::unique_ptr<Engine>> Engine::create(EngineOptions opts) {
  std::unique_ptr<Engine> e(new Engine(std::move(opts)));
  GPUOC_TRY(e->init());
  return e;
}

Result<void> Engine::init() {
  clock_ = o_.clock ? o_.clock : std::shared_ptr<IClock>(&SystemClock::instance(), [](IClock*) {});
  policy_ = std::make_shared<SafetyPolicy>(o_.config.safety);
  for (const auto& n : policy_->normalization_notes()) log::warn("config: {}", n);

  if (o_.backend_override) {
    backend_ = o_.backend_override;
    if (o_.dry_run) backend_ = std::make_shared<DryRunBackend>(backend_);
  } else {
    GPUOC_TRY_ASSIGN(backend_, make_backend(o_.backend, o_.dry_run));
  }
  GPUOC_TRY_ASSIGN(auto ids, backend_->enumerate());
  for (auto& id : ids) gpus_.push_back(std::make_unique<Gpu>(backend_, id, policy_));

  lkg_ = std::make_unique<LkgStore>(o_.paths.lkg_file, o_.dry_run);
  if (auto r = lkg_->load(); !r) log::error("LKG store: {} (starting empty)", r.error().message);
  state_ = std::make_unique<StateStore>(o_.paths.state_file, o_.dry_run);
  if (auto r = state_->load(); !r) return make_error(r.error().code, "state.json: " + r.error().message);
  profiles_ = std::make_unique<ProfileStore>(o_.paths.system_profiles_dir, o_.paths.user_profiles_dir);
  (void)profiles_->load();

  // Workloads and the event log: the mock provides its own; real hardware
  // uses the gpuoc-worker process and the Windows Event Log.
  IGpuBackend* inner = backend_.get();
  if (auto* dry = dynamic_cast<DryRunBackend*>(inner)) inner = &dry->inner();
  auto* mock = dynamic_cast<mock::MockBackend*>(inner);
  if (o_.workloads_override) {
    workloads_ = o_.workloads_override;
  } else if (mock) {
    workloads_ = mock->workload_factory();
  } else {
    WorkerOptions w;
    w.worker_path = o_.worker_path;
    w.params = o_.kernel;
    w.hang_timeout = o_.config.tuning.hang_timeout;
    workloads_ = make_worker_factory(w);
  }
  if (o_.events_override) {
    events_ = o_.events_override;
  } else if (mock) {
    events_ = mock->event_log();
  } else {
    events_ = make_system_event_log();
  }
  processes_ = o_.processes_override ? o_.processes_override : make_system_process_lister();
  switcher_ = std::make_unique<AutoSwitcher>(processes_);
  return success();
}

std::vector<Gpu*> Engine::gpus() {
  std::vector<Gpu*> v;
  for (auto& g : gpus_) v.push_back(g.get());
  return v;
}

Result<std::vector<Gpu*>> Engine::select(const std::string& sel) {
  if (gpus_.empty()) return make_error(ErrorCode::NotFound, "no GPUs found");
  if (sel.empty() || sel == "all") return gpus();
  if (sel.find_first_not_of("0123456789") == std::string::npos) {
    const int idx = std::stoi(sel);
    for (auto& g : gpus_) {
      if (g->id().index == idx) return std::vector<Gpu*>{g.get()};
    }
    return make_error(ErrorCode::NotFound, "no GPU with index " + sel);
  }
  std::vector<Gpu*> hits;
  for (auto& g : gpus_) {
    if (g->id().uuid == sel) return std::vector<Gpu*>{g.get()};
    if (g->id().uuid.starts_with(sel)) hits.push_back(g.get());
  }
  if (hits.size() == 1) return hits;
  if (hits.empty()) return make_error(ErrorCode::NotFound, "no GPU matches '" + sel + "'");
  return make_error(ErrorCode::InvalidArgument, "'" + sel + "' matches more than one GPU");
}

GpuController* Engine::controller_for(const Gpu* g) {
  for (auto& c : controllers_) {
    if (&c->gpu() == g) return c.get();
  }
  return nullptr;
}

std::unique_ptr<ITrialRunner> Engine::make_trial_runner() {
  auto stress = std::make_shared<StressRunner>(workloads_, events_, *clock_);
  std::shared_ptr<IBenchmark> bench = make_benchmark();
  return std::make_unique<StressTrialRunner>(stress, bench, o_.config.monitor.poll);
}

std::unique_ptr<TuningLog> Engine::make_tuning_log(const GpuId& id) {
  return std::make_unique<TuningLog>(o_.paths.tuning_log(id.uuid, o_.dry_run), false);
}

std::unique_ptr<IBenchmark> Engine::make_benchmark() { return gpuoc::make_benchmark(o_.config.benchmark, workloads_, *clock_); }

std::string Engine::default_profile() const {
  std::string p = state_->get_global("active_profile");
  return p.empty() ? o_.config.profiles.default_profile : p;
}

json Engine::service_info() const {
  return json{{"version", "1.0.0"},
              {"backend", backend_->name()},
              {"dry_run", o_.dry_run},
              {"autotune_enabled", o_.config.autotune.enabled},
              {"acknowledge_risk", policy_->config().acknowledge_risk},
              {"autotune_permitted", SafetyPolicy::autotune_permitted(o_.config.autotune.enabled, policy_->config())},
              {"objective", to_string(o_.config.autotune.objective)},
              {"default_profile", default_profile()},
              {"auto_switch", o_.config.profiles.auto_switch},
              {"auto_switched_profile", switcher_ && switcher_->active() ? json(*switcher_->active()) : json(nullptr)},
              {"config", o_.config.source}};
}

json Engine::gpu_json(Gpu& g, bool with_telemetry) {
  json j = to_json(g.id());
  if (auto c = g.capabilities()) {
    j["capabilities"] = to_json(c.value());
    j["driver_version"] = c.value().driver_version;
  }
  if (auto l = g.limits()) {
    const auto& lim = l.value();
    j["limits"] = json{{"max_temp_c", lim.max_temp_c},
                       {"target_temp_c", lim.target_temp_c},
                       {"core_offset_mhz", lim.core_offset_allowed ? to_json(lim.core_offset_mhz) : json(nullptr)},
                       {"mem_offset_mhz", lim.mem_offset_allowed ? to_json(lim.mem_offset_mhz) : json(nullptr)},
                       {"power_limit_w", lim.power_allowed ? to_json(lim.power_limit_w) : json(nullptr)},
                       {"voltage_mv", lim.voltage_allowed ? to_json(lim.voltage_mv) : json(nullptr)}};
  }
  if (with_telemetry) {
    if (auto t = g.telemetry()) {
      j["telemetry"] = to_json(t.value());
    } else {
      j["telemetry_error"] = t.error().message;
    }
    if (auto s = g.settings()) j["settings"] = to_json(s.value());
  }
  if (auto e = lkg_->latest(g.id().uuid)) {
    j["lkg"] = json{{"objective", to_string(e->objective)},
                    {"settings", to_json(e->settings)},
                    {"validated_at", format_iso8601(e->validated_at)},
                    {"driver_version", e->driver_version},
                    {"score", e->score},
                    {"baseline_score", e->baseline_score},
                    {"unit", e->score_unit}};
  }
  if (auto* c = controller_for(&g)) j["controller"] = c->status();
  return j;
}

// ---- service mode ---------------------------------------------------------------------

Result<void> Engine::start_controllers() {
  if (gpus_.empty()) log::warn("no GPUs found; the service will idle");
  for (auto& g : gpus_) {
    ControllerDeps d;
    d.config = &o_.config;
    d.clock = clock_.get();
    d.lkg = lkg_.get();
    d.state = state_.get();
    d.events = events_;
    d.make_trial_runner = [this] { return make_trial_runner(); };
    d.make_tuning_log = [this](const GpuId& id) { return make_tuning_log(id); };
    d.dry_run = o_.dry_run;
    auto c = std::make_unique<GpuController>(*g, d);
    if (auto r = c->startup(); !r) log::error("[{}] startup: {}", g->id().uuid, r.error().message);
    controllers_.push_back(std::move(c));
  }
  // Default profile at boot: only validated or conservative settings, and
  // only with safety.acknowledge_risk (unvalidated settings never persist
  // across a reboot).
  const std::string def = default_profile();
  if (!def.empty()) {
    if (auto r = profile_apply(def, "", /*autonomous=*/true); !r)
      log::warn("default profile '{}' not applied: {}", def, r.error().message);
  }
  return success();
}

void Engine::run(const std::atomic<bool>& stop) {
  stop_threads_.store(false);
  for (auto& c : controllers_) {
    GpuController* ctl = c.get();
    threads_.emplace_back([this, ctl, &stop] {
      heartbeat::ThreadScope watched;  // the watchdog notices if this thread hangs
      while (!stop.load() && !stop_threads_.load()) ctl->step();
    });
  }
  heartbeat::ThreadScope watched;
  SteadyTime next_switch = clock_->now();
  while (!stop.load() && !stop_threads_.load()) {
    heartbeat::beat();
    if (o_.config.profiles.auto_switch && clock_->now() >= next_switch) {
      next_switch = clock_->now() + o_.config.profiles.poll;
      poll_auto_switch();
    }
    clock_->sleep_for(Ms(200));
  }
}

void Engine::poll_auto_switch() {
  if (!policy_->autonomous_writes_permitted()) return;
  auto change = switcher_->poll(profiles_->list());
  if (!change) return;
  log::info("auto-switch: {}", change->reason);
  const std::string target = change->profile.value_or(default_profile());
  if (target.empty()) {
    for (auto& c : controllers_) c->clear_target();
    return;
  }
  if (auto r = profile_apply(target, "", /*autonomous=*/true); !r)
    log::warn("auto-switch to '{}' failed: {}", target, r.error().message);
}

void Engine::shutdown(const std::string& reason) {
  if (shut_down_) return;
  shut_down_ = true;
  stop_threads_.store(true);
  for (auto& c : controllers_) c->abort_tuning();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
  for (auto& c : controllers_) c->shutdown(reason);
  log::info("all GPUs restored to stock ({})", reason);
}

Result<json> Engine::profile_apply(const std::string& name, const std::string& selector, bool autonomous) {
  auto prof = profiles_->get(name);
  if (!prof) return make_error(ErrorCode::NotFound, "no profile named '" + name + "'");
  if (autonomous && !policy_->autonomous_writes_permitted())
    return make_error(ErrorCode::SafetyRefusal, "automatic profile changes need safety.acknowledge_risk = true");
  GPUOC_TRY_ASSIGN(auto targets, select(selector));
  json out = json::array();
  for (Gpu* g : targets) {
    GPUOC_TRY_ASSIGN(Capabilities caps, g->capabilities());
    GPUOC_TRY_ASSIGN(ResolvedProfile rp, resolve_profile(*prof, g->id(), caps, *policy_, lkg_.get()));
    GpuController* ctl = controller_for(g);
    json item{{"gpu", g->id().uuid}, {"profile", name}, {"mode", to_string(prof->mode)}};
    if (prof->mode == ProfileMode::Auto) {
      // The controller applies the validated settings (or tunes, if allowed).
      if (ctl) {
        ctl->set_objective(prof->objective);
        ctl->clear_target();
      }
      item["validated"] = rp.validated;
      item["note"] = rp.needs_tuning ? "no validated settings yet; tuning runs if autotune is enabled" : "validated";
    } else {
      if (autonomous && !rp.validated && !is_conservative(rp.settings, caps)) {
        return make_error(ErrorCode::SafetyRefusal,
                          std::format("profile '{}' raises clocks, power or voltage above stock without validation; "
                                      "it can only be applied by an explicit `gpuoc profile apply`",
                                      name));
      }
      if (!ctl) return make_error(ErrorCode::Unavailable, "no controller (service not running)");
      GPUOC_TRY(ctl->set_target(ActiveTarget{"profile:" + name, rp.settings, rp.validated, rp.temp_target_c}));
      item["settings"] = to_json(rp.settings);
    }
    out.push_back(item);
  }
  return out;
}

Result<json> Engine::profile_activate(const std::string& name) {
  if (!profiles_->get(name)) return make_error(ErrorCode::NotFound, "no profile named '" + name + "'");
  GPUOC_TRY(state_->set_global("active_profile", name));
  GPUOC_TRY_ASSIGN(json applied, profile_apply(name, "", false));
  return json{{"default_profile", name}, {"applied", applied}};
}

rpc::Dispatcher Engine::make_dispatcher() {
  rpc::Dispatcher d;
  using P = const json&;
  using C = const platform::CallerInfo&;
  auto gpu_sel = [](P p) { return p.contains("gpu") && p["gpu"].is_string() ? p["gpu"].get<std::string>() : std::string(); };

  d.add("list", false, [this](P, C) -> Result<json> {
    json a = json::array();
    for (Gpu* g : gpus()) {
      json j = to_json(g->id());
      if (auto c = g->capabilities()) j["driver_version"] = c.value().driver_version;
      a.push_back(j);
    }
    return a;
  });
  d.add("status", false, [this](P, C) -> Result<json> {
    json a = json::array();
    for (Gpu* g : gpus()) a.push_back(gpu_json(*g, true));
    return json{{"service", service_info()}, {"gpus", a}};
  });
  d.add("set", true, [this, gpu_sel](P p, C) -> Result<json> {
    if (!p.contains("settings")) return make_error(ErrorCode::InvalidArgument, "params.settings missing");
    GPUOC_TRY_ASSIGN(Settings s, settings_from_json(p["settings"]));
    if (s.empty()) return make_error(ErrorCode::InvalidArgument, "nothing to set");
    GPUOC_TRY_ASSIGN(auto targets, select(gpu_sel(p)));
    json out = json::array();
    for (Gpu* g : targets) {
      GpuController* c = controller_for(g);
      if (!c) return make_error(ErrorCode::Unavailable, "no controller");
      GPUOC_TRY(c->set_target(ActiveTarget{"manual", s, false, std::nullopt}));
      out.push_back(json{{"gpu", g->id().uuid}, {"applied", to_json(s)}});
    }
    return out;
  });
  d.add("reset", true, [this, gpu_sel](P p, C) -> Result<json> {
    GPUOC_TRY_ASSIGN(auto targets, select(gpu_sel(p)));
    json out = json::array();
    for (Gpu* g : targets) {
      GpuController* c = controller_for(g);
      if (!c) return make_error(ErrorCode::Unavailable, "no controller");
      GPUOC_TRY(c->revert_to_stock("gpuoc reset"));
      out.push_back(g->id().uuid);
    }
    return json{{"reset", out}};
  });
  d.add("auto", true, [this, gpu_sel](P p, C) -> Result<json> {
    GPUOC_TRY_ASSIGN(auto targets, select(gpu_sel(p)));
    for (Gpu* g : targets) {
      if (auto* c = controller_for(g)) c->clear_target();
    }
    return json{{"ok", true}};
  });
  d.add("tune.start", true, [this, gpu_sel](P p, C) -> Result<json> {
    if (!policy_->config().acknowledge_risk)
      return make_error(ErrorCode::SafetyRefusal, "tuning needs safety.acknowledge_risk = true");
    GPUOC_TRY_ASSIGN(auto targets, select(gpu_sel(p)));
    std::optional<Objective> obj;
    if (p.contains("objective") && p["objective"].is_string()) {
      GPUOC_TRY_ASSIGN(Objective o, parse_objective(p["objective"].get<std::string>()));
      obj = o;
    }
    std::optional<Sec> budget;
    if (p.contains("budget_s") && p["budget_s"].is_number_integer()) budget = Sec(p["budget_s"].get<long long>());
    json out = json::array();
    for (Gpu* g : targets) {
      GpuController* c = controller_for(g);
      if (!c) return make_error(ErrorCode::Unavailable, "no controller");
      if (obj) c->set_objective(*obj);
      c->set_budget_override(budget);
      c->request_retune();
      out.push_back(g->id().uuid);
    }
    return json{{"tuning_requested", out}};
  });
  d.add("tune.status", false, [this](P, C) -> Result<json> {
    json a = json::array();
    for (Gpu* g : gpus()) {
      json j{{"gpu", g->id().uuid}};
      if (auto* c = controller_for(g)) j["controller"] = c->status();
      TuningLog log(o_.paths.tuning_log(g->id().uuid, o_.dry_run));
      if (auto s = log.status_summary()) j["log"] = s.value();
      a.push_back(j);
    }
    return a;
  });
  d.add("tune.abort", true, [this, gpu_sel](P p, C) -> Result<json> {
    GPUOC_TRY_ASSIGN(auto targets, select(gpu_sel(p)));
    for (Gpu* g : targets) {
      if (auto* c = controller_for(g)) c->abort_tuning();
    }
    return json{{"aborted", true}};
  });
  d.add("profile.list", false, [this](P, C) -> Result<json> {
    json a = json::array();
    for (const auto& pr : profiles_->list())
      a.push_back(json{{"name", pr.name}, {"mode", to_string(pr.mode)}, {"objective", to_string(pr.objective)},
                       {"source", pr.source}, {"builtin", pr.builtin}, {"rules", pr.match.size()}});
    return json{{"profiles", a}, {"default", default_profile()}, {"errors", profiles_->load_errors()}};
  });
  d.add("profile.apply", true, [this, gpu_sel](P p, C) -> Result<json> {
    if (!p.contains("name") || !p["name"].is_string()) return make_error(ErrorCode::InvalidArgument, "params.name missing");
    return profile_apply(p["name"].get<std::string>(), gpu_sel(p), false);
  });
  d.add("profile.activate", true, [this](P p, C) -> Result<json> {
    if (!p.contains("name") || !p["name"].is_string()) return make_error(ErrorCode::InvalidArgument, "params.name missing");
    return profile_activate(p["name"].get<std::string>());
  });
  d.add("profile.reload", true, [this](P, C) -> Result<json> {
    GPUOC_TRY(profiles_->load());
    return json{{"profiles", profiles_->list().size()}, {"errors", profiles_->load_errors()}};
  });
  d.add("gpu.enable", true, [this, gpu_sel](P p, C) -> Result<json> {
    GPUOC_TRY_ASSIGN(auto targets, select(gpu_sel(p)));
    for (Gpu* g : targets) {
      if (auto* c = controller_for(g)) GPUOC_TRY(c->enable());
    }
    return json{{"enabled", true}};
  });
  return d;
}

}  // namespace gpuoc
