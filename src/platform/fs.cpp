#include "platform/fs.hpp"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gpuoc::platform {

#ifdef _WIN32
std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}
#endif

std::filesystem::path u8path(const std::string& utf8) {
#ifdef _WIN32
  return std::filesystem::path(widen(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

std::string path_utf8(const std::filesystem::path& p) {
#ifdef _WIN32
  return narrow(p.wstring());
#else
  return p.string();
#endif
}

std::FILE* fopen_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
  std::FILE* f = nullptr;
  if (_wfopen_s(&f, widen(path).c_str(), widen(mode).c_str()) != 0) return nullptr;
  return f;
#else
  return std::fopen(path.c_str(), mode);
#endif
}

Result<std::string> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return make_error(ErrorCode::NotFound, std::format("cannot open {}", path_utf8(p)));
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return make_error(ErrorCode::IoError, std::format("read error on {}", path_utf8(p)));
  return ss.str();
}

Result<void> ensure_dir(const std::filesystem::path& p) {
  if (p.empty()) return success();
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", path_utf8(p), ec.message()));
  return success();
}

Result<void> write_file_atomic(const std::filesystem::path& p, const std::string& contents) {
  GPUOC_TRY(ensure_dir(p.parent_path()));
  std::filesystem::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return make_error(ErrorCode::IoError, std::format("cannot write {}", path_utf8(tmp)));
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out) return make_error(ErrorCode::IoError, std::format("write failed on {}", path_utf8(tmp)));
  }
#ifdef _WIN32
  if (!MoveFileExW(tmp.wstring().c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return make_error(ErrorCode::IoError, std::format("rename to {} failed (error {})", path_utf8(p), GetLastError()));
  }
#else
  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) return make_error(ErrorCode::IoError, std::format("rename to {} failed: {}", path_utf8(p), ec.message()));
#endif
  return success();
}

Result<void> append_line_durable(const std::filesystem::path& p, const std::string& line) {
  GPUOC_TRY(ensure_dir(p.parent_path()));
  std::string data = line;
  if (data.empty() || data.back() != '\n') data.push_back('\n');
#ifdef _WIN32
  HANDLE h = CreateFileW(p.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return make_error(ErrorCode::IoError, std::format("cannot open {} (error {})", path_utf8(p), GetLastError()));
  DWORD written = 0;
  const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  const BOOL flushed = ok ? FlushFileBuffers(h) : FALSE;
  CloseHandle(h);
  if (!ok || written != data.size() || !flushed)
    return make_error(ErrorCode::IoError, std::format("append to {} failed", path_utf8(p)));
#else
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return make_error(ErrorCode::IoError, std::format("cannot open {}", p.string()));
  const ssize_t n = ::write(fd, data.data(), data.size());
  const int s = ::fsync(fd);
  ::close(fd);
  if (n != static_cast<ssize_t>(data.size()) || s != 0)
    return make_error(ErrorCode::IoError, std::format("append to {} failed", p.string()));
#endif
  return success();
}

std::string getenv_utf8(const char* name) {
#ifdef _WIN32
  const std::wstring wname = widen(name);
  const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (n == 0) return {};
  std::wstring buf(n, L'\0');
  const DWORD m = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
  buf.resize(m);
  return narrow(buf);
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

void setenv_utf8(const char* name, const std::string& value) {
#ifdef _WIN32
  SetEnvironmentVariableW(widen(name).c_str(), value.empty() ? nullptr : widen(value).c_str());
#else
  if (value.empty()) {
    ::unsetenv(name);
  } else {
    ::setenv(name, value.c_str(), 1);
  }
#endif
}

}  // namespace gpuoc::platform
