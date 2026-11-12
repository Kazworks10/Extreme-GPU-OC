#include "core/json_codec.hpp"

#include <ctime>
#include <format>

namespace gpuoc {

using nlohmann::json;

json to_json(const Range& r) { return json{{"min", r.min}, {"max", r.max}}; }

json to_json(const FanCurve& c) {
  if (c.automatic()) return "auto";
  json a = json::array();
  for (const auto& p : c.points) a.push_back(json{{"temp_c", p.temp_c}, {"pct", p.pct}});
  return a;
}

Result<FanCurve> fan_curve_from_json(const json& j) {
  if (j.is_string() && j.get<std::string>() == "auto") return FanCurve::driver_auto();
  if (!j.is_array()) return make_error(ErrorCode::ParseError, "fan_curve must be \"auto\" or an array");
  FanCurve c;
  for (const auto& p : j) {
    if (!p.is_object() || !p.contains("temp_c") || !p.contains("pct") || !p["temp_c"].is_number_integer() ||
        !p["pct"].is_number_integer())
      return make_error(ErrorCode::ParseError, "fan_curve points need integer temp_c and pct");
    c.points.push_back(FanPoint{p["temp_c"].get<int>(), p["pct"].get<int>()});
  }
  GPUOC_TRY(c.validate());
  return c;
}

json to_json(const Settings& s) {
  json j = json::object();
  if (s.core_offset_mhz) j["core_offset_mhz"] = *s.core_offset_mhz;
  if (s.mem_offset_mhz) j["mem_offset_mhz"] = *s.mem_offset_mhz;
  if (s.power_limit_w) j["power_limit_w"] = *s.power_limit_w;
  if (s.voltage_mv) j["voltage_mv"] = *s.voltage_mv;
  if (s.fan_curve) j["fan_curve"] = to_json(*s.fan_curve);
  return j;
}

Result<Settings> settings_from_json(const json& j) {
  if (!j.is_object()) return make_error(ErrorCode::ParseError, "settings must be an object");
  Settings s;
  auto get_int = [&](const char* key, std::optional<int>& out) -> Result<void> {
    if (!j.contains(key) || j[key].is_null()) return success();
    if (!j[key].is_number_integer()) return make_error(ErrorCode::ParseError, std::format("{} must be an integer", key));
    out = j[key].get<int>();
    return success();
  };
  for (auto it = j.begin(); it != j.end(); ++it) {
    const auto& k = it.key();
    if (k != "core_offset_mhz" && k != "mem_offset_mhz" && k != "power_limit_w" && k != "voltage_mv" && k != "fan_curve")
      return make_error(ErrorCode::ParseError, std::format("unknown settings key '{}'", k));
  }
  GPUOC_TRY(get_int("core_offset_mhz", s.core_offset_mhz));
  GPUOC_TRY(get_int("mem_offset_mhz", s.mem_offset_mhz));
  GPUOC_TRY(get_int("power_limit_w", s.power_limit_w));
  GPUOC_TRY(get_int("voltage_mv", s.voltage_mv));
  if (j.contains("fan_curve") && !j["fan_curve"].is_null()) {
    GPUOC_TRY_ASSIGN(FanCurve c, fan_curve_from_json(j["fan_curve"]));
    s.fan_curve = c;
  }
  return s;
}

json to_json(const Telemetry& t) {
  json j{{"core_mhz", t.core_mhz},   {"mem_mhz", t.mem_mhz},   {"temp_c", t.temp_c},
         {"power_w", t.power_w},     {"fan_pct", t.fan_pct},   {"util_pct", t.util_pct},
         {"ecc_uncorrected", t.ecc_uncorrected}, {"limit_flags", t.limit_flags}};
  j["voltage_mv"] = t.voltage_mv ? json(*t.voltage_mv) : json(nullptr);
  return j;
}

json to_json(const Capabilities& c) {
  return json{{"core_offset", c.core_offset},
              {"mem_offset", c.mem_offset},
              {"power_limit", c.power_limit},
              {"fan_control", c.fan_control},
              {"voltage_control", c.voltage_control},
              {"locked_clocks", c.locked_clocks},
              {"core_offset_mhz", to_json(c.core_offset_mhz)},
              {"mem_offset_mhz", to_json(c.mem_offset_mhz)},
              {"power_limit_w", to_json(c.power_limit_w)},
              {"voltage_mv", to_json(c.voltage_mv)},
              {"stock_power_limit_w", c.stock_power_limit_w},
              {"stock_voltage_mv", c.stock_voltage_mv},
              {"voltage_step_mv", c.voltage_step_mv},
              {"fan_pct", to_json(c.fan_pct)},
              {"ecc_supported", c.ecc_supported},
              {"driver_version", c.driver_version}};
}

json to_json(const GpuId& id) { return json{{"uuid", id.uuid}, {"name", id.name}, {"index", id.index}}; }

long long to_epoch_ms(WallTime t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

WallTime from_epoch_ms(long long ms) {
  return WallTime(std::chrono::duration_cast<WallTime::duration>(std::chrono::milliseconds(ms)));
}

std::string format_iso8601(WallTime t) {
  const auto tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace gpuoc
