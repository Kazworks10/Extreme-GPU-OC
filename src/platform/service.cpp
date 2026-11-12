#include "platform/service.hpp"

#include <chrono>
#include <format>
#include <thread>

#include "core/log.hpp"
#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace gpuoc::platform {

const char* to_string(ServiceStatus s) noexcept {
  switch (s) {
    case ServiceStatus::StartPending: return "start_pending";
    case ServiceStatus::Running: return "running";
    case ServiceStatus::StopPending: return "stop_pending";
    case ServiceStatus::Stopped: return "stopped";
  }
  return "?";
}

// ---- ServiceLifecycle (portable) --------------------------------------------------

ServiceLifecycle::ServiceLifecycle(IStatusReporter& reporter, Hooks hooks)
    : reporter_(reporter), hooks_(std::move(hooks)) {}

int ServiceLifecycle::run() {
  {
    std::lock_guard lk(mu_);
    current_ = ServiceStatus::StartPending;
    reporter_.report(ServiceStatus::StartPending, 0, Ms(30000));
  }
  if (hooks_.start) {
    if (auto r = hooks_.start(); !r) {
      log::error("service start failed: {}", r.error().message);
      if (hooks_.stop) hooks_.stop("start failed");
      std::lock_guard lk(mu_);
      current_ = ServiceStatus::Stopped;
      reporter_.report(ServiceStatus::Stopped, 1, Ms(0));
      return 1;
    }
  }
  {
    std::lock_guard lk(mu_);
    if (!stop_.load()) {
      current_ = ServiceStatus::Running;
      reporter_.report(ServiceStatus::Running, 0, Ms(0));
    }
  }
  if (hooks_.run && !stop_.load()) hooks_.run(stop_);
  std::string reason;
  {
    std::lock_guard lk(mu_);
    reason = reason_;
    if (current_ != ServiceStatus::StopPending) {
      current_ = ServiceStatus::StopPending;
      reporter_.report(ServiceStatus::StopPending, 0, Ms(30000));
    }
  }
  // Synchronous: stock is restored before Stopped is reported.
  if (hooks_.stop) hooks_.stop(reason);
  std::lock_guard lk(mu_);
  current_ = ServiceStatus::Stopped;
  reporter_.report(ServiceStatus::Stopped, 0, Ms(0));
  return 0;
}

void ServiceLifecycle::control(ServiceControl c) {
  if (c == ServiceControl::Interrogate) return;
  std::lock_guard lk(mu_);
  reason_ = c == ServiceControl::Stop ? "service stop" : c == ServiceControl::Preshutdown ? "system preshutdown" : "system shutdown";
  stop_.store(true);
  if (current_ == ServiceStatus::Running || current_ == ServiceStatus::StartPending) {
    current_ = ServiceStatus::StopPending;
    reporter_.report(ServiceStatus::StopPending, 0, Ms(30000));
  }
}

#ifdef _WIN32

namespace {

ServiceLifecycle::Hooks* g_hooks = nullptr;
ServiceLifecycle* g_life = nullptr;
SERVICE_STATUS_HANDLE g_handle = nullptr;
int g_exit = 0;

class ScmReporter final : public IStatusReporter {
 public:
  void report(ServiceStatus s, int exit_code, Ms wait_hint) override {
    std::lock_guard lk(mu_);
    SERVICE_STATUS st{};
    st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    switch (s) {
      case ServiceStatus::StartPending: st.dwCurrentState = SERVICE_START_PENDING; break;
      case ServiceStatus::Running: st.dwCurrentState = SERVICE_RUNNING; break;
      case ServiceStatus::StopPending: st.dwCurrentState = SERVICE_STOP_PENDING; break;
      case ServiceStatus::Stopped: st.dwCurrentState = SERVICE_STOPPED; break;
    }
    st.dwControlsAccepted = s == ServiceStatus::Running ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
    if (exit_code != 0) {
      st.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
      st.dwServiceSpecificExitCode = static_cast<DWORD>(exit_code);
    }
    st.dwCheckPoint = (s == ServiceStatus::Running || s == ServiceStatus::Stopped) ? 0 : ++checkpoint_;
    st.dwWaitHint = static_cast<DWORD>(wait_hint.count());
    if (g_handle) SetServiceStatus(g_handle, &st);
  }

