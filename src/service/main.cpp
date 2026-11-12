// gpuocd.exe — the ExtremeGpuOc Windows service.
//
//   gpuocd                         run under the Service Control Manager
//   gpuocd --console               same code without the SCM (debugging)
//   options: --config <path> --dry-run --backend <auto|nvidia|mock> --pipe <name> -v
//
// Start: every GPU is reverted to stock before anything else. Stop and
// preshutdown: tuning is aborted and stock is restored synchronously before
// SERVICE_STOPPED is reported. A watchdog reverts and exits if the service
// stops making progress; the SCM failure actions restart it.
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cli/exit_codes.hpp"
#include "controller/engine.hpp"
#include "controller/watchdog.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"
#include "platform/pipe.hpp"
#include "platform/service.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace gpuoc {
namespace {

struct Options {
  bool console = false;
  bool dry_run = false;
  bool verbose = false;
  std::string config;
  std::string backend = "auto";
  std::string pipe = platform::kDefaultPipeName;
};

class ConsoleReporter final : public platform::IStatusReporter {
 public:
  void report(platform::ServiceStatus s, int code, Ms) override {
    log::info("service status: {}{}", platform::to_string(s), code ? " (exit code " + std::to_string(code) + ")" : "");
  }
};

int run(const Options& opt) {
  const platform::Paths paths = platform::resolve_paths();
  (void)platform::ensure_dir(paths.logs_dir);
  log::open_file(platform::path_utf8(paths.logs_dir / "gpuocd.log"));
  log::set_stderr(opt.console);
  log::set_level(opt.verbose ? log::Level::Debug : log::Level::Info);
  log::info("gpuocd 1.0.0 starting ({}{})", opt.console ? "console" : "service", opt.dry_run ? ", dry-run" : "");

  auto cfg = load_config(opt.config.empty() ? paths.config_file : platform::u8path(opt.config));
  if (!cfg) {
    log::error("configuration: {}", cfg.error().message);
    return cli::exit_code_for(cfg.error().code);
  }
  for (const auto& w : cfg.value().warnings) log::warn("{}", w);

  EngineOptions eo;
  eo.config = cfg.value();
  eo.paths = paths;
  eo.dry_run = opt.dry_run;
  eo.backend = opt.backend;
  auto engine = Engine::create(std::move(eo));
  if (!engine) {
    log::error("startup: {}", engine.error().message);
    return cli::exit_code_for(engine.error().code);
  }
  Engine& e = *engine.value();
  rpc::Dispatcher dispatcher = e.make_dispatcher();
  std::unique_ptr<platform::PipeServer> pipe;
  Watchdog watchdog(std::chrono::duration_cast<std::chrono::milliseconds>(cfg.value().service.watchdog_timeout));

  platform::ServiceLifecycle::Hooks hooks;
  hooks.start = [&]() -> Result<void> {
    GPUOC_TRY(e.start_controllers());  // reverts every GPU to stock first
    GPUOC_TRY_ASSIGN(pipe, platform::PipeServer::start(opt.pipe, [&](const std::string& req, const platform::CallerInfo& c) {
                       return dispatcher.handle(req, c);
                     }));
    watchdog.start();
    log::info("control pipe {} ready", platform::pipe_path(opt.pipe));
    return success();
  };
  hooks.run = [&](const std::atomic<bool>& stop) { e.run(stop); };
  hooks.stop = [&](const std::string& reason) {
    watchdog.stop();
    if (pipe) pipe->stop();
    e.shutdown(reason);  // synchronous stock restore
  };

  if (!opt.console) return platform::run_as_service(std::move(hooks));

  ConsoleReporter reporter;
  platform::ServiceLifecycle life(reporter, hooks);
  platform::install_console_handler([&](const char* event) {
    log::warn("{}: stopping", event);
    // Close/logoff/shutdown may terminate the process right after the handler
    // returns, so restore synchronously here as well (idempotent).
    StockRegistry::instance().restore_all(event);
    life.control(platform::ServiceControl::Stop);
  });
  return life.run();
}

}  // namespace
}  // namespace gpuoc

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(gpuoc::platform::narrow(argv[i]));
#else
int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
#endif
  gpuoc::Options opt;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
    if (a == "--console") opt.console = true;
    else if (a == "--dry-run") opt.dry_run = true;
    else if (a == "-v" || a == "--verbose") opt.verbose = true;
    else if (a == "--config") opt.config = value();
    else if (a == "--backend") opt.backend = value();
    else if (a == "--pipe") opt.pipe = value();
    else {
      std::fprintf(stderr,
                   "usage: gpuocd [--console] [--config <path>] [--dry-run] [--backend auto|nvidia|mock] [--pipe <name>] [-v]\n");
      return gpuoc::cli::kExitUsage;
    }
  }
  return gpuoc::run(opt);
}
