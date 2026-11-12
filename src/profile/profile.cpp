#include "profile/profile.hpp"

#include <cmath>
#include <format>
#include <sstream>

#include "core/toml_reader.hpp"

namespace gpuoc {

const char* to_string(ProfileMode m) noexcept {
  switch (m) {
    case ProfileMode::Stock: return "stock";
    case ProfileMode::Fixed: return "fixed";
    case ProfileMode::Auto: return "auto";
  }
  return "?";
}

ProfileTarget ProfileTarget::merged(const ProfileTarget& o) const {
  ProfileTarget t = *this;
  if (o.core_offset_mhz) t.core_offset_mhz = o.core_offset_mhz;
  if (o.mem_offset_mhz) t.mem_offset_mhz = o.mem_offset_mhz;
  if (o.power_limit_w) {
    t.power_limit_w = o.power_limit_w;
    t.power_limit_pct.reset();
  }
  if (o.power_limit_pct) {
    t.power_limit_pct = o.power_limit_pct;
    t.power_limit_w.reset();
  }
  if (o.voltage_mv) t.voltage_mv = o.voltage_mv;
  if (o.fan_curve) t.fan_curve = o.fan_curve;
  if (o.temp_target_c) t.temp_target_c = o.temp_target_c;
  return t;
}

bool valid_profile_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}

namespace {

Result<FanCurve> read_fan_curve(const toml::node& n, const std::string& where) {
  if (auto s = n.value<std::string>(); s && *s == "auto") return FanCurve::driver_auto();
  const toml::array* arr = n.as_array();
  if (!arr) return make_error(ErrorCode::ParseError, where + " must be \"auto\" or an array of [temp_c, pct] pairs");
  FanCurve c;
  for (const auto& el : *arr) {
    const toml::array* pair = el.as_array();
    if (!pair || pair->size() != 2 || !(*pair)[0].is_integer() || !(*pair)[1].is_integer())
      return make_error(ErrorCode::ParseError, where + " entries must be [temp_c, pct] integer pairs");
    c.points.push_back(FanPoint{static_cast<int>(*(*pair)[0].value<int64_t>()), static_cast<int>(*(*pair)[1].value<int64_t>())});
  }
  if (auto v = c.validate(); !v) return make_error(ErrorCode::ParseError, where + ": " + v.error().message);
  return c;
}

Result<ProfileTarget> read_target(TomlTableReader& r, const toml::table* tbl) {
  ProfileTarget t;
  GPUOC_TRY(r.get_opt_int("core_offset_mhz", t.core_offset_mhz, -1000, 2000));
  GPUOC_TRY(r.get_opt_int("mem_offset_mhz", t.mem_offset_mhz, -3000, 6000));
  GPUOC_TRY(r.get_opt_int("power_limit_w", t.power_limit_w, 1, 1500));
  GPUOC_TRY(r.get_opt_int("power_limit_pct", t.power_limit_pct, 10, 200));
  GPUOC_TRY(r.get_opt_int("voltage_mv", t.voltage_mv, 400, 1500));
  r.mark_known("temp_target_c");
  if (tbl && tbl->get("temp_target_c")) {
    double temp = 0;
    GPUOC_TRY(r.get_double("temp_target_c", temp, 30, HardLimits::kMaxTempC));
    t.temp_target_c = temp;
  }
  r.mark_known("fan_curve");
  return t;
}

Result<ProfileTarget> read_target_table(const toml::table* tbl, const std::string& path) {
  TomlTableReader r(tbl, path);
  GPUOC_TRY_ASSIGN(ProfileTarget t, read_target(r, tbl));
  if (tbl) {
    if (const toml::node* fc = tbl->get("fan_curve")) {
      GPUOC_TRY_ASSIGN(FanCurve c, read_fan_curve(*fc, path + ".fan_curve"));
      t.fan_curve = c;
    }
  }
  if (t.power_limit_w && t.power_limit_pct)
    return make_error(ErrorCode::ParseError, path + ": set power_limit_w or power_limit_pct, not both");
  GPUOC_TRY(r.finish());
  return t;
}

}  // namespace

