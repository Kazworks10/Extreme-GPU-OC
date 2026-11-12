// gpuoc.exe — command-line front end.
//
// When the ExtremeGpuOc service is running, commands go to it over
// \\.\pipe\gpuoc (JSON-RPC 2.0). Otherwise they run locally ("local mode"):
// writes then require an elevated prompt, and anything written is held only
// while gpuoc runs — every exit path restores stock.
#include <atomic>
#include <cstdio>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "backends/nvidia/nvidia_backend.hpp"
#include "bench/process_workload.hpp"
#include "bench/vulkan_kernel.hpp"
#include "cli/exit_codes.hpp"
#include "controller/engine.hpp"
#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "platform/dynlib.hpp"
#include "platform/fs.hpp"
#include "platform/process.hpp"
#include "platform/service.hpp"
#include "stress/stress_runner.hpp"
#include "tuner/tuner.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using nlohmann::json;
namespace gc = gpuoc::cli;

namespace gpuoc {
namespace {

constexpr const char* kUsage = R"(usage: gpuoc [options] <command> [arguments]

commands:
  list                                   list NVIDIA GPUs
  status [--json]                        telemetry, settings, controller state
  doctor                                 check drivers, libraries, privileges
  set --gpu <uuid|index> [--core +N] [--mem +N] [--power W] [--voltage mV]
  reset [--gpu <uuid|index>]
  bench [--gpu ...]
  stress --duration <s> [--gpu ...]
  tune [--objective max_performance|max_perf_per_watt|balanced] [--budget <s>] [--gpu ...]
  tune status | tune abort [--gpu ...]
  profile list | show <name> | create <name> [...] | edit <name> [--set key=value ...]
          | delete <name> | apply <name> [--gpu ...] | activate <name>
  service install | uninstall | start | stop | status

options:
  --json            machine-readable output
  --config <path>   configuration file (default %ProgramData%\ExtremeGpuOc\gpuoc.toml)
  --dry-run         log every write, apply nothing (local mode)
  --backend <name>  auto | nvidia | mock (local mode)
  --local           do not use the service even if it is running
  --for <s>         local set/apply: hold settings for <s> seconds (default: until Ctrl+C)
  -v, --verbose     debug logging

exit codes: 0 ok, 1 usage, 2 unsupported, 3 safety refusal, 4 hardware error, 5 insufficient privileges
)";

const std::set<std::string> kValued = {"config", "backend", "pipe", "gpu", "core", "mem", "power", "voltage",
                                       "duration", "objective", "budget", "for", "from", "set", "mode",
                                       "power-pct", "temp-target", "match-exe", "match-path", "description",
                                       "priority"};
const std::set<std::string> kFlags = {"json", "dry-run", "local", "verbose", "help", "version", "system", "user"};

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::vector<std::string>> opts;
  std::set<std::string> flags;

