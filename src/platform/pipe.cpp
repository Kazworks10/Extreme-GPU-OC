#include "platform/pipe.hpp"

#include <atomic>
#include <format>
#include <thread>

#include "core/log.hpp"
#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace gpuoc::platform {

namespace {
constexpr std::size_t kMaxMessage = 1 << 20;
}

#ifdef _WIN32

std::string pipe_path(const std::string& name) { return "\\\\.\\pipe\\" + name; }

namespace {

struct Event {
  HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~Event() {
    if (h) CloseHandle(h);
  }
  Event() = default;
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;
};

// Overlapped read of one whole message. Returns false on error/timeout.
bool read_message(HANDLE h, std::string& out, DWORD timeout_ms) {
  char buf[4096];
  for (;;) {
    Event ev;
    OVERLAPPED ov{};
    ov.hEvent = ev.h;
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof buf, &n, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
      if (WaitForSingleObject(ev.h, timeout_ms) != WAIT_OBJECT_0) {
        CancelIo(h);
        return false;
      }
      ok = GetOverlappedResult(h, &ov, &n, FALSE);
      err = ok ? ERROR_SUCCESS : GetLastError();
    } else if (!ok && err == ERROR_MORE_DATA) {
      GetOverlappedResult(h, &ov, &n, FALSE);
    }
    if (ok || err == ERROR_MORE_DATA) {
      out.append(buf, n);
      if (out.size() > kMaxMessage) return false;
      if (ok) return true;
      continue;
    }
    return false;
  }
}

bool write_message(HANDLE h, const std::string& data, DWORD timeout_ms) {
  Event ev;
  OVERLAPPED ov{};
  ov.hEvent = ev.h;
  DWORD n = 0;
  BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(ev.h, timeout_ms) != WAIT_OBJECT_0) {
      CancelIo(h);
      return false;
    }
    ok = GetOverlappedResult(h, &ov, &n, FALSE);
  }
  return ok && n == data.size();
}

CallerInfo identify_caller(HANDLE pipe) {
  CallerInfo c;
  ULONG pid = 0;
  if (GetNamedPipeClientProcessId(pipe, &pid)) c.pid = pid;
  if (!ImpersonateNamedPipeClient(pipe)) return c;
  HANDLE token = nullptr;
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof sid_buf;
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid_buf, &sid_size)) {
      BOOL member = FALSE;
      // With UAC, a non-elevated administrator's token carries the
      // Administrators SID as deny-only, so this is FALSE until elevated.
      if (CheckTokenMembership(token, sid_buf, &member)) c.is_admin = member != FALSE;
    }
    CloseHandle(token);
  }
  RevertToSelf();
  return c;
}

}  // namespace

struct PipeServer::Impl {
  std::wstring path;
  Handler handler;
  HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES sa{};
  std::thread thread;

  ~Impl() {
    SetEvent(stop_event);
    if (thread.joinable()) thread.join();
    CloseHandle(stop_event);
    if (sd) LocalFree(sd);
  }

  HANDLE create_instance(bool first) {
    return CreateNamedPipeW(path.c_str(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa);
  }

  void loop(HANDLE inst) {
    for (;;) {
      Event ev;
      OVERLAPPED ov{};
      ov.hEvent = ev.h;
      bool connected = false;
      const BOOL c = ConnectNamedPipe(inst, &ov);
      const DWORD err = GetLastError();
      if (!c && err == ERROR_IO_PENDING) {
        HANDLE hs[2] = {ev.h, stop_event};
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
          CancelIo(inst);
          CloseHandle(inst);
          return;
        }
        DWORD n = 0;
        connected = GetOverlappedResult(inst, &ov, &n, FALSE) != FALSE;
      } else {
        connected = c || err == ERROR_PIPE_CONNECTED;
      }
      // Create the next instance before releasing this one so the name is
      // never vacant (pipe-squatting protection).
      HANDLE next = create_instance(false);
      if (connected) {
        std::string req;
        if (read_message(inst, req, 5000)) {
          const CallerInfo caller = identify_caller(inst);
          const std::string resp = handler(req, caller);
          if (write_message(inst, resp, 5000)) FlushFileBuffers(inst);
        }
      }
      DisconnectNamedPipe(inst);
      CloseHandle(inst);
      if (next == INVALID_HANDLE_VALUE) {
        log::error("pipe server: CreateNamedPipe failed (error {}); stopping", GetLastError());
        return;
      }
      inst = next;
      if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
        CloseHandle(inst);
        return;
      }
    }
  }
};

Result<std::unique_ptr<PipeServer>> PipeServer::start(const std::string& name, Handler handler) {
  auto impl = std::make_unique<Impl>();
  impl->path = widen(pipe_path(name));
  impl->handler = std::move(handler);
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(widen(kPipeSddl).c_str(), SDDL_REVISION_1, &impl->sd,
                                                            nullptr))
    return make_error(ErrorCode::IoError, std::format("invalid pipe security descriptor (error {})", GetLastError()));
  impl->sa.nLength = sizeof(impl->sa);
  impl->sa.lpSecurityDescriptor = impl->sd;
  impl->sa.bInheritHandle = FALSE;
  HANDLE first = impl->create_instance(true);
  if (first == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    return make_error(e == ERROR_ACCESS_DENIED ? ErrorCode::Busy : ErrorCode::IoError,
                      std::format("cannot create {} (error {}){}", pipe_path(name), e,
                                  e == ERROR_ACCESS_DENIED ? ": another process owns this pipe name" : ""));
  }
  Impl* raw = impl.get();
  impl->thread = std::thread([raw, first] { raw->loop(first); });
  return std::unique_ptr<PipeServer>(new PipeServer(std::move(impl)));
}

