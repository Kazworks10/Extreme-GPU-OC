#include "platform/dynlib.hpp"

#include <format>

#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <climits>
#endif

namespace gpuoc::platform {

Result<std::shared_ptr<DynLib>> DynLib::open(const std::vector<std::string>& candidates) {
  std::string tried;
  for (const auto& c : candidates) {
#ifdef _WIN32
    const std::wstring w = widen(c);
    const bool absolute = w.size() > 2 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'));
    HMODULE h = absolute ? LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
                         : LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) {
      wchar_t buf[MAX_PATH];
      const DWORD n = GetModuleFileNameW(h, buf, MAX_PATH);
      return std::shared_ptr<DynLib>(new DynLib(h, n ? narrow(std::wstring(buf, n)) : c));
    }
    tried += std::format("{}{} (error {})", tried.empty() ? "" : ", ", c, GetLastError());
#else
    void* h = ::dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h) return std::shared_ptr<DynLib>(new DynLib(h, c));
    const char* err = ::dlerror();
    tried += std::format("{}{} ({})", tried.empty() ? "" : ", ", c, err ? err : "not found");
#endif
  }
  return make_error(ErrorCode::Unavailable, "could not load any of: " + tried);
}

DynLib::~DynLib() {
#ifdef _WIN32
  if (handle_) FreeLibrary(static_cast<HMODULE>(handle_));
#else
  if (handle_) ::dlclose(handle_);
#endif
}

void* DynLib::symbol(const char* name) const {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
  return ::dlsym(handle_, name);
#endif
}

std::string DynLib::file_version() const {
#ifdef _WIN32
  const std::wstring w = widen(path_);
  DWORD dummy = 0;
  const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &dummy);
  if (size == 0) return {};
  std::vector<char> data(size);
  if (!GetFileVersionInfoW(w.c_str(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
  return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                     HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
#else
  return {};
#endif
}

std::string current_exe_dir() {
#ifdef _WIN32
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  const auto pos = buf.find_last_of(L"\\/");
  return narrow(pos == std::wstring::npos ? std::wstring(L".") : buf.substr(0, pos));
#else
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return ".";
  std::string s(buf, static_cast<std::size_t>(n));
  const auto pos = s.find_last_of('/');
  return pos == std::string::npos ? std::string(".") : s.substr(0, pos);
#endif
}

}  // namespace gpuoc::platform
