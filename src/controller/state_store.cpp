#include "controller/state_store.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"

namespace gpuoc {

using nlohmann::json;

int GpuRuntimeState::violations_since(WallTime since) const {
  return static_cast<int>(
      std::count_if(violations.begin(), violations.end(), [&](const ViolationRecord& v) { return v.time >= since; }));
}

int GpuRuntimeState::instability_since(WallTime since) const {
  return static_cast<int>(std::count_if(violations.begin(), violations.end(), [&](const ViolationRecord& v) {
    return v.time >= since && v.is_instability();
  }));
}

StateStore::StateStore(std::filesystem::path file, bool read_only) : file_(std::move(file)), read_only_(read_only) {}

Result<void> StateStore::load() {
  std::lock_guard lk(mu_);
  gpus_.clear();
  globals_.clear();
  auto text = platform::read_file(file_);
  if (!text) return text.error().code == ErrorCode::NotFound ? success() : Result<void>(text.error());
  json j = json::parse(text.value(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("gpus") || !j["gpus"].is_object()) {
    // A corrupt state file must not unlock a disabled GPU silently: keep
    // nothing loaded, but report it.
    return make_error(ErrorCode::ParseError, "state.json is corrupt");
  }
  if (j.contains("globals") && j["globals"].is_object()) {
    for (auto it = j["globals"].begin(); it != j["globals"].end(); ++it) {
      if (it.value().is_string()) globals_[it.key()] = it.value().get<std::string>();
    }
  }
  for (auto it = j["gpus"].begin(); it != j["gpus"].end(); ++it) {
    const json& g = it.value();
    GpuRuntimeState s;
    s.disabled = g.value("disabled", false);
    s.disabled_reason = g.value("disabled_reason", std::string());
    if (g.contains("cooldown_until_ms")) s.cooldown_until = from_epoch_ms(g["cooldown_until_ms"].get<long long>());
    if (g.contains("tune_failed_at_ms")) s.tune_failed_at = from_epoch_ms(g["tune_failed_at_ms"].get<long long>());
    s.tune_failure = g.value("tune_failure", std::string());
    if (g.contains("violations") && g["violations"].is_array()) {
      for (const auto& v : g["violations"]) {
        s.violations.push_back(ViolationRecord{from_epoch_ms(v.value("time_ms", 0LL)), v.value("kind", std::string()),
                                               v.value("reason", std::string())});
      }
    }
    gpus_[it.key()] = std::move(s);
  }
  return success();
}

GpuRuntimeState StateStore::get(const std::string& uuid) const {
  std::lock_guard lk(mu_);
  auto it = gpus_.find(uuid);
  return it == gpus_.end() ? GpuRuntimeState{} : it->second;
}

Result<void> StateStore::update(const std::string& uuid, const std::function<void(GpuRuntimeState&)>& fn) {
  std::lock_guard lk(mu_);
  GpuRuntimeState& s = gpus_[uuid];
  fn(s);
  // Keep the history bounded: one week is plenty for 24 h windows.
  if (s.violations.size() > 200) s.violations.erase(s.violations.begin(), s.violations.end() - 200);
  return save_locked();
}

std::string StateStore::get_global(const std::string& key) const {
  std::lock_guard lk(mu_);
  auto it = globals_.find(key);
  return it == globals_.end() ? std::string() : it->second;
}

Result<void> StateStore::set_global(const std::string& key, const std::string& value) {
  std::lock_guard lk(mu_);
  if (value.empty()) {
    globals_.erase(key);
  } else {
    globals_[key] = value;
  }
  return save_locked();
}

Result<void> StateStore::save_locked() const {
  if (read_only_) return success();
  json j{{"version", 1}, {"gpus", json::object()}, {"globals", json::object()}};
  for (const auto& [k, v] : globals_) j["globals"][k] = v;
  for (const auto& [uuid, s] : gpus_) {
    json g{{"disabled", s.disabled}, {"disabled_reason", s.disabled_reason}, {"tune_failure", s.tune_failure}};
    if (s.cooldown_until) g["cooldown_until_ms"] = to_epoch_ms(*s.cooldown_until);
    if (s.tune_failed_at) g["tune_failed_at_ms"] = to_epoch_ms(*s.tune_failed_at);
    json vs = json::array();
    for (const auto& v : s.violations)
      vs.push_back(json{{"time_ms", to_epoch_ms(v.time)}, {"time", format_iso8601(v.time)}, {"kind", v.kind}, {"reason", v.reason}});
    g["violations"] = vs;
    j["gpus"][uuid] = g;
  }
  return platform::write_file_atomic(file_, j.dump(2));
}

}  // namespace gpuoc