 private:
  std::mutex mu_;
  DWORD checkpoint_ = 0;
};

DWORD WINAPI scm_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  if (!g_life) return NO_ERROR;
  switch (control) {
    case SERVICE_CONTROL_STOP: g_life->control(ServiceControl::Stop); return NO_ERROR;
    case SERVICE_CONTROL_PRESHUTDOWN: g_life->control(ServiceControl::Preshutdown); return NO_ERROR;
    case SERVICE_CONTROL_SHUTDOWN: g_life->control(ServiceControl::Shutdown); return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE: return NO_ERROR;
    default: return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_handle = RegisterServiceCtrlHandlerExW(kServiceName, scm_handler, nullptr);
  if (!g_handle) return;
  ScmReporter reporter;
  ServiceLifecycle life(reporter, *g_hooks);
  g_life = &life;
  g_exit = life.run();
  g_life = nullptr;
}

struct ScHandle {
  SC_HANDLE h = nullptr;
  explicit ScHandle(SC_HANDLE v) : h(v) {}
  ~ScHandle() {
    if (h) CloseServiceHandle(h);
  }
  ScHandle(const ScHandle&) = delete;
  ScHandle& operator=(const ScHandle&) = delete;
  explicit operator bool() const { return h != nullptr; }
};

Error last_error(const char* what) {
  const DWORD e = GetLastError();
  return make_error(e == ERROR_ACCESS_DENIED ? ErrorCode::PermissionDenied
                    : e == ERROR_SERVICE_DOES_NOT_EXIST ? ErrorCode::NotFound
                                                        : ErrorCode::IoError,
                    std::format("{} failed (error {})", what, e));
}

Result<void> wait_for_state(SC_HANDLE svc, DWORD want, Ms timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  SERVICE_STATUS_PROCESS ssp{};
  DWORD needed = 0;
  while (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof ssp, &needed)) {
    if (ssp.dwCurrentState == want) return success();
    if (std::chrono::steady_clock::now() > deadline) return make_error(ErrorCode::Timeout, "service state change timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  return last_error("QueryServiceStatusEx");
}

}  // namespace

int run_as_service(ServiceLifecycle::Hooks hooks) {
  g_hooks = &hooks;
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), service_main}, {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(table)) {
    log::error("StartServiceCtrlDispatcher failed (error {}); use --console to run outside the SCM", GetLastError());
    return 1;
  }
  return g_exit;
}

Result<void> install_service(const std::string& exe_path, const std::string& args) {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
  if (!scm) return last_error("OpenSCManager");
  const std::wstring cmd = L"\"" + widen(exe_path) + L"\"" + (args.empty() ? L"" : L" " + widen(args));
  ScHandle svc(CreateServiceW(scm.h, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                              SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr,
                              nullptr /* LocalSystem */, nullptr));
  if (!svc) return last_error("CreateService");
  wchar_t desc_text[] = L"Autonomous NVIDIA GPU overclocking with mandatory safety limits.";
  SERVICE_DESCRIPTIONW desc{desc_text};
  ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, &desc);
  SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
  if (!ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed))
    return last_error("ChangeServiceConfig2(delayed auto start)");
  SC_ACTION actions[3] = {{SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}, {SC_ACTION_RESTART, 60000}};
  SERVICE_FAILURE_ACTIONSW fa{};
  fa.dwResetPeriod = 24 * 3600;
  fa.cActions = 3;
  fa.lpsaActions = actions;
  if (!ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa)) return last_error("ChangeServiceConfig2(failure actions)");
  // Also restart after a non-crash failure exit (e.g. the watchdog).
  SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
  ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
  SERVICE_PRESHUTDOWN_INFO pre{60000};
  ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_PRESHUTDOWN_INFO, &pre);
  return success();
}