  [[nodiscard]] bool has(const std::string& f) const { return flags.count(f) != 0; }
  [[nodiscard]] std::optional<std::string> get(const std::string& k) const {
    auto it = opts.find(k);
    if (it == opts.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
  }
  [[nodiscard]] std::vector<std::string> all(const std::string& k) const {
    auto it = opts.find(k);
    return it == opts.end() ? std::vector<std::string>{} : it->second;
  }
};

Result<Args> parse_args(const std::vector<std::string>& argv) {
  Args a;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    std::string s = argv[i];
    if (s == "-h") s = "--help";
    if (s == "-v") s = "--verbose";
    if (s.starts_with("--")) {
      std::string key = s.substr(2);
      std::optional<std::string> inline_val;
      if (auto eq = key.find('='); eq != std::string::npos) {
        inline_val = key.substr(eq + 1);
        key = key.substr(0, eq);
      }
      if (kFlags.count(key)) {
        a.flags.insert(key);
      } else if (kValued.count(key)) {
        if (inline_val) {
          a.opts[key].push_back(*inline_val);
        } else if (i + 1 < argv.size()) {
          a.opts[key].push_back(argv[++i]);
        } else {
          return make_error(ErrorCode::InvalidArgument, "--" + key + " needs a value");
        }
      } else {
        return make_error(ErrorCode::InvalidArgument, "unknown option " + s);
      }
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

Result<int> parse_int(const std::string& what, const std::string& v) {
  try {
    std::size_t used = 0;
    const long long n = std::stoll(v, &used);
    if (used != v.size()) throw std::invalid_argument(v);
    if (n < -1000000 || n > 1000000) throw std::out_of_range(v);
    return static_cast<int>(n);
  } catch (...) {
    return make_error(ErrorCode::InvalidArgument, std::format("{}: '{}' is not an integer", what, v));
  }
}

std::atomic<bool> g_abort{false};

class Cli {
 public:
  explicit Cli(Args a) : a_(std::move(a)) {}

  int run() {
    json_ = a_.has("json");
    if (a_.has("verbose")) log::set_level(log::Level::Debug);
    if (a_.has("version")) {
      std::printf("gpuoc %s\n", "1.0.0");
      return gc::kExitOk;
    }
    if (a_.has("help") || a_.pos.empty()) {
      std::fputs(kUsage, a_.pos.empty() && !a_.has("help") ? stderr : stdout);
      return a_.has("help") ? gc::kExitOk : gc::kExitUsage;
    }
    paths_ = platform::resolve_paths();
    pipe_ = a_.get("pipe").value_or(platform::kDefaultPipeName);
    const std::string cfg_path = a_.get("config").value_or(platform::path_utf8(paths_.config_file));
    auto cfg = load_config(platform::u8path(cfg_path));
    if (!cfg) return fail(cfg.error());
    cfg_ = cfg.value();
    for (const auto& w : cfg_.warnings) log::debug("{}", w);

    const std::string cmd = a_.pos[0];
    const bool force_local = a_.has("local") || a_.has("dry-run") || a_.get("backend").has_value();
    if (!force_local && cmd != "service" && cmd != "doctor") remote_ = service_reachable();

    if (cmd == "list") return cmd_list();
    if (cmd == "status") return cmd_status();
    if (cmd == "doctor") return cmd_doctor();
    if (cmd == "set") return cmd_set();
    if (cmd == "reset") return cmd_reset();
    if (cmd == "bench") return cmd_bench();
    if (cmd == "stress") return cmd_stress();
    if (cmd == "tune") return cmd_tune();
    if (cmd == "profile") return cmd_profile();
    if (cmd == "service") return cmd_service();
    return usage("unknown command '" + cmd + "'");
  }

 private:
  // ---- output -------------------------------------------------------------------------
  int usage(const std::string& msg) {
    if (json_) {
      std::cout << json{{"error", {{"code", "usage"}, {"message", msg}}}}.dump() << "\n";
    } else {
      std::cerr << "gpuoc: " << msg << "\n(run `gpuoc --help` for usage)\n";
    }
    return gc::kExitUsage;
  }
  int fail(const Error& e) {
    if (json_) {
      std::cout << json{{"error", {{"code", to_string(e.code)}, {"message", e.message}}}}.dump() << "\n";
    } else {
      std::cerr << "gpuoc: error: " << e.message << "\n";
    }
    return gc::exit_code_for(e.code);
  }
  int done(const json& j, const std::function<void()>& human) {
    if (json_) {
      std::cout << j.dump(2) << "\n";
    } else {
      human();
    }
    return gc::kExitOk;
  }
  std::string gpu_sel() const { return a_.get("gpu").value_or(""); }

  // ---- transport ----------------------------------------------------------------------
  bool service_reachable() { return rpc::call(pipe_, "list", json::object(), Ms(1500)).ok(); }

  Result<json> remote(const std::string& method, const json& params) {
    auto r = rpc::call(pipe_, method, params, Ms(30000));
    if (!r && r.error().code == ErrorCode::PermissionDenied)
      return make_error(ErrorCode::PermissionDenied, r.error().message + " (run gpuoc from an elevated prompt)");
    return r;
  }

  Result<Engine*> local_engine() {
    if (engine_) return engine_.get();
    EngineOptions o;
    o.config = cfg_;
    o.paths = paths_;
    o.dry_run = a_.has("dry-run");
    o.backend = a_.get("backend").value_or("auto");
    GPUOC_TRY_ASSIGN(engine_, Engine::create(std::move(o)));
    return engine_.get();
  }

  Result<void> require_write_privileges() {
    if (a_.has("dry-run") || a_.get("backend").value_or("") == "mock") return success();
    if (!platform::is_elevated())
      return make_error(ErrorCode::PermissionDenied, "changing GPU settings needs an elevated (Administrator) prompt");
    return success();
  }

  // Keep the process alive while settings are held; StockGuards restore on exit.
  void hold() {
    const auto f = a_.get("for");
    if (f) {
      const int secs = std::max(0, parse_int("--for", *f).value_or(0));
      for (int i = 0; i < secs * 10 && !g_abort.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return;
    }
    if (!json_) std::cout << "holding these settings until Ctrl+C (stock is restored on exit; install the service to keep them)\n";
    while (!g_abort.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // ---- commands -----------------------------------------------------------------------
  int cmd_list() {
    json gpus;
    if (remote_) {
      auto r = remote("list", json::object());
      if (!r) return fail(r.error());
      gpus = r.value();
    } else {
      auto e = local_engine();
      if (!e) return fail(e.error());
      gpus = json::array();
      for (Gpu* g : e.value()->gpus()) {
        json j = to_json(g->id());
        if (auto c = g->capabilities()) j["driver_version"] = c.value().driver_version;
        gpus.push_back(j);
      }
    }
    return done(gpus, [&] {
      if (gpus.empty()) std::cout << "no NVIDIA GPUs found\n";
      for (const auto& g : gpus)
        std::cout << std::format("{:>2}  {}  {}  (driver {})\n", g.value("index", -1), g.value("uuid", ""),
                                 g.value("name", ""), g.value("driver_version", "?"));
    });
  }

  int cmd_status() {
    json st;
    if (remote_) {
      auto r = remote("status", json::object());
      if (!r) return fail(r.error());
      st = r.value();
    } else {
      auto e = local_engine();
      if (!e) return fail(e.error());
      json a = json::array();
      for (Gpu* g : e.value()->gpus()) a.push_back(e.value()->gpu_json(*g, true));
      st = json{{"service", nullptr}, {"local", e.value()->service_info()}, {"gpus", a}};
    }
    return done(st, [&] {
      std::cout << (remote_ ? "service: running\n" : "service: not running (local view)\n");
      for (const auto& g : st["gpus"]) {
        std::cout << std::format("\n[{}] {}  {}\n", g.value("index", -1), g.value("name", ""), g.value("uuid", ""));
        if (g.contains("telemetry")) {
          const auto& t = g["telemetry"];
          std::cout << std::format("  {:.0f} C  {:.0f} W  core {:.0f} MHz  mem {:.0f} MHz  fan {:.0f}%  util {:.0f}%\n",
                                   t.value("temp_c", 0.0), t.value("power_w", 0.0), t.value("core_mhz", 0.0),
                                   t.value("mem_mhz", 0.0), t.value("fan_pct", 0.0), t.value("util_pct", 0.0));
        }
        if (g.contains("settings")) std::cout << "  settings:   " << g["settings"].dump() << "\n";
        if (g.contains("controller")) {
          const auto& c = g["controller"];
          std::cout << "  controller: " << c.value("state", "?") << " — " << c.value("state_reason", "") << "\n";
        }
        if (g.contains("lkg")) std::cout << "  validated:  " << g["lkg"]["settings"].dump() << " ("
                                         << g["lkg"].value("objective", "") << ", " << g["lkg"].value("validated_at", "") << ")\n";
      }
    });
  }

  int cmd_doctor() {
    json checks = json::array();
    int worst = gc::kExitOk;
    auto add = [&](const std::string& name, const std::string& level, const std::string& detail) {
      checks.push_back(json{{"check", name}, {"result", level}, {"detail", detail}});
    };
    add("elevation", platform::is_elevated() ? "ok" : "warn",
        platform::is_elevated() ? "running elevated" : "not elevated: local writes and service install need an elevated prompt");
    const json nv = nvidia_diagnostics();
    const json& nvml = nv.value("nvml", json::object());
    if (nvml.value("loaded", false)) {
      add("nvml.dll", "ok", std::format("{} (version {})", nvml.value("path", "?"), nvml.value("file_version", "?")));
      add("driver", "ok", "NVIDIA driver " + nvml.value("driver_version", std::string("?")));
      add("gpus", nvml.value("device_count", 0) > 0 ? "ok" : "fail",
          std::format("{} NVIDIA GPU(s)", nvml.value("device_count", 0)));
    } else {
      add("nvml.dll", "fail", nvml.value("error", std::string("not found")));
      worst = gc::kExitUnsupported;
    }
    const json& nvapi = nv.value("nvapi", json::object());
    if (nvapi.value("loaded", false)) {
      add("nvapi64.dll", "ok", std::format("{} (version {})", nvapi.value("path", "?"), nvapi.value("file_version", "?")));
      std::string fns;
      for (const auto& [k, v] : nvapi.value("functions", json::object()).items())
        fns += std::format("{}{}={}", fns.empty() ? "" : ", ", k, v.get<bool>() ? "yes" : "no");
      add("nvapi functions", "ok", fns.empty() ? "none" : fns);
    } else {
      add("nvapi64.dll", "warn", nvapi.value("error", std::string("not found")) + " (clock offsets and voltage unavailable)");
    }
    auto ev = make_system_event_log();
    if (ev->available()) {
      auto q = ev->driver_events_since(std::chrono::system_clock::now() - std::chrono::hours(24));
      add("event log", q ? "ok" : "warn",
          q ? std::format("System log readable; {} NVIDIA driver failure event(s) in 24 h", q.value().size()) : q.error().message);
    } else {
      add("event log", "warn", "Windows Event Log unavailable on this platform");
    }
    auto vk = VulkanKernel::list_devices();
    add("vulkan", vk && !vk.value().empty() ? "ok" : "warn",
        vk ? std::format("{} Vulkan device(s)", vk.value().size()) : vk.error().message);
    const std::string worker = default_worker_path();
    add("worker", std::filesystem::exists(platform::u8path(worker)) ? "ok" : "warn", worker);
    add("config", "ok", cfg_.source);
    const bool gates = SafetyPolicy::autotune_permitted(cfg_.autotune.enabled, cfg_.safety);
    add("autotune gates", gates ? "ok" : "warn",
        gates ? "autotune.enabled and safety.acknowledge_risk are set" : "monitor-only until both are set");
    auto svc = platform::query_service();
    add("service", "ok", svc ? svc.value() : svc.error().message);
    const json out{{"checks", checks}};
    if (json_) {
      std::cout << out.dump(2) << "\n";
    } else {
      for (const auto& c : checks)
        std::cout << std::format("[{:<4}] {:<16} {}\n", c["result"].get<std::string>(), c["check"].get<std::string>(),
                                 c["detail"].get<std::string>());
    }
    return worst;
  }

  Result<Settings> settings_from_flags() {
    Settings s;
    if (auto v = a_.get("core")) {
      GPUOC_TRY_ASSIGN(s.core_offset_mhz, parse_int("--core", *v));
    }
    if (auto v = a_.get("mem")) {
      GPUOC_TRY_ASSIGN(s.mem_offset_mhz, parse_int("--mem", *v));
    }
    if (auto v = a_.get("power")) {
      GPUOC_TRY_ASSIGN(s.power_limit_w, parse_int("--power", *v));
    }
    if (auto v = a_.get("voltage")) {
      GPUOC_TRY_ASSIGN(s.voltage_mv, parse_int("--voltage", *v));
    }
    return s;
  }

  int cmd_set() {
    if (!a_.get("gpu")) return usage("set needs --gpu <uuid|index>");
    auto s = settings_from_flags();
    if (!s) return fail(s.error());
    if (s.value().empty()) return usage("set needs at least one of --core, --mem, --power, --voltage");
    if (remote_) {
      auto r = remote("set", json{{"gpu", gpu_sel()}, {"settings", to_json(s.value())}});
      if (!r) return fail(r.error());
      return done(r.value(), [&] { std::cout << "applied by the service: " << s.value().to_string() << "\n"; });
    }
    if (auto p = require_write_privileges(); !p) return fail(p.error());
    auto e = local_engine();
    if (!e) return fail(e.error());
    auto targets = e.value()->select(gpu_sel());
    if (!targets) return fail(targets.error());
    std::vector<std::unique_ptr<StockGuard>> guards;
    json out = json::array();
    for (Gpu* g : targets.value()) {
      guards.push_back(std::make_unique<StockGuard>(*g));
      auto r = g->apply(s.value(), ClampMode::Reject);  // limits enforced; never clamped silently
      if (!r) return fail(r.error());
      out.push_back(json{{"gpu", g->id().uuid}, {"applied", to_json(r.value().applied)}});
      if (!json_) std::cout << std::format("[{}] applied {}\n", g->id().uuid, r.value().applied.to_string());
    }
    if (json_) std::cout << out.dump(2) << "\n";
    hold();
    return gc::kExitOk;
  }

  int cmd_reset() {
    if (remote_) {
      auto r = remote("reset", json{{"gpu", gpu_sel()}});
      if (!r) return fail(r.error());
      return done(r.value(), [] { std::cout << "stock restored\n"; });
    }
    if (auto p = require_write_privileges(); !p) return fail(p.error());
    auto e = local_engine();
    if (!e) return fail(e.error());
    auto targets = e.value()->select(gpu_sel());
    if (!targets) return fail(targets.error());
    json out = json::array();
    for (Gpu* g : targets.value()) {
      if (auto r = g->reset_to_stock(); !r) return fail(r.error());
      out.push_back(g->id().uuid);
    }
    return done(json{{"reset", out}}, [&] { std::cout << "stock restored on " << out.size() << " GPU(s)\n"; });
  }

  Result<void> refuse_if_service_busy() {
    if (!remote_) return success();
    auto st = remote("status", json::object());
    if (!st) return success();
    for (const auto& g : st.value()["gpus"]) {
      const std::string s = g.contains("controller") ? g["controller"].value("state", "") : "";
      if (s == "Baseline" || s == "Tune" || s == "Validate" || s == "Commit")
        return make_error(ErrorCode::Busy, "the service is tuning " + g.value("uuid", std::string("a GPU")) +
                                               "; run `gpuoc tune abort` first");
    }
    return success();
  }

  int cmd_bench() {
    if (auto b = refuse_if_service_busy(); !b) return fail(b.error());
    auto e = local_engine();
    if (!e) return fail(e.error());
    auto targets = e.value()->select(gpu_sel());
    if (!targets) return fail(targets.error());
    json out = json::array();
    for (Gpu* g : targets.value()) {
      auto bench = e.value()->make_benchmark();
      if (!json_) std::cout << std::format("[{}] {} benchmark (warm-up {} s, {} runs)...\n", g->id().uuid, bench->name(),
                                           cfg_.benchmark.warmup.count(), cfg_.benchmark.runs);
      auto r = bench->run(*g, &g_abort);
      if (!r) return fail(r.error());
      const auto& b = r.value();
      out.push_back(json{{"gpu", g->id().uuid}, {"name", b.name}, {"score", b.score}, {"unit", b.unit},
                         {"valid", b.valid}, {"invalid_reason", b.invalid_reason}, {"runs", b.stats.run_scores},
                         {"cv_pct", b.stats.cv_pct}, {"iter_ms_mean", b.stats.iter_ms_mean},
                         {"iter_ms_p99", b.stats.iter_ms_p99}, {"avg_power_w", b.avg_power_w},
                         {"perf_per_watt", b.perf_per_watt}, {"max_temp_c", b.max_temp_c}});
      if (!json_)
        std::cout << std::format("  score {:.3f} {} (median of {}), CV {:.2f}%, {:.0f} W, {:.0f} C{}\n", b.score, b.unit,
                                 b.stats.run_scores.size(), b.stats.cv_pct, b.avg_power_w, b.max_temp_c,
                                 b.valid ? "" : "  INVALID: " + b.invalid_reason);
    }
    if (json_) std::cout << out.dump(2) << "\n";
    return gc::kExitOk;
  }

  int cmd_stress() {
    auto d = a_.get("duration");
    if (!d) return usage("stress needs --duration <seconds>");
    auto secs = parse_int("--duration", *d);
    if (!secs || secs.value() <= 0) return usage("--duration must be a positive number of seconds");
    if (auto b = refuse_if_service_busy(); !b) return fail(b.error());
    auto e = local_engine();
    if (!e) return fail(e.error());
    auto targets = e.value()->select(gpu_sel());
    if (!targets) return fail(targets.error());
    StressRunner runner(e.value()->workloads(), e.value()->events(), e.value()->clock());
    StressOptions o;
    o.duration = Sec(secs.value());
    o.poll = cfg_.monitor.poll;
    json out = json::array();
    int code = gc::kExitOk;
    for (Gpu* g : targets.value()) {
      if (!json_) std::cout << std::format("[{}] stress test for {} s...\n", g->id().uuid, secs.value());
      auto rep = runner.run(*g, o, &g_abort);
      out.push_back(json{{"gpu", g->id().uuid}, {"verdict", to_string(rep.verdict.kind)}, {"reason", rep.verdict.reason},
                         {"iterations", rep.signals.iterations}, {"max_temp_c", rep.signals.max_temp_c},
                         {"max_power_w", rep.signals.max_power_w}, {"details", rep.signals.details}});
      if (!json_) std::cout << "  " << rep.verdict.to_string() << "\n";
      if (!rep.verdict.is_stable()) code = gc::kExitHardware;
    }
    if (json_) std::cout << out.dump(2) << "\n";
    return code;
  }

  int cmd_tune() {
    const std::string sub = a_.pos.size() > 1 ? a_.pos[1] : "";
    if (sub == "status") {
      if (remote_) {
        auto r = remote("tune.status", json::object());
        if (!r) return fail(r.error());
        return done(r.value(), [&] { std::cout << r.value().dump(2) << "\n"; });
      }
      auto e = local_engine();
      if (!e) return fail(e.error());
      json a = json::array();
      for (Gpu* g : e.value()->gpus()) {
        json j{{"gpu", g->id().uuid}};
        TuningLog log(paths_.tuning_log(g->id().uuid, a_.has("dry-run")));
        auto s = log.status_summary();
        j["log"] = s ? s.value() : json(nullptr);
        a.push_back(j);
      }
      return done(a, [&] { std::cout << a.dump(2) << "\n"; });
    }
    if (sub == "abort") {
      if (!remote_) return fail(make_error(ErrorCode::Unavailable, "the service is not running; a local `gpuoc tune` stops with Ctrl+C"));
      auto r = remote("tune.abort", json{{"gpu", gpu_sel()}});
      if (!r) return fail(r.error());
      return done(r.value(), [] { std::cout << "abort requested\n"; });
    }
    if (!sub.empty()) return usage("unknown tune subcommand '" + sub + "'");

    std::optional<Objective> obj;
    if (auto o = a_.get("objective")) {
      auto p = parse_objective(*o);
      if (!p) return fail(p.error());
      obj = p.value();
    }
    std::optional<Sec> budget;
    if (auto b = a_.get("budget")) {
      auto v = parse_int("--budget", *b);
      if (!v || v.value() < 60) return usage("--budget must be at least 60 seconds");
      budget = Sec(v.value());
    }
    if (remote_) {
      json p{{"gpu", gpu_sel()}};
      if (obj) p["objective"] = to_string(*obj);
      if (budget) p["budget_s"] = budget->count();
      auto r = remote("tune.start", p);
      if (!r) return fail(r.error());
      return done(r.value(), [] { std::cout << "tuning started by the service; follow it with `gpuoc tune status`\n"; });
    }
    // Local foreground tuning.
    if (!cfg_.safety.acknowledge_risk)
      return fail(make_error(ErrorCode::SafetyRefusal, "tuning needs safety.acknowledge_risk = true in gpuoc.toml"));
    if (auto p = require_write_privileges(); !p) return fail(p.error());
    auto e = local_engine();
    if (!e) return fail(e.error());
    auto targets = e.value()->select(gpu_sel());
    if (!targets) return fail(targets.error());
    json out = json::array();
    for (Gpu* g : targets.value()) {
      StockGuard guard(*g);
      auto runner = e.value()->make_trial_runner();
      auto tlog = e.value()->make_tuning_log(g->id());
      TunerOptions o;
      o.tuning = cfg_.tuning;
      o.objective = obj.value_or(cfg_.autotune.objective);
      o.budget = budget.value_or(cfg_.autotune.budget);
      o.bench_estimate = cfg_.benchmark.warmup + cfg_.benchmark.run_duration * cfg_.benchmark.runs;
      Tuner tuner(*g, *runner, *tlog, e.value()->clock(), o);
      if (!json_) std::cout << std::format("[{}] tuning ({}, budget {} s); Ctrl+C aborts and restores stock\n",
                                           g->id().uuid, to_string(o.objective), o.budget.count());
      auto r = tuner.run(&g_abort);
      if (!r) return fail(r.error());
      const auto& t = r.value();
      json item{{"gpu", g->id().uuid}, {"validated", t.validated}, {"settings", to_json(t.settings)},
                {"baseline_score", t.baseline.score}, {"final_score", t.final_score}, {"unit", t.unit},
                {"trials", t.trials}, {"elapsed_s", t.elapsed_s}, {"notes", t.notes}};
      if (t.validated) {
        LkgEntry le;
        le.gpu_uuid = g->id().uuid;
        le.gpu_name = g->id().name;
        le.driver_version = g->capabilities() ? g->capabilities().value().driver_version : "";
        le.objective = o.objective;
        le.settings = t.settings;
        le.validated = true;
        le.validated_at = e.value()->clock().wall_now();
        le.validation_duration = t.validation_duration;
        le.score = t.final_score;
        le.baseline_score = t.baseline.score;
        le.score_unit = t.unit;
        le.power_w = t.final_power_w;
        le.max_temp_c = t.final_max_temp_c;
        if (auto c = e.value()->lkg().commit(le); !c) return fail(c.error());
        item["committed"] = !e.value()->dry_run();
      }
      out.push_back(item);
      if (!json_) {
        std::cout << std::format("  {}: {}  score {:.2f} -> {:.2f} {}  ({} trials)\n",
                                 t.validated ? "validated" : "not validated", t.settings.to_string(), t.baseline.score,
                                 t.final_score, t.unit, t.trials);
        for (const auto& n : t.notes) std::cout << "  note: " << n << "\n";
      }
    }
    if (json_) std::cout << out.dump(2) << "\n";
    return gc::kExitOk;
  }

  // ---- profiles -------------------------------------------------------------------------
  Result<Profile> profile_from_flags(Profile p) {
    if (auto v = a_.get("description")) p.description = *v;
    if (auto v = a_.get("objective")) {
      GPUOC_TRY_ASSIGN(p.objective, parse_objective(*v));
    }
    if (auto v = a_.get("priority")) {
      GPUOC_TRY_ASSIGN(p.priority, parse_int("--priority", *v));
    }
    if (auto v = a_.get("mode")) {
      if (*v == "stock") p.mode = ProfileMode::Stock;
      else if (*v == "fixed") p.mode = ProfileMode::Fixed;
      else if (*v == "auto") p.mode = ProfileMode::Auto;
      else return make_error(ErrorCode::InvalidArgument, "--mode must be stock, fixed or auto");
    }
    auto opt = [&](const char* flag, std::optional<int>& field) -> Result<void> {
      if (auto v = a_.get(flag)) {
        GPUOC_TRY_ASSIGN(int n, parse_int(std::string("--") + flag, *v));
        field = n;
        p.mode = ProfileMode::Fixed;
      }
      return success();
    };
    GPUOC_TRY(opt("core", p.target.core_offset_mhz));
    GPUOC_TRY(opt("mem", p.target.mem_offset_mhz));
    GPUOC_TRY(opt("power", p.target.power_limit_w));
    GPUOC_TRY(opt("power-pct", p.target.power_limit_pct));
    GPUOC_TRY(opt("voltage", p.target.voltage_mv));
    if (auto v = a_.get("temp-target")) {
      try {
        p.target.temp_target_c = std::stod(*v);
      } catch (...) {
        return make_error(ErrorCode::InvalidArgument, "--temp-target must be a number");
      }
    }
    for (const auto& m : a_.all("match-exe")) p.match.push_back(MatchRule{m, ""});
    for (const auto& m : a_.all("match-path")) p.match.push_back(MatchRule{"", m});
    // key=value edits use the TOML parser for validation.
    for (const auto& kv : a_.all("set")) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) return make_error(ErrorCode::InvalidArgument, "--set expects key=value");
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (k == "objective") {
        GPUOC_TRY_ASSIGN(p.objective, parse_objective(v));
      } else if (k == "description") {
        p.description = v;
      }
      else if (k == "mode") {
        if (v == "stock") p.mode = ProfileMode::Stock;
        else if (v == "fixed") p.mode = ProfileMode::Fixed;
        else if (v == "auto") p.mode = ProfileMode::Auto;
        else return make_error(ErrorCode::InvalidArgument, "mode must be stock, fixed or auto");
      } else {
        std::optional<int>* field = k == "core_offset_mhz" ? &p.target.core_offset_mhz
                                    : k == "mem_offset_mhz" ? &p.target.mem_offset_mhz
                                    : k == "power_limit_w"  ? &p.target.power_limit_w
                                    : k == "power_limit_pct" ? &p.target.power_limit_pct
                                    : k == "voltage_mv"     ? &p.target.voltage_mv
                                                            : nullptr;
        if (!field) return make_error(ErrorCode::InvalidArgument, "unknown profile key '" + k + "'");
        if (v.empty() || v == "none") {
          field->reset();
        } else {
          GPUOC_TRY_ASSIGN(int n, parse_int(k, v));
          *field = n;
        }
      }
    }
    if (p.mode != ProfileMode::Fixed) {
      p.target.core_offset_mhz.reset();
      p.target.mem_offset_mhz.reset();
      p.target.power_limit_w.reset();
      p.target.power_limit_pct.reset();
      p.target.voltage_mv.reset();
      p.target.fan_curve.reset();
    }
    return p;
  }

  int cmd_profile() {
    const std::string sub = a_.pos.size() > 1 ? a_.pos[1] : "";
    const std::string name = a_.pos.size() > 2 ? a_.pos[2] : "";
    ProfileStore store(paths_.system_profiles_dir, paths_.user_profiles_dir);
    (void)store.load();
    auto reload_service = [&] {
      if (remote_) (void)remote("profile.reload", json::object());
    };

    if (sub == "list") {
      json a = json::array();
      for (const auto& p : store.list())
        a.push_back(json{{"name", p.name}, {"mode", to_string(p.mode)}, {"objective", to_string(p.objective)},
                         {"source", p.source}, {"rules", p.match.size()}, {"priority", p.priority}});
      json out{{"profiles", a}, {"errors", store.load_errors()}};
      return done(out, [&] {
        for (const auto& p : a)
          std::cout << std::format("{:<16} {:<6} {:<18} {}\n", p["name"].get<std::string>(), p["mode"].get<std::string>(),
                                   p["objective"].get<std::string>(), p["source"].get<std::string>());
        for (const auto& e : store.load_errors()) std::cout << "invalid: " << e << "\n";
      });
    }
    if (sub.empty()) return usage("profile needs a subcommand");
    if (name.empty()) return usage("profile " + sub + " needs a profile name");

    if (sub == "show") {
      auto p = store.get(name);
      if (!p) return fail(make_error(ErrorCode::NotFound, "no profile named '" + name + "'"));
      const std::string text = serialize_profile(*p);
      return done(json{{"name", p->name}, {"source", p->source}, {"toml", text}},
                  [&] { std::cout << "# " << p->source << "\n" << text; });
    }
    if (sub == "create") {
      if (store.get(name) && !store.get(name)->builtin)
        return fail(make_error(ErrorCode::InvalidArgument, "profile '" + name + "' already exists; use `profile edit`"));
      Profile p;
      if (auto from = a_.get("from")) {
        auto text = platform::read_file(platform::u8path(*from));
        if (!text) return fail(text.error());
        auto parsed = parse_profile(text.value(), *from);
        if (!parsed) return fail(parsed.error());
        p = parsed.value();
      }
      p.name = name;
      p.mode = a_.get("from") ? p.mode : ProfileMode::Stock;
      auto built = profile_from_flags(p);
      if (!built) return fail(built.error());
      auto file = store.save(built.value(), !a_.has("system"));
      if (!file) return fail(file.error());
      reload_service();
      return done(json{{"created", name}, {"file", platform::path_utf8(file.value())}},
                  [&] { std::cout << "created " << platform::path_utf8(file.value()) << "\n"; });
    }
    if (sub == "edit") {
      auto p = store.get(name);
      if (!p) return fail(make_error(ErrorCode::NotFound, "no profile named '" + name + "'"));
      if (p->name == "stock") return fail(make_error(ErrorCode::InvalidArgument, "the built-in 'stock' profile cannot be edited"));
      const bool user_scope = p->builtin || p->source.find(platform::path_utf8(paths_.user_profiles_dir)) == 0 || !a_.has("system");
      if (!a_.all("set").empty() || a_.get("core") || a_.get("mem") || a_.get("power") || a_.get("voltage") ||
          a_.get("objective") || a_.get("mode") || a_.get("power-pct") || a_.get("temp-target") ||
          a_.get("description") || !a_.all("match-exe").empty() || !a_.all("match-path").empty()) {
        auto built = profile_from_flags(*p);
        if (!built) return fail(built.error());
        auto file = store.save(built.value(), user_scope);
        if (!file) return fail(file.error());
        reload_service();
        return done(json{{"edited", name}, {"file", platform::path_utf8(file.value())}},
                    [&] { std::cout << "saved " << platform::path_utf8(file.value()) << "\n"; });
      }
      return edit_in_editor(store, *p, user_scope, reload_service);
    }
    if (sub == "delete") {
      if (auto r = store.remove(name); !r) return fail(r.error());
      reload_service();
      return done(json{{"deleted", name}}, [&] { std::cout << "deleted profile '" << name << "'\n"; });
    }
    if (sub == "apply") {
      if (remote_) {
        auto r = remote("profile.apply", json{{"name", name}, {"gpu", gpu_sel()}});
        if (!r) return fail(r.error());
        return done(r.value(), [&] { std::cout << "profile '" << name << "' applied by the service\n"; });
      }
      if (auto pr = require_write_privileges(); !pr) return fail(pr.error());
      auto e = local_engine();
      if (!e) return fail(e.error());
      auto p = e.value()->profiles().get(name);
      if (!p) return fail(make_error(ErrorCode::NotFound, "no profile named '" + name + "'"));
      auto targets = e.value()->select(gpu_sel());
      if (!targets) return fail(targets.error());
      std::vector<std::unique_ptr<StockGuard>> guards;
      for (Gpu* g : targets.value()) {
        auto caps = g->capabilities();
        if (!caps) return fail(caps.error());
        auto rp = resolve_profile(*p, g->id(), caps.value(), *e.value()->policy(), &e.value()->lkg());
        if (!rp) return fail(rp.error());
        if (rp.value().needs_tuning)
          return fail(make_error(ErrorCode::Unavailable, "profile '" + name + "' has no validated settings for " +
                                                             g->id().uuid + " yet; run `gpuoc tune` first"));
        guards.push_back(std::make_unique<StockGuard>(*g));
        auto r = g->apply(rp.value().settings, ClampMode::Reject);
        if (!r) return fail(r.error());
        if (!json_) std::cout << std::format("[{}] profile '{}': {}\n", g->id().uuid, name, r.value().applied.to_string());
      }
      hold();
      return gc::kExitOk;
    }
    if (sub == "activate") {
      if (remote_) {
        auto r = remote("profile.activate", json{{"name", name}});
        if (!r) return fail(r.error());
        return done(r.value(), [&] { std::cout << "default profile is now '" << name << "'\n"; });
      }
      if (!store.get(name)) return fail(make_error(ErrorCode::NotFound, "no profile named '" + name + "'"));
      if (auto pr = require_write_privileges(); !pr) return fail(pr.error());
      StateStore st(paths_.state_file, a_.has("dry-run"));
      if (auto r = st.load(); !r) return fail(r.error());
      if (auto r = st.set_global("active_profile", name); !r) return fail(r.error());
      return done(json{{"default_profile", name}, {"applied", false}}, [&] {
        std::cout << "default profile is now '" << name << "' (applied when the service starts)\n";
      });
    }
    return usage("unknown profile subcommand '" + sub + "'");
  }

  int edit_in_editor(ProfileStore& store, const Profile& p, bool user_scope, const std::function<void()>& reload) {
    const auto target = store.file_for(p.name, user_scope);
    const auto tmp = std::filesystem::temp_directory_path() / platform::u8path("gpuoc-edit-" + p.name + ".toml");
    if (auto w = platform::write_file_atomic(tmp, serialize_profile(p)); !w) return fail(w.error());
    std::string editor = platform::getenv_utf8("EDITOR");
#ifdef _WIN32
    if (editor.empty()) editor = "notepad.exe";
    const std::string cmdline = editor + " " + platform::quote_windows_arg(platform::path_utf8(tmp));
#else
    if (editor.empty()) editor = "vi";
    const std::string cmdline = editor + " '" + platform::path_utf8(tmp) + "'";
#endif
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = platform::widen(cmdline);
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
      return fail(make_error(ErrorCode::IoError, "cannot start editor: " + editor));
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    if (std::system(cmdline.c_str()) != 0) return fail(make_error(ErrorCode::IoError, "editor failed: " + editor));
#endif
    auto text = platform::read_file(tmp);
    if (!text) return fail(text.error());
    auto parsed = parse_profile(text.value(), platform::path_utf8(tmp));
    if (!parsed) return fail(parsed.error());  // the original file is untouched
    if (parsed.value().name != p.name)
      return fail(make_error(ErrorCode::InvalidArgument, "renaming a profile in the editor is not supported"));
    if (auto w = platform::write_file_atomic(target, text.value()); !w) return fail(w.error());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    reload();
    return done(json{{"edited", p.name}, {"file", platform::path_utf8(target)}},
                [&] { std::cout << "saved " << platform::path_utf8(target) << "\n"; });
  }

  int cmd_service() {
    const std::string sub = a_.pos.size() > 1 ? a_.pos[1] : "";
    Result<void> r = success();
    if (sub == "install") {
#ifdef _WIN32
      const std::string exe = platform::current_exe_dir() + "\\gpuocd.exe";
#else
      const std::string exe = platform::current_exe_dir() + "/gpuocd";
#endif
      std::string args;
      if (auto c = a_.get("config")) args = "--config " + platform::quote_windows_arg(*c);
      r = platform::install_service(exe, args);
    } else if (sub == "uninstall") {
      r = platform::uninstall_service();
    } else if (sub == "start") {
      r = platform::start_service();
    } else if (sub == "stop") {
      r = platform::stop_service();
    } else if (sub == "status") {
      auto s = platform::query_service();
      if (!s) return fail(s.error());
      return done(json{{"service", s.value()}}, [&] { std::cout << "ExtremeGpuOc: " << s.value() << "\n"; });
    } else {
      return usage("service needs install, uninstall, start, stop or status");
    }
    if (!r) return fail(r.error());
    return done(json{{"service", sub}, {"ok", true}}, [&] { std::cout << "service " << sub << ": ok\n"; });
  }

  Args a_;
  bool json_ = false;
  bool remote_ = false;
  std::string pipe_;
  Config cfg_;
  platform::Paths paths_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace
}  // namespace gpuoc

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(gpuoc::platform::narrow(argv[i]));
#else
int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
#endif
  gpuoc::log::set_level(gpuoc::log::Level::Warn);
  // Ctrl+C / Ctrl+Break / console close: restore stock synchronously.
  gpuoc::platform::install_console_handler([](const char* event) {
    gpuoc::g_abort.store(true);
    gpuoc::StockRegistry::instance().restore_all(event);
  });
  auto parsed = gpuoc::parse_args(args);
  if (!parsed) {
    std::cerr << "gpuoc: " << parsed.error().message << "\n(run `gpuoc --help` for usage)\n";
    return gc::kExitUsage;
  }
  gpuoc::Cli cli(std::move(parsed).value());
  return cli.run();
}
