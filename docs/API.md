# API

## C++ overclocking API (`namespace gpuoc`)

Include `gpuoc/api.hpp`. Nothing throws across the API: every fallible call returns `Result<T>`, which holds either a value or an `Error{code, message}`. NVML and NVAPI status codes are converted at the backend boundary (see `nvml_error` and `nvapi_error`).

```cpp
struct GpuId { std::string uuid; std::string name; int index; };

struct Capabilities {
  bool core_offset, mem_offset, power_limit, fan_control, voltage_control, locked_clocks;
  Range core_offset_mhz, mem_offset_mhz;   // as reported by the driver
  Range power_limit_w;
  Range voltage_mv;                        // only meaningful if voltage_control
  // Extensions:
  int stock_power_limit_w;   // driver default (stock TDP)
  int stock_voltage_mv;      // nominal voltage when voltage_control
  int voltage_step_mv;       // 0 = not reported (the safety policy then uses 6 mV)
  Range fan_pct;             // driver min/max manual fan speed
  bool ecc_supported;
  std::string driver_version;
};

struct Telemetry {
  double core_mhz, mem_mhz, temp_c, power_w, fan_pct, util_pct;
  std::optional<double> voltage_mv;
  std::uint64_t ecc_uncorrected;           // 0 if unsupported
  std::uint32_t limit_flags;               // kLimitPower | kLimitThermal | kLimitHwSlowdown
  std::chrono::steady_clock::time_point ts;
};

struct Settings {                          // nullopt = leave unchanged
  std::optional<int> core_offset_mhz, mem_offset_mhz, power_limit_w, voltage_mv;
  std::optional<FanCurve> fan_curve;       // an empty curve means driver-automatic
};

class IGpuBackend {
 public:
  virtual Result<std::vector<GpuId>> enumerate() = 0;
  virtual Result<Capabilities>       capabilities(const GpuId&) = 0;
  virtual Result<Telemetry>          read_telemetry(const GpuId&) = 0;
  virtual Result<Settings>           read_settings(const GpuId&) = 0;
  virtual Result<void>               apply(const GpuId&, const Settings&) = 0;
  virtual Result<void>               reset_to_stock(const GpuId&) = 0;
  virtual const char*                name() const noexcept = 0;
};
```

`Gpu` wraps a backend. It enforces capability checks, the driver ranges, the safety limits and gates, and read-back verification. `StockGuard` restores stock on every exit path.

### Example

```cpp
#include "gpuoc/api.hpp"
#include "backends/nvidia/nvidia_backend.hpp"

using namespace gpuoc;

int main() {
  auto backend = make_nvidia_backend();              // Result<std::shared_ptr<IGpuBackend>>
  if (!backend) return 2;                            // e.g. nvml.dll missing -> Unavailable

  SafetyConfig cfg;                                  // hard ceilings always apply
  auto policy = std::make_shared<SafetyPolicy>(cfg);
  GpuId id = backend.value()->enumerate().value().at(0);
  Gpu gpu(backend.value(), id, policy);

  StockGuard guard(gpu);                             // stock comes back however we leave
  Settings s;
  s.core_offset_mhz = 60;
  s.mem_offset_mhz = 200;
  auto r = gpu.apply(s, ClampMode::Reject);          // OutOfRange / SafetyRefusal / VerifyFailed ...
  if (!r) { std::puts(r.error().to_string().c_str()); return 3; }

  auto t = gpu.telemetry();
  if (t) std::printf("%.0f C, %.0f W\n", t.value().temp_c, t.value().power_w);
  return 0;                                          // ~StockGuard -> reset_to_stock()
}
```

## JSON-RPC 2.0 over `\\.\pipe\gpuoc`

The service accepts one request and sends one response per connection, in message mode. Methods marked *write* require the caller to be an elevated Administrator (checked per call by impersonating the client). Errors use the standard codes (`-32700`, `-32600`, `-32601`, `-32602`, `-32603`) plus application codes:

| Code | `ErrorCode` | Code | `ErrorCode` |
|---|---|---|---|
| -32001 | InvalidArgument | -32009 | VerifyFailed |
| -32002 | Unsupported | -32010 | GpuLost |
| -32003 | SafetyRefusal | -32011 | Timeout |
| -32004 | HardwareError | -32012 | Aborted |
| -32005 | PermissionDenied | -32013 | Unavailable |
| -32006 | NotFound | -32014 | ParseError |
| -32007 | Busy | -32015 | IoError |
| -32008 | OutOfRange | | |

