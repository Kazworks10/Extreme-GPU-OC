#include "platform/process.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gpuoc::platform {

std::string quote_windows_arg(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

std::string build_windows_command_line(const std::vector<std::string>& argv) {
  std::string out;
  for (const auto& a : argv) {
    if (!out.empty()) out.push_back(' ');
    out += quote_windows_arg(a);
  }
  return out;
}

// Shared line buffer filled by a reader thread.
namespace {
struct LineBuffer {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  std::string partial;
  bool eof = false;
  static constexpr std::size_t kMaxLine = 1 << 20;

  void feed(const char* data, std::size_t n) {
    std::lock_guard lk(mu);
    for (std::size_t i = 0; i < n; ++i) {
      if (data[i] == '\n') {
        if (!partial.empty() && partial.back() == '\r') partial.pop_back();
        lines.push_back(std::move(partial));
        partial.clear();
      } else if (partial.size() < kMaxLine) {
        partial.push_back(data[i]);
      }
    }
    cv.notify_all();
  }
  void finish() {
    std::lock_guard lk(mu);
    if (!partial.empty()) {
      lines.push_back(std::move(partial));
      partial.clear();
    }
    eof = true;
    cv.notify_all();
  }
  Result<std::optional<std::string>> next(Ms timeout) {
    std::unique_lock lk(mu);
    if (!cv.wait_for(lk, timeout, [&] { return !lines.empty() || eof; }))
      return make_error(ErrorCode::Timeout, "no output within timeout");
    if (lines.empty()) return std::optional<std::string>();
    std::string s = std::move(lines.front());
    lines.pop_front();
    return std::optional<std::string>(std::move(s));
  }
};
}  // namespace

#ifdef _WIN32

struct ChildProcess::Impl {
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE out_read = nullptr;
  std::thread reader;
  LineBuffer buf;
  bool killed = false;

  ~Impl() {
    if (job) TerminateJobObject(job, 1);
    if (process) WaitForSingleObject(process, 5000);
    if (reader.joinable()) reader.join();
    if (out_read) CloseHandle(out_read);
    if (process) CloseHandle(process);
    if (job) CloseHandle(job);
  }
};

namespace {

Result<std::unique_ptr<ChildProcess::Impl>> spawn_impl(std::wstring cmdline) {
  auto impl = std::make_unique<ChildProcess::Impl>();
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_write = nullptr;
  if (!CreatePipe(&impl->out_read, &out_write, &sa, 0))
    return make_error(ErrorCode::IoError, std::format("CreatePipe failed ({})", GetLastError()));
  SetHandleInformation(impl->out_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);

  // Only the two handles we pass may be inherited.
  HANDLE inherit[2] = {out_write, nul};
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  const bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                  nul != INVALID_HANDLE_VALUE ? sizeof(inherit) : sizeof(HANDLE),
                                                  nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = out_write;
  si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
  si.StartupInfo.hStdError = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
  si.lpAttributeList = attrs_ok ? attrs : nullptr;

  impl->job = CreateJobObjectW(nullptr, nullptr);
  if (!impl->job) {
    CloseHandle(out_write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return make_error(ErrorCode::IoError, std::format("CreateJobObject failed ({})", GetLastError()));
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &li, sizeof(li));

  PROCESS_INFORMATION pi{};
  const DWORD flags =
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
  const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                 &si.StartupInfo, &pi);
  const DWORD err = GetLastError();
  if (attrs_ok) DeleteProcThreadAttributeList(attrs);
  CloseHandle(out_write);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    return make_error(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::IoError,
                      std::format("CreateProcessW failed (error {})", err));
  }
  if (!AssignProcessToJobObject(impl->job, pi.hProcess)) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return make_error(ErrorCode::IoError, std::format("AssignProcessToJobObject failed ({})", GetLastError()));
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  impl->process = pi.hProcess;
  ChildProcess::Impl* raw = impl.get();
  impl->reader = std::thread([raw] {
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(raw->out_read, chunk, sizeof chunk, &n, nullptr) && n > 0) raw->buf.feed(chunk, n);
    raw->buf.finish();
  });
  return impl;
}

