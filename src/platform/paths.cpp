#include "platform/paths.hpp"

#include "platform/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#endif

namespace gpuoc::platform {

namespace {

#ifdef _WIN32
std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) out = std::filesystem::path(raw);
  if (raw) CoTaskMemFree(raw);
  return out;
}
#endif

std::filesystem::path resolve_root(const char* override_env, const char* windows_env, bool program_data) {
  if (std::string v = getenv_utf8(override_env); !v.empty()) return u8path(v);
#ifdef _WIN32
  auto kf = known_folder(program_data ? FOLDERID_ProgramData : FOLDERID_RoamingAppData);
  if (!kf.empty()) return kf;
  if (std::string v = getenv_utf8(windows_env); !v.empty()) return u8path(v);
  return program_data ? std::filesystem::path(L"C:\\ProgramData") : std::filesystem::path();
#else
  if (std::string v = getenv_utf8(windows_env); !v.empty()) return u8path(v);
  const std::string home = getenv_utf8("HOME");
  if (program_data) return home.empty() ? std::filesystem::path("/var/lib") : u8path(home) / ".local" / "share";
  return home.empty() ? std::filesystem::path("/tmp") : u8path(home) / ".config";
#endif
}

}  // namespace

std::string safe_file_component(const std::string& s) {
  std::string out;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    out.push_back(ok ? c : '_');
  }
  return out.empty() ? std::string("unknown") : out;
}

std::filesystem::path Paths::tuning_log(const std::string& gpu_uuid, bool dry_run) const {
  return logs_dir / u8path((dry_run ? "dryrun-tune-" : "tune-") + safe_file_component(gpu_uuid) + ".jsonl");
}

Paths resolve_paths() {
  Paths p;
  p.program_data_root = resolve_root(kEnvProgramData, "ProgramData", true);
  p.app_data_root = resolve_root(kEnvAppData, "APPDATA", false);
  p.system_dir = p.program_data_root / kAppDirName;
  p.user_dir = p.app_data_root / kAppDirName;
  p.config_file = p.system_dir / "gpuoc.toml";
  p.lkg_file = p.system_dir / "lkg.json";
  p.state_file = p.system_dir / "state.json";
  p.logs_dir = p.system_dir / "logs";
  p.system_profiles_dir = p.system_dir / "profiles.d";
  p.user_profiles_dir = p.user_dir / "profiles.d";
  return p;
}

}  // namespace gpuoc::platform
