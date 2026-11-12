// Profiles: named TOML files with an objective, target settings, tuning
// options, per-GPU overrides, and process match rules.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.hpp"
#include "core/lkg_store.hpp"
#include "gpuoc/safety.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc {

enum class ProfileMode {
  Stock,  // stock settings
  Fixed,  // the target fields as written (limits still enforced)
  Auto,   // the validated (LKG) settings tuned for the objective
};

const char* to_string(ProfileMode m) noexcept;

struct ProfileTarget {
  std::optional<int> core_offset_mhz;
  std::optional<int> mem_offset_mhz;
  std::optional<int> power_limit_w;
  std::optional<int> power_limit_pct;  // percent of stock TDP
  std::optional<int> voltage_mv;
  std::optional<FanCurve> fan_curve;
  std::optional<double> temp_target_c;  // can only tighten the global target
  [[nodiscard]] ProfileTarget merged(const ProfileTarget& over) const;
  bool operator==(const ProfileTarget&) const = default;
};

struct MatchRule {
  std::string exe;   // executable name glob, e.g. "game*.exe" (case-insensitive)
  std::string path;  // full executable path (case-insensitive)
  bool operator==(const MatchRule&) const = default;
};

struct Profile {
  std::string name;
  std::string description;
  Objective objective = Objective::MaxPerfPerWatt;
  ProfileMode mode = ProfileMode::Fixed;
  ProfileTarget target;
  std::map<std::string, ProfileTarget> gpu_overrides;  // keyed by GPU UUID
  std::optional<Sec> tuning_budget;
  std::optional<int> tuning_margin_steps;
  std::vector<MatchRule> match;
  int priority = 100;  // auto-switch order: lower first, then name
  // Not serialized:
  std::string source;  // "builtin" or a file path
  bool builtin = false;
};

bool valid_profile_name(std::string_view name);

// Parse a profile. Invalid TOML, unknown keys, wrong types, and out-of-range
// values are errors with the file name and key in the message.
Result<Profile> parse_profile(std::string_view toml_text, const std::string& source);
std::string serialize_profile(const Profile& p);

std::vector<Profile> builtin_profiles();

// What a profile means for one GPU.
struct ResolvedProfile {
  Settings settings;
  bool validated = false;     // came from the LKG store
  bool needs_tuning = false;  // Auto profile without a validated LKG entry
  Objective objective = Objective::MaxPerfPerWatt;
  std::optional<double> temp_target_c;
};

// Resolve and check against the GPU's effective limits (same rules as user
// input: out-of-range values are rejected, never clamped).
Result<ResolvedProfile> resolve_profile(const Profile& p, const GpuId& gpu, const Capabilities& caps,
                                        const SafetyPolicy& policy, const LkgStore* lkg);

}  // namespace gpuoc
