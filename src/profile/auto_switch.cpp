#include "profile/auto_switch.hpp"

#include <cctype>
#include <filesystem>
#include <format>

#include "core/log.hpp"
#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace gpuoc {

namespace {
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
  // Iterative wildcard match with backtracking on the last '*'.
  std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

bool same_path(std::string_view a, std::string_view b) {
  if (a.size() != b.size() || a.empty()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i] == '/' ? '\\' : lower(a[i]);
    char y = b[i] == '/' ? '\\' : lower(b[i]);
    if (x != y) return false;
  }
  return true;
}

std::optional<SwitchMatch> find_match(const std::vector<Profile>& profiles, const std::vector<ProcessInfo>& procs) {
  for (const auto& prof : profiles) {
    for (const auto& rule : prof.match) {
      for (const auto& proc : procs) {
        const bool hit = !rule.exe.empty() ? glob_match(rule.exe, proc.exe_name)
                                           : (!proc.full_path.empty() && same_path(rule.path, proc.full_path));
        if (hit) return SwitchMatch{prof.name, proc};
      }
    }
  }
  return std::nullopt;
}

std::optional<AutoSwitcher::Change> AutoSwitcher::poll(const std::vector<Profile>& profiles) {
  auto procs = lister_->list();
  if (!procs) {
    log::warn("auto-switch: process list unavailable: {}", procs.error().message);
    return std::nullopt;
  }
  auto m = find_match(profiles, procs.value());
  std::optional<std::string> want = m ? std::optional<std::string>(m->profile) : std::nullopt;
  if (want == active_) return std::nullopt;
  Change c;
  c.profile = want;
  c.reason = m ? std::format("{} (pid {}) matched profile '{}'", m->process.exe_name, m->process.pid, m->profile)
               : std::format("matching process for '{}' exited", active_.value_or("?"));
  active_ = want;
  return c;
}

#ifdef _WIN32

namespace {
class ToolhelpLister final : public IProcessLister {
 public:
  Result<std::vector<ProcessInfo>> list() override {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
      return make_error(ErrorCode::IoError, std::format("CreateToolhelp32Snapshot failed ({})", GetLastError()));
    std::vector<ProcessInfo> out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
      ProcessInfo p;
      p.pid = e.th32ProcessID;
      p.exe_name = platform::narrow(e.szExeFile);
      if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) p.full_path = platform::narrow(std::wstring(buf, n));
        CloseHandle(h);
      }
      out.push_back(std::move(p));
    }
    CloseHandle(snap);
    return out;
  }
};
}  // namespace

std::unique_ptr<IProcessLister> make_system_process_lister() { return std::make_unique<ToolhelpLister>(); }

#else

namespace {
class ProcLister final : public IProcessLister {
 public:
  Result<std::vector<ProcessInfo>> list() override {
    std::vector<ProcessInfo> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
      const std::string name = e.path().filename().string();
      if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
      ProcessInfo p;
      p.pid = std::stoul(name);
      std::error_code ec2;
      const auto exe = std::filesystem::read_symlink(e.path() / "exe", ec2);
      if (!ec2) {
        p.full_path = exe.string();
        p.exe_name = exe.filename().string();
      } else if (auto comm = platform::read_file(e.path() / "comm")) {
        p.exe_name = comm.value();
        while (!p.exe_name.empty() && (p.exe_name.back() == '\n' || p.exe_name.back() == '\r')) p.exe_name.pop_back();
      }
      out.push_back(std::move(p));
    }
    return out;
  }
};
}  // namespace

std::unique_ptr<IProcessLister> make_system_process_lister() { return std::make_unique<ProcLister>(); }

#endif

}  // namespace gpuoc