Result<Profile> parse_profile(std::string_view text, const std::string& source) {
  auto prefix = [&](const Error& e) { return make_error(e.code, source + ": " + e.message); };
  auto root_r = parse_toml(text, source);
  if (!root_r) return root_r.error();
  const toml::table& root = root_r.value();
  TomlTableReader top(&root, "profile");
  Profile p;
  p.source = source;
  if (auto r = top.get_string("name", p.name); !r) return prefix(r.error());
  if (!valid_profile_name(p.name))
    return make_error(ErrorCode::ParseError,
                      std::format("{}: name '{}' must be 1-64 characters of letters, digits, '-' or '_'", source, p.name));
  if (auto r = top.get_string("description", p.description); !r) return prefix(r.error());
  std::string obj = to_string(p.objective);
  if (auto r = top.get_string("objective", obj); !r) return prefix(r.error());
  auto o = parse_objective(obj);
  if (!o) return prefix(o.error());
  p.objective = o.value();
  if (auto r = top.get_int("priority", p.priority, 0, 1000000); !r) return prefix(r.error());

  // [target]
  const toml::table* target = top.sub_table("target");
  std::string mode = target ? "fixed" : "stock";
  if (target) {
    if (const toml::node* m = target->get("mode")) {
      auto s = m->value<std::string>();
      if (!s) return make_error(ErrorCode::ParseError, source + ": target.mode must be a string");
      mode = *s;
    }
  }
  if (mode == "stock") {
    p.mode = ProfileMode::Stock;
  } else if (mode == "fixed") {
    p.mode = ProfileMode::Fixed;
  } else if (mode == "auto") {
    p.mode = ProfileMode::Auto;
  } else {
    return make_error(ErrorCode::ParseError, source + ": target.mode must be \"stock\", \"fixed\" or \"auto\"");
  }
  if (target) {
    // Re-read without "mode" as a field.
    toml::table copy = *target;
    copy.erase("mode");
    auto t = read_target_table(&copy, "target");
    if (!t) return prefix(t.error());
    p.target = t.value();
  }
  const bool has_clock_fields = p.target.core_offset_mhz || p.target.mem_offset_mhz || p.target.power_limit_w ||
                                p.target.power_limit_pct || p.target.voltage_mv || p.target.fan_curve;
  if (p.mode != ProfileMode::Fixed && has_clock_fields)
    return make_error(ErrorCode::ParseError,
                      std::format("{}: target.mode = \"{}\" cannot set clock, power, voltage or fan fields", source, mode));

  // [tuning]
  {
    TomlTableReader r(top.sub_table("tuning"), "tuning");
    std::optional<int> budget;
    if (auto e = r.get_opt_int("budget_s", budget, 60, 7 * 24 * 3600); !e) return prefix(e.error());
    if (budget) p.tuning_budget = Sec(*budget);
    if (auto e = r.get_opt_int("margin_steps", p.tuning_margin_steps, 0, 10); !e) return prefix(e.error());
    if (auto e = r.finish(); !e) return prefix(e.error());
  }

  // [[match]]
  if (const toml::array* arr = top.array("match")) {
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const toml::table* m = (*arr)[i].as_table();
      if (!m) return make_error(ErrorCode::ParseError, source + ": [[match]] entries must be tables");
      TomlTableReader r(m, std::format("match[{}]", i));
      MatchRule rule;
      if (auto e = r.get_string("exe", rule.exe); !e) return prefix(e.error());
      if (auto e = r.get_string("path", rule.path); !e) return prefix(e.error());
      if (auto e = r.finish(); !e) return prefix(e.error());
      if (rule.exe.empty() == rule.path.empty())
        return make_error(ErrorCode::ParseError, std::format("{}: match[{}] needs exactly one of exe or path", source, i));
      p.match.push_back(rule);
    }
  }

  // [gpu."GPU-..."] per-GPU overrides
  if (const toml::table* gpus = top.sub_table("gpu")) {
    for (const auto& [k, v] : *gpus) {
      const std::string uuid(k.str());
      if (!uuid.starts_with("GPU-"))
        return make_error(ErrorCode::ParseError, std::format("{}: [gpu.\"{}\"] must be keyed by a GPU UUID (GPU-...)", source, uuid));
      if (p.mode != ProfileMode::Fixed)
        return make_error(ErrorCode::ParseError, source + ": per-GPU overrides need target.mode = \"fixed\"");
      auto t = read_target_table(v.as_table(), "gpu." + uuid);
      if (!t) return prefix(t.error());
      p.gpu_overrides[uuid] = t.value();
    }
  }
  if (auto e = top.finish(); !e) return prefix(e.error());
  return p;
}

namespace {

std::string toml_quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out.push_back(c);
  }
  return out + "\"";
}

void write_target(std::ostringstream& o, const ProfileTarget& t) {
  if (t.core_offset_mhz) o << "core_offset_mhz = " << *t.core_offset_mhz << "\n";
  if (t.mem_offset_mhz) o << "mem_offset_mhz = " << *t.mem_offset_mhz << "\n";
  if (t.power_limit_w) o << "power_limit_w = " << *t.power_limit_w << "\n";
  if (t.power_limit_pct) o << "power_limit_pct = " << *t.power_limit_pct << "\n";
  if (t.voltage_mv) o << "voltage_mv = " << *t.voltage_mv << "\n";
  if (t.temp_target_c) o << "temp_target_c = " << std::format("{:.1f}", *t.temp_target_c) << "\n";
  if (t.fan_curve) {
    if (t.fan_curve->automatic()) {
      o << "fan_curve = \"auto\"\n";
    } else {
      o << "fan_curve = [";
      for (std::size_t i = 0; i < t.fan_curve->points.size(); ++i)
        o << (i ? ", " : "") << "[" << t.fan_curve->points[i].temp_c << ", " << t.fan_curve->points[i].pct << "]";
      o << "]\n";
    }
  }
}

}  // namespace

