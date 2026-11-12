// File locations.
//   System: %ProgramData%\ExtremeGpuOc (config, LKG, state, logs, system profiles)
//   User:   %APPDATA%\ExtremeGpuOc (user profiles)
// Tests override the roots with GPUOC_PROGRAMDATA and GPUOC_APPDATA.
#pragma once

#include <filesystem>
#include <string>

namespace gpuoc::platform {

struct Paths {
  std::filesystem::path program_data_root;  // %ProgramData%
  std::filesystem::path app_data_root;      // %APPDATA%
  std::filesystem::path system_dir;         // %ProgramData%\ExtremeGpuOc
  std::filesystem::path user_dir;           // %APPDATA%\ExtremeGpuOc
  std::filesystem::path config_file;        // gpuoc.toml
  std::filesystem::path lkg_file;           // lkg.json
  std::filesystem::path state_file;         // state.json (violations, disabled GPUs)
  std::filesystem::path logs_dir;           // logs directory
  std::filesystem::path system_profiles_dir;  // profiles.d (system)
  std::filesystem::path user_profiles_dir;    // profiles.d (user)

  [[nodiscard]] std::filesystem::path tuning_log(const std::string& gpu_uuid, bool dry_run = false) const;
};

inline constexpr const char* kAppDirName = "ExtremeGpuOc";
inline constexpr const char* kEnvProgramData = "GPUOC_PROGRAMDATA";
inline constexpr const char* kEnvAppData = "GPUOC_APPDATA";

// Resolution order for each root: GPUOC_* override, then the Windows known
// folder (FOLDERID_ProgramData / FOLDERID_RoamingAppData), then the
// %ProgramData% / %APPDATA% environment variables. Non-Windows development
// hosts fall back to $HOME-relative directories.
Paths resolve_paths();

// Sanitize a GPU UUID for use in a file name.
std::string safe_file_component(const std::string& s);

}  // namespace gpuoc::platform
