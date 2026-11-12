// Profile locations and precedence:
//   built-in < system (%ProgramData%\ExtremeGpuOc\profiles.d) < user (%APPDATA%\ExtremeGpuOc\profiles.d)
// The built-in "stock" profile can be neither overridden nor deleted.
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "profile/profile.hpp"

namespace gpuoc {

class ProfileStore {
 public:
  ProfileStore(std::filesystem::path system_dir, std::filesystem::path user_dir);

  // Invalid files are skipped and reported in load_errors(); they never
  // prevent the other profiles from loading.
  Result<void> load();
  [[nodiscard]] std::vector<Profile> list() const;  // sorted by priority, then name
  [[nodiscard]] std::optional<Profile> get(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> load_errors() const;

  // Write a profile file (validated first). user_scope: %APPDATA%, else %ProgramData%.
  Result<std::filesystem::path> save(const Profile& p, bool user_scope);
  // Delete the file defining `name`. Built-ins cannot be deleted.
  Result<void> remove(const std::string& name);
  [[nodiscard]] std::filesystem::path file_for(const std::string& name, bool user_scope) const;

 private:
  std::filesystem::path system_dir_;
  std::filesystem::path user_dir_;
  mutable std::mutex mu_;
  std::vector<Profile> profiles_;
  std::vector<std::string> errors_;
};

}  // namespace gpuoc
