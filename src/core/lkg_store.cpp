#include "core/lkg_store.hpp"

#include <algorithm>
#include <format>

#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"

namespace gpuoc {

using nlohmann::json;

namespace {

json entry_to_json(const LkgEntry& e) {
  return json{{"gpu_uuid", e.gpu_uuid},
              {"gpu_name", e.gpu_name},
              {"driver_version", e.driver_version},
              {"objective", to_string(e.objective)},
              {"settings", to_json(e.settings)},
              {"validated", e.validated},
              {"validated_at_ms", to_epoch_ms(e.validated_at)},
              {"validated_at", format_iso8601(e.validated_at)},
              {"validation_duration_s", e.validation_duration.count()},
              {"score", e.score},
              {"baseline_score", e.baseline_score},
              {"score_unit", e.score_unit},
              {"power_w", e.power_w},
              {"max_temp_c", e.max_temp_c},
              {"revoked", e.revoked},
              {"revoke_reason", e.revoke_reason}};
}

Result<LkgEntry> entry_from_json(const json& j) {
  if (!j.is_object()) return make_error(ErrorCode::ParseError, "LKG entry must be an object");
  LkgEntry e;
  e.gpu_uuid = j.value("gpu_uuid", std::string());
  e.gpu_name = j.value("gpu_name", std::string());
  e.driver_version = j.value("driver_version", std::string());
  GPUOC_TRY_ASSIGN(e.objective, parse_objective(j.value("objective", std::string("max_perf_per_watt"))));
  if (!j.contains("settings")) return make_error(ErrorCode::ParseError, "LKG entry without settings");
  GPUOC_TRY_ASSIGN(e.settings, settings_from_json(j["settings"]));
  e.validated = j.value("validated", false);
  e.validated_at = from_epoch_ms(j.value("validated_at_ms", 0LL));
  e.validation_duration = Sec(j.value("validation_duration_s", 0LL));
  e.score = j.value("score", 0.0);
  e.baseline_score = j.value("baseline_score", 0.0);
  e.score_unit = j.value("score_unit", std::string());
  e.power_w = j.value("power_w", 0.0);
  e.max_temp_c = j.value("max_temp_c", 0.0);
  e.revoked = j.value("revoked", false);
  e.revoke_reason = j.value("revoke_reason", std::string());
  if (e.gpu_uuid.empty()) return make_error(ErrorCode::ParseError, "LKG entry without gpu_uuid");
  return e;
}

}  // namespace

LkgStore::LkgStore(std::filesystem::path file, bool read_only) : file_(std::move(file)), read_only_(read_only) {}

Result<void> LkgStore::load() {
  std::lock_guard lk(mu_);
  entries_.clear();
  auto text = platform::read_file(file_);
  if (!text) return text.error().code == ErrorCode::NotFound ? success() : Result<void>(text.error());
  json j = json::parse(text.value(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("entries") || !j["entries"].is_array())
    return make_error(ErrorCode::ParseError, std::format("{} is not a valid LKG store", platform::path_utf8(file_)));
  for (const auto& je : j["entries"]) {
    auto e = entry_from_json(je);
    if (!e) {
      log::warn("ignoring malformed LKG entry: {}", e.error().message);
      continue;
    }
    if (!e.value().validated) {
      // Defense in depth: never trust an unvalidated entry, even if the file says so.
      log::warn("ignoring unvalidated LKG entry for {}", e.value().gpu_uuid);
      continue;
    }
    entries_.push_back(std::move(e).value());
  }
  return success();
}

Result<void> LkgStore::save_locked() const {
  if (read_only_) return success();
  json j{{"version", 1}, {"entries", json::array()}};
  for (const auto& e : entries_) j["entries"].push_back(entry_to_json(e));
  return platform::write_file_atomic(file_, j.dump(2));
}

Result<void> LkgStore::commit(const LkgEntry& entry) {
  if (!entry.validated)
    return make_error(ErrorCode::SafetyRefusal, "only validated settings may enter the LKG store");
  if (entry.gpu_uuid.empty()) return make_error(ErrorCode::InvalidArgument, "LKG entry needs a GPU UUID");
  std::lock_guard lk(mu_);
  if (read_only_) {
    log::info("[dry-run] would commit LKG for {} ({}): {}", entry.gpu_uuid, to_string(entry.objective),
              entry.settings.to_string());
    return success();
  }
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const LkgEntry& e) {
    return e.gpu_uuid == entry.gpu_uuid && e.objective == entry.objective;
  });
  if (it != entries_.end()) {
    *it = entry;
  } else {
    entries_.push_back(entry);
  }
  return save_locked();
}

Result<void> LkgStore::revoke(const std::string& gpu_uuid, const std::string& reason) {
  std::lock_guard lk(mu_);
  bool any = false;
  for (auto& e : entries_) {
    if (e.gpu_uuid == gpu_uuid && !e.revoked) {
      e.revoked = true;
      e.revoke_reason = reason;
      any = true;
    }
  }
  if (!any) return success();
  if (read_only_) {
    log::info("[dry-run] would revoke LKG for {}: {}", gpu_uuid, reason);
    return success();
  }
  return save_locked();
}

std::optional<LkgEntry> LkgStore::get(const std::string& gpu_uuid, Objective objective) const {
  std::lock_guard lk(mu_);
  for (const auto& e : entries_) {
    if (e.gpu_uuid == gpu_uuid && e.objective == objective && e.validated && !e.revoked) return e;
  }
  return std::nullopt;
}

std::optional<LkgEntry> LkgStore::latest(const std::string& gpu_uuid) const {
  std::lock_guard lk(mu_);
  const LkgEntry* best = nullptr;
  for (const auto& e : entries_) {
    if (e.gpu_uuid != gpu_uuid || !e.validated || e.revoked) continue;
    if (!best || e.validated_at > best->validated_at) best = &e;
  }
  return best ? std::optional<LkgEntry>(*best) : std::nullopt;
}

std::vector<LkgEntry> LkgStore::all() const {
  std::lock_guard lk(mu_);
  return entries_;
}

}  // namespace gpuoc
