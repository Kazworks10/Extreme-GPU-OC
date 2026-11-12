// Process-based profile auto-switching. The process list is polled (every
// 2 s by default); executable paths come from QueryFullProcessImageNameW.
// Rules are checked in profile order (priority, then name) and rule order;
// the first match wins. When no rule matches, the default profile returns.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gpuoc/result.hpp"
#include "profile/profile.hpp"

namespace gpuoc {

struct ProcessInfo {
  unsigned long pid = 0;
  std::string exe_name;   // "game.exe"
  std::string full_path;  // "C:\Games\game.exe" (empty if access was denied)
};

class IProcessLister {
 public:
  virtual ~IProcessLister() = default;
  virtual Result<std::vector<ProcessInfo>> list() = 0;
};

// CreateToolhelp32Snapshot + QueryFullProcessImageNameW (Windows), /proc
// (other platforms).
std::unique_ptr<IProcessLister> make_system_process_lister();

// Case-insensitive glob with '*' and '?'.
bool glob_match(std::string_view pattern, std::string_view text);
// Case-insensitive path equality with '/' and '\' treated alike.
bool same_path(std::string_view a, std::string_view b);

struct SwitchMatch {
  std::string profile;
  ProcessInfo process;
};

// First match over `profiles` (already in priority order).
std::optional<SwitchMatch> find_match(const std::vector<Profile>& profiles, const std::vector<ProcessInfo>& procs);

class AutoSwitcher {
 public:
  explicit AutoSwitcher(std::shared_ptr<IProcessLister> lister) : lister_(std::move(lister)) {}
  struct Change {
    std::optional<std::string> profile;  // nullopt: back to the default profile
    std::string reason;
  };
  // Poll once. Returns a change when the matching profile differs from the
  // last poll.
  std::optional<Change> poll(const std::vector<Profile>& profiles);
  [[nodiscard]] const std::optional<std::string>& active() const noexcept { return active_; }

 private:
  std::shared_ptr<IProcessLister> lister_;
  std::optional<std::string> active_;
};

}  // namespace gpuoc