Result<std::string> pipe_call(const std::string& name, const std::string& request, Ms timeout) {
  const std::wstring path = widen(pipe_path(name));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  HANDLE h = INVALID_HANDLE_VALUE;
  for (;;) {
    h = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h != INVALID_HANDLE_VALUE) break;
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) {
      const auto left = std::chrono::duration_cast<Ms>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) return make_error(ErrorCode::Timeout, "service pipe busy");
      WaitNamedPipeW(path.c_str(), static_cast<DWORD>(left.count()));
      continue;
    }
    if (err == ERROR_FILE_NOT_FOUND) return make_error(ErrorCode::Unavailable, "service is not running");
    if (err == ERROR_ACCESS_DENIED) return make_error(ErrorCode::PermissionDenied, "access to the service pipe denied");
    return make_error(ErrorCode::IoError, std::format("cannot open {} (error {})", pipe_path(name), err));
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
  const DWORD ms = static_cast<DWORD>(std::max<long long>(1, timeout.count()));
  std::string resp;
  const bool ok = write_message(h, request, ms) && read_message(h, resp, ms);
  CloseHandle(h);
  if (!ok) return make_error(ErrorCode::Timeout, "no response from the service");
  return resp;
}

#else  // Unix domain socket (development hosts)

std::string pipe_path(const std::string& name) {
  std::string dir = getenv_utf8("GPUOC_SOCKET_DIR");
  if (dir.empty()) dir = "/tmp";
  return dir + "/gpuoc-" + name + ".sock";
}

struct PipeServer::Impl {
  int fd = -1;
  int stop_pipe[2] = {-1, -1};
  std::string path;
  Handler handler;
  std::thread thread;

  ~Impl() {
    if (stop_pipe[1] >= 0) {
      const char c = 1;
      (void)!::write(stop_pipe[1], &c, 1);
    }
    if (thread.joinable()) thread.join();
    if (fd >= 0) ::close(fd);
    for (int p : stop_pipe) {
      if (p >= 0) ::close(p);
    }
    ::unlink(path.c_str());
  }

  void loop() {
    for (;;) {
      pollfd pfd[2] = {{fd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
      if (::poll(pfd, 2, -1) < 0) {
        if (errno == EINTR) continue;
        return;
      }
      if (pfd[1].revents) return;
      const int c = ::accept(fd, nullptr, nullptr);
      if (c < 0) continue;
      timeval tv{5, 0};
      ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      std::string req;
      char buf[4096];
      for (;;) {
        const ssize_t n = ::read(c, buf, sizeof buf);
        if (n <= 0) break;
        req.append(buf, static_cast<std::size_t>(n));
        if (req.size() > kMaxMessage) break;
      }
      CallerInfo caller;
#ifdef SO_PEERCRED
      ucred cred{};
      socklen_t len = sizeof cred;
      if (::getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        caller.pid = static_cast<unsigned long>(cred.pid);
        caller.is_admin = cred.uid == 0;
      }
#endif
      const std::string resp = handler(req, caller);
      std::size_t off = 0;
      while (off < resp.size()) {
        const ssize_t n = ::write(c, resp.data() + off, resp.size() - off);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
      }
      ::close(c);
    }
  }
};

Result<std::unique_ptr<PipeServer>> PipeServer::start(const std::string& name, Handler handler) {
  auto impl = std::make_unique<Impl>();
  impl->path = pipe_path(name);
  impl->handler = std::move(handler);
  if (::pipe(impl->stop_pipe) != 0) return make_error(ErrorCode::IoError, "pipe() failed");
  impl->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl->fd < 0) return make_error(ErrorCode::IoError, "socket() failed");
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (impl->path.size() >= sizeof addr.sun_path) return make_error(ErrorCode::InvalidArgument, "socket path too long");
  std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", impl->path.c_str());
  ::unlink(impl->path.c_str());
  if (::bind(impl->fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(impl->fd, 16) != 0)
    return make_error(ErrorCode::IoError, std::format("cannot listen on {}", impl->path));
  ::chmod(impl->path.c_str(), 0666);
  Impl* raw = impl.get();
  impl->thread = std::thread([raw] { raw->loop(); });
  return std::unique_ptr<PipeServer>(new PipeServer(std::move(impl)));
}

Result<std::string> pipe_call(const std::string& name, const std::string& request, Ms timeout) {
  const std::string path = pipe_path(name);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return make_error(ErrorCode::IoError, "socket() failed");
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return make_error(ErrorCode::Unavailable, "service is not running");
  }
  timeval tv{static_cast<time_t>(timeout.count() / 1000), static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::size_t off = 0;
  while (off < request.size()) {
    const ssize_t n = ::write(fd, request.data() + off, request.size() - off);
    if (n <= 0) break;
    off += static_cast<std::size_t>(n);
  }
  ::shutdown(fd, SHUT_WR);
  std::string resp;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) break;
    resp.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (resp.empty()) return make_error(ErrorCode::Timeout, "no response from the service");
  return resp;
}

#endif

PipeServer::PipeServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PipeServer::~PipeServer() = default;
void PipeServer::stop() { impl_.reset(); }

}  // namespace gpuoc::platform