| Method | Write | Params | Result |
|---|---|---|---|
| `list` | | — | `[{uuid, name, index, driver_version}]` |
| `status` | | — | `{service, gpus: [{…, capabilities, limits, telemetry, settings, lkg, controller}]}` |
| `set` | ✔ | `{gpu?, settings}` | `[{gpu, applied}]` |
| `reset` | ✔ | `{gpu?}` | `{reset: [uuid]}` |
| `auto` | ✔ | `{gpu?}` | Drops a manual target and returns to automatic mode |
| `tune.start` | ✔ | `{gpu?, objective?, budget_s?}` | `{tuning_requested: [uuid]}` |
| `tune.status` | | — | `[{gpu, controller, log}]` |
| `tune.abort` | ✔ | `{gpu?}` | `{aborted: true}` |
| `profile.list` | | — | `{profiles, default, errors}` |
| `profile.apply` | ✔ | `{name, gpu?}` | `[{gpu, profile, mode, settings?}]` |
| `profile.activate` | ✔ | `{name}` | `{default_profile, applied}` |
| `profile.reload` | ✔ | — | `{profiles, errors}` |
| `gpu.enable` | ✔ | `{gpu?}` | Re-enables auto-overclock after disablement |

`gpu` is a GPU index, a UUID, or a unique UUID prefix. Leave it out to target every GPU. `settings` uses the JSON form shown below.

```json
{"jsonrpc":"2.0","id":1,"method":"set","params":{"gpu":"0","settings":{"core_offset_mhz":60,"power_limit_w":300}}}
```

## File formats

**Settings (JSON)**: `{"core_offset_mhz": int, "mem_offset_mhz": int, "power_limit_w": int, "voltage_mv": int, "fan_curve": "auto" | [{"temp_c": int, "pct": int}, …]}`. Every key is optional.

**`lkg.json`**: `{"version": 1, "entries": [{gpu_uuid, gpu_name, driver_version, objective, settings, validated: true, validated_at_ms, validated_at, validation_duration_s, score, baseline_score, score_unit, power_w, max_temp_c, revoked, revoke_reason}]}`. When the file is loaded, entries with `validated: false` are ignored.

**Tuning log** (`logs\tune-<uuid>.jsonl`, append-only, each line flushed to disk before the next trial):

```
{"type":"session","session":"…","gpu":"GPU-…","driver":"566.36","objective":"…","params":"<hash>","started_ms":…}
{"type":"trial_start","seq":3,"stage":"mem","kind":"stress","settings":{…},"duration_s":120}
{"type":"trial_result","seq":3,"verdict":"stable","reason":"","score":…,"avg_power_w":…,"max_temp_c":…,"elapsed_s":120.0}
{"type":"suspect_region","stage":"core","from":120,"to":135}
{"type":"stage_done","stage":"core","found":195}
{"type":"validated","settings":{…},"score":…}
{"type":"session_end","reason":"validated"}
```

**`state.json`**: per GPU, `violations[]`, `cooldown_until_ms`, `disabled`, `disabled_reason`, `tune_failed_at_ms`, `tune_failure`; plus `globals.active_profile`.

**Profiles (TOML)**: see [`examples/profiles`](../examples/profiles).

```toml
name = "gaming"                 # letters, digits, '-', '_'
description = "..."
objective = "max_performance"   # max_performance | max_perf_per_watt | balanced
priority = 10                   # auto-switch order (lower first, then name)

[target]
mode = "fixed"                  # stock | fixed | auto (validated LKG for the objective)
core_offset_mhz = 90            # fixed only
mem_offset_mhz = 400
power_limit_w = 300             # or power_limit_pct = 90 (percent of stock TDP)
voltage_mv = 1050               # needs safety.allow_voltage and driver support
fan_curve = [[40, 30], [60, 50], [80, 100]]   # or "auto"
temp_target_c = 72              # can only tighten the global target

[tuning]
budget_s = 7200
margin_steps = 2

[[match]]
exe = "game*.exe"               # executable-name glob, case-insensitive
[[match]]
path = "C:\\Games\\X\\x.exe"     # or a full path

[gpu."GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"]
core_offset_mhz = 60            # per-GPU override of [target]
```
