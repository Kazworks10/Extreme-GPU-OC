#include "profile/profile_store.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "core/log.hpp"
#include "platform/fs.hpp"

namespace gpuoc {

ProfileStore::ProfileStore(std::filesystem::path system_dir, std::filesystem::path user_dir)
    : system_dir_(std::move(system_dir)), user_dir_(std::move(user_dir)) {}

Result<void> ProfileStore::load() {
  std::map<std::string, Profile> by_name;
  std::vector<std::string> errors;
  for (auto& b : builtin_profiles()) by_name[b.name] = b;

  for (const auto& dir : {system_dir_, user_dir_}) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) continue;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (e.is_regular_file() && e.path().extension() == ".toml") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      const std::string where = platform::path_utf8(f);
      auto text = platform::read_file(f);
      if (!text) {
        errors.push_back(text.error().message);
        continue;
      }
      auto p = parse_profile(text.value(), where);
      if (!p) {
        errors.push_back(p.error().message);
        continue;
      }
      if (p.value().name == "stock") {
        errors.push_back(where + ": the built-in 'stock' profile cannot be overridden");
        continue;
      }
      Profile parsed = std::move(p).value();
      const std::string key = parsed.name;
      by_name[key] = std::move(parsed);
    }
  }
  for (const auto& e : errors) log::warn("profile: {}", e);
  std::lock_guard lk(mu_);
  profiles_.clear();
  for (auto& [n, p] : by_name) {
    (void)n;
    profiles_.push_back(std::move(p));
  }
  std::stable_sort(profiles_.begin(), profiles_.end(), [](const Profile& a, const Profile& b) {
    return a.priority != b.priority ? a.priority < b.priority : a.name < b.name;
  });
  errors_ = std::move(errors);
  return success();
}

std::vector<Profile> ProfileStore::list() const {
  std::lock_guard lk(mu_);
  return profiles_;
}

std::optional<Profile> ProfileStore::get(const std::string& name) const {
  std::lock_guard lk(mu_);
  for (const auto& p : profiles_) {
    if (p.name == name) return p;
  }
  return std::nullopt;
}

std::vector<std::string> ProfileStore::load_errors() const {
  std::lock_guard lk(mu_);
  return errors_;
}

std::filesystem::path ProfileStore::file_for(const std::string& name, bool user_scope) const {
  return (user_scope ? user_dir_ : system_dir_) / platform::u8path(name + ".toml");
}

Result<std::filesystem::path> ProfileStore::save(const Profile& p, bool user_scope) {
  if (!valid_profile_name(p.name)) return make_error(ErrorCode::InvalidArgument, "invalid profile name '" + p.name + "'");
  if (p.name == "stock") return make_error(ErrorCode::InvalidArgument, "the built-in 'stock' profile cannot be replaced");
  const std::string text = serialize_profile(p);
  // Round-trip through the parser so nothing invalid is ever written.
  GPUOC_TRY(parse_profile(text, p.name + ".toml"));
  const auto file = file_for(p.name, user_scope);
  GPUOC_TRY(platform::write_file_atomic(file, text));
  GPUOC_TRY(load());
  return file;
}

Result<void> ProfileStore::remove(const std::string& name) {
  auto p = get(name);
  if (!p) return make_error(ErrorCode::NotFound, "no profile named '" + name + "'");
  if (p->builtin) return make_error(ErrorCode::InvalidArgument, "built-in profile '" + name + "' cannot be deleted");
  std::error_code ec;
  if (!std::filesystem::remove(platform::u8path(p->source), ec) || ec)
    return make_error(ErrorCode::IoError, std::format("cannot delete {}: {}", p->source, ec ? ec.message() : "not found"));
  return load();
}

}  // namespace gpuoc
