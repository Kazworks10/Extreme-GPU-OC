// JSON encoding of the device model (LKG store, tuning log, JSON-RPC, --json).
#pragma once

#include <nlohmann/json.hpp>

#include "gpuoc/clock.hpp"
#include "gpuoc/types.hpp"

namespace gpuoc {

nlohmann::json to_json(const Settings& s);
Result<Settings> settings_from_json(const nlohmann::json& j);
nlohmann::json to_json(const FanCurve& c);
Result<FanCurve> fan_curve_from_json(const nlohmann::json& j);
nlohmann::json to_json(const Telemetry& t);
nlohmann::json to_json(const Capabilities& c);
nlohmann::json to_json(const GpuId& id);
nlohmann::json to_json(const Range& r);

// Wall time as integer milliseconds since the Unix epoch.
long long to_epoch_ms(WallTime t);
WallTime from_epoch_ms(long long ms);
std::string format_iso8601(WallTime t);

}  // namespace gpuoc