std::string serialize_profile(const Profile& p) {
  std::ostringstream o;
  o << "name = " << toml_quote(p.name) << "\n";
  if (!p.description.empty()) o << "description = " << toml_quote(p.description) << "\n";
  o << "objective = " << toml_quote(to_string(p.objective)) << "\n";
  if (p.priority != 100) o << "priority = " << p.priority << "\n";
  o << "\n[target]\nmode = " << toml_quote(to_string(p.mode)) << "\n";
  write_target(o, p.target);
  if (p.tuning_budget || p.tuning_margin_steps) {
    o << "\n[tuning]\n";
    if (p.tuning_budget) o << "budget_s = " << p.tuning_budget->count() << "\n";
    if (p.tuning_margin_steps) o << "margin_steps = " << *p.tuning_margin_steps << "\n";
  }
  for (const auto& m : p.match) {
    o << "\n[[match]]\n";
    if (!m.exe.empty()) o << "exe = " << toml_quote(m.exe) << "\n";
    if (!m.path.empty()) o << "path = " << toml_quote(m.path) << "\n";
  }
  for (const auto& [uuid, t] : p.gpu_overrides) {
    o << "\n[gpu." << toml_quote(uuid) << "]\n";
    write_target(o, t);
  }
  return o.str();
}

std::vector<Profile> builtin_profiles() {
  std::vector<Profile> v;
  Profile stock;
  stock.name = "stock";
  stock.description = "Driver defaults. Always present; cannot be deleted.";
  stock.mode = ProfileMode::Stock;
  v.push_back(stock);

  Profile quiet;
  quiet.name = "quiet";
  quiet.description = "80% power limit and a gentle fan curve, stock clocks.";
  quiet.objective = Objective::MaxPerfPerWatt;
  quiet.mode = ProfileMode::Fixed;
  quiet.target.core_offset_mhz = 0;
  quiet.target.mem_offset_mhz = 0;
  quiet.target.power_limit_pct = 80;
  quiet.target.fan_curve = FanCurve{{{40, 30}, {60, 35}, {75, 55}, {85, 100}}};
  v.push_back(quiet);

  Profile balanced;
  balanced.name = "balanced";
  balanced.description = "Validated overclock tuned for the balanced objective.";
  balanced.objective = Objective::Balanced;
  balanced.mode = ProfileMode::Auto;
  v.push_back(balanced);

  Profile max;
  max.name = "max";
  max.description = "Validated overclock tuned for maximum performance.";
  max.objective = Objective::MaxPerformance;
  max.mode = ProfileMode::Auto;
  v.push_back(max);

  for (auto& p : v) {
    p.builtin = true;
    p.source = "builtin";
  }
  return v;
}

Result<ResolvedProfile> resolve_profile(const Profile& p, const GpuId& gpu, const Capabilities& caps,
                                        const SafetyPolicy& policy, const LkgStore* lkg) {
  ResolvedProfile r;
  r.objective = p.objective;
  ProfileTarget t = p.target;
  if (auto it = p.gpu_overrides.find(gpu.uuid); it != p.gpu_overrides.end()) t = t.merged(it->second);
  r.temp_target_c = t.temp_target_c;
  switch (p.mode) {
    case ProfileMode::Stock:
      r.settings = stock_settings_for(caps);
      if (r.settings.voltage_mv && !policy.config().allow_voltage) r.settings.voltage_mv.reset();
      return r;
    case ProfileMode::Auto: {
      std::optional<LkgEntry> e = lkg ? lkg->get(gpu.uuid, p.objective) : std::nullopt;
      if (!e) {
        r.needs_tuning = true;
        return r;
      }
      r.settings = e->settings;
      r.validated = true;
      break;
    }
    case ProfileMode::Fixed: {
      Settings s;
      s.core_offset_mhz = t.core_offset_mhz;
      s.mem_offset_mhz = t.mem_offset_mhz;
      s.voltage_mv = t.voltage_mv;
      s.fan_curve = t.fan_curve;
      if (t.power_limit_w) s.power_limit_w = t.power_limit_w;
      if (t.power_limit_pct) {
        if (caps.stock_power_limit_w <= 0)
          return make_error(ErrorCode::Unsupported, std::format("profile '{}': stock TDP unknown for {}", p.name, gpu.uuid));
        s.power_limit_w = static_cast<int>(std::lround(caps.stock_power_limit_w * *t.power_limit_pct / 100.0));
      }
      r.settings = s;
      break;
    }
  }
  // Built-ins and user profiles go through the same limits.
  auto checked = policy.check(caps, r.settings, ClampMode::Reject);
  if (!checked)
    return make_error(checked.error().code,
                      std::format("profile '{}' on {}: {}", p.name, gpu.uuid, checked.error().message));
  return r;
}

}  // namespace gpuoc