Result<void> uninstall_service() {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) return last_error("OpenSCManager");
  ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
  if (!svc) return last_error("OpenService");
  SERVICE_STATUS st{};
  if (ControlService(svc.h, SERVICE_CONTROL_STOP, &st)) (void)wait_for_state(svc.h, SERVICE_STOPPED, Ms(60000));
  if (!DeleteService(svc.h)) return last_error("DeleteService");
  return success();
}

Result<void> start_service() {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) return last_error("OpenSCManager");
  ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
  if (!svc) return last_error("OpenService");
  if (!StartServiceW(svc.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return last_error("StartService");
  return wait_for_state(svc.h, SERVICE_RUNNING, Ms(60000));
}

Result<void> stop_service() {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) return last_error("OpenSCManager");
  ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (!svc) return last_error("OpenService");
  SERVICE_STATUS st{};
  if (!ControlService(svc.h, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
    return last_error("ControlService(stop)");
  return wait_for_state(svc.h, SERVICE_STOPPED, Ms(90000));
}

Result<std::string> query_service() {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) return last_error("OpenSCManager");
  ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_STATUS));
  if (!svc) {
    if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return std::string("not installed");
    return last_error("OpenService");
  }
  SERVICE_STATUS_PROCESS ssp{};
  DWORD needed = 0;
  if (!QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof ssp, &needed))
    return last_error("QueryServiceStatusEx");
  switch (ssp.dwCurrentState) {
    case SERVICE_RUNNING: return std::string("running");
    case SERVICE_STOPPED: return std::string("stopped");
    case SERVICE_START_PENDING: return std::string("start pending");
    case SERVICE_STOP_PENDING: return std::string("stop pending");
    default: return std::format("state {}", ssp.dwCurrentState);
  }
}

namespace {
std::function<void(const char*)>* g_console_cb = nullptr;
BOOL WINAPI console_handler(DWORD type) {
  const char* ev = type == CTRL_C_EVENT       ? "Ctrl+C"
                   : type == CTRL_BREAK_EVENT ? "Ctrl+Break"
                   : type == CTRL_CLOSE_EVENT ? "console close"
                   : type == CTRL_LOGOFF_EVENT ? "logoff"
                                               : "shutdown";
  if (g_console_cb) (*g_console_cb)(ev);
  return TRUE;
}
}  // namespace

void install_console_handler(std::function<void(const char* event)> on_event) {
  static std::function<void(const char*)> cb;
  cb = std::move(on_event);
  g_console_cb = &cb;
  SetConsoleCtrlHandler(console_handler, TRUE);
}

bool is_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION e{};
  DWORD size = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size);
  CloseHandle(token);
  return ok && e.TokenIsElevated;
}

#else  // non-Windows development hosts

int run_as_service(ServiceLifecycle::Hooks) {
  log::error("Windows services are not available on this platform; use --console");
  return 1;
}
Result<void> install_service(const std::string&, const std::string&) {
  return make_error(ErrorCode::Unsupported, "Windows services are not available on this platform");
}
Result<void> uninstall_service() { return make_error(ErrorCode::Unsupported, "Windows services are not available on this platform"); }
Result<void> start_service() { return make_error(ErrorCode::Unsupported, "Windows services are not available on this platform"); }
Result<void> stop_service() { return make_error(ErrorCode::Unsupported, "Windows services are not available on this platform"); }
Result<std::string> query_service() { return std::string("not installed"); }

namespace {
std::atomic<int> g_signal{0};
void on_signal(int sig) { g_signal.store(sig); }
}  // namespace

void install_console_handler(std::function<void(const char* event)> on_event) {
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  // Signal handlers may not do real work; a watcher thread runs the callback.
  std::thread([cb = std::move(on_event)] {
    for (;;) {
      const int sig = g_signal.exchange(0);
      if (sig) cb(sig == SIGINT ? "Ctrl+C" : "SIGTERM");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }).detach();
}

bool is_elevated() { return ::geteuid() == 0; }

#endif

}  // namespace gpuoc::platform