ExitInfo exit_info(DWORD code, bool killed) {
  ExitInfo e;
  e.code = static_cast<int>(code);
  e.killed = killed;
  e.crashed = !killed && code >= 0xC0000000u;
  e.description = e.crashed ? std::format("crashed with status 0x{:08X}", code) : std::format("exit code {}", code);
  return e;
}

}  // namespace

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const std::vector<std::string>& argv) {
  if (argv.empty()) return make_error(ErrorCode::InvalidArgument, "empty argv");
  GPUOC_TRY_ASSIGN(auto impl, spawn_impl(widen(build_windows_command_line(argv))));
  return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn_command_line(const std::string& command_line) {
  if (command_line.empty()) return make_error(ErrorCode::InvalidArgument, "empty command line");
  GPUOC_TRY_ASSIGN(auto impl, spawn_impl(widen(command_line)));
  return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

std::optional<ExitInfo> ChildProcess::wait(Ms timeout) {
  const DWORD w = WaitForSingleObject(impl_->process, static_cast<DWORD>(timeout.count()));
  if (w != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 0;
  GetExitCodeProcess(impl_->process, &code);
  return exit_info(code, impl_->killed);
}

void ChildProcess::kill() {
  impl_->killed = true;
  TerminateJobObject(impl_->job, 1);
}

bool ChildProcess::running() { return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT; }

#else  // POSIX (development hosts)

struct ChildProcess::Impl {
  pid_t pid = -1;
  int out_read = -1;
  std::thread reader;
  LineBuffer buf;
  bool killed = false;
  std::optional<ExitInfo> exit;

  void reap(int status) {
    ExitInfo e;
    e.killed = killed;
    if (WIFEXITED(status)) {
      e.code = WEXITSTATUS(status);
      e.description = std::format("exit code {}", e.code);
    } else if (WIFSIGNALED(status)) {
      e.code = 128 + WTERMSIG(status);
      e.crashed = !killed;
      e.description = std::format("killed by signal {}", WTERMSIG(status));
    }
    exit = e;
  }

  ~Impl() {
    if (pid > 0 && !exit) {
      ::kill(-pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
    }
    if (reader.joinable()) reader.join();
    if (out_read >= 0) ::close(out_read);
  }
};

namespace {

Result<std::unique_ptr<ChildProcess::Impl>> spawn_impl(const std::vector<std::string>& argv) {
  int fds[2];
  if (::pipe(fds) != 0) return make_error(ErrorCode::IoError, "pipe failed");
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return make_error(ErrorCode::IoError, "fork failed");
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    ::dup2(fds[1], 1);
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, 0);
      ::dup2(devnull, 2);
    }
    ::close(fds[0]);
    ::close(fds[1]);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    ::execv(args[0], args.data());
    ::_exit(127);
  }
  ::setpgid(pid, pid);
  ::close(fds[1]);
  auto impl = std::make_unique<ChildProcess::Impl>();
  impl->pid = pid;
  impl->out_read = fds[0];
  ChildProcess::Impl* raw = impl.get();
  impl->reader = std::thread([raw] {
    char chunk[4096];
    for (;;) {
      const ssize_t n = ::read(raw->out_read, chunk, sizeof chunk);
      if (n > 0) {
        raw->buf.feed(chunk, static_cast<std::size_t>(n));
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    raw->buf.finish();
  });
  return impl;
}

}  // namespace

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const std::vector<std::string>& argv) {
  if (argv.empty()) return make_error(ErrorCode::InvalidArgument, "empty argv");
  if (::access(argv[0].c_str(), X_OK) != 0) return make_error(ErrorCode::NotFound, "not executable: " + argv[0]);
  GPUOC_TRY_ASSIGN(auto impl, spawn_impl(argv));
  return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn_command_line(const std::string& command_line) {
  if (command_line.empty()) return make_error(ErrorCode::InvalidArgument, "empty command line");
  GPUOC_TRY_ASSIGN(auto impl, spawn_impl({"/bin/sh", "-c", command_line}));
  return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

std::optional<ExitInfo> ChildProcess::wait(Ms timeout) {
  if (impl_->exit) return impl_->exit;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    int status = 0;
    const pid_t r = ::waitpid(impl_->pid, &status, WNOHANG);
    if (r == impl_->pid) {
      impl_->reap(status);
      return impl_->exit;
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void ChildProcess::kill() {
  impl_->killed = true;
  if (impl_->pid > 0 && !impl_->exit) ::kill(-impl_->pid, SIGKILL);
}

bool ChildProcess::running() { return !wait(Ms(0)).has_value(); }

#endif

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ChildProcess::~ChildProcess() = default;

Result<std::optional<std::string>> ChildProcess::read_line(Ms timeout) { return impl_->buf.next(timeout); }

Result<CaptureResult> run_capture(const std::string& command_line, Ms timeout) {
  GPUOC_TRY_ASSIGN(auto child, ChildProcess::spawn_command_line(command_line));
  CaptureResult out;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto left = std::chrono::duration_cast<Ms>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      out.timed_out = true;
      child->kill();
      break;
    }
    auto line = child->read_line(left);
    if (!line) {
      out.timed_out = true;
      child->kill();
      break;
    }
    if (!line.value()) break;  // EOF
    out.output += *line.value();
    out.output += '\n';
  }
  auto e = child->wait(Ms(5000));
  if (!e) {
    child->kill();
    e = child->wait(Ms(5000));
  }
  if (e) out.exit = *e;
  return out;
}

}  // namespace gpuoc::platform
