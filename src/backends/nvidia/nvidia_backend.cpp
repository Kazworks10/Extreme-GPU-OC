#include "backends/nvidia/nvidia_backend.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "backends/nvidia/nvidia_backend_impl.hpp"
#include "core/log.hpp"
#include "gpuoc/safety.hpp"

namespace gpuoc {
namespace nvidia {

namespace {
// Floor/ceil division that keeps ranges inside the driver range.
int div_ceil(int a, int b) { return a >= 0 ? (a + b - 1) / b : -((-a) / b); }
int div_floor(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

Capabilities derive_capabilities(const NvmlFacts& f, const std::optional<Pstates20>& p, bool can_set) {
  Capabilities c;
  c.driver_version = f.driver_version;
  c.locked_clocks = f.locked_clocks_fn;
  c.ecc_supported = f.ecc_ok;

  if (f.power_ok && f.power_min_mw > 0 && f.power_max_mw >= f.power_min_mw && f.power_default_mw > 0) {
    c.power_limit = true;
    c.power_limit_w = Range{div_ceil(static_cast<int>(f.power_min_mw), 1000), div_floor(static_cast<int>(f.power_max_mw), 1000)};
    c.stock_power_limit_w = static_cast<int>(std::lround(f.power_default_mw / 1000.0));
    if (!c.power_limit_w.valid()) c.power_limit = false;
  }
  if (f.fans_ok && f.fan_count > 0) {
    c.fan_control = true;
    c.fan_pct = Range{static_cast<int>(f.fan_min_pct), static_cast<int>(f.fan_max_pct)};
  }
  if (p && can_set && p->editable && p->p0_editable) {
    if (p->graphics_khz.present && p->graphics_khz.editable && p->graphics_khz.max > p->graphics_khz.min) {
      c.core_offset = true;
      c.core_offset_mhz = Range{div_ceil(p->graphics_khz.min, 1000), div_floor(p->graphics_khz.max, 1000)};
    }
    if (p->memory_khz.present && p->memory_khz.editable && p->memory_khz.max > p->memory_khz.min) {
      c.mem_offset = true;
      c.mem_offset_mhz = Range{div_ceil(p->memory_khz.min, 1000), div_floor(p->memory_khz.max, 1000)};
    }
    if (p->has_core_voltage && p->voltage_uv.present && p->voltage_uv.editable && p->voltage_uv.max > p->voltage_uv.min &&
        p->base_voltage_uv > 0) {
      const int nominal_uv = p->base_voltage_uv - p->voltage_uv.value;
      c.voltage_control = true;
      c.voltage_mv = Range{div_ceil(nominal_uv + p->voltage_uv.min, 1000), div_floor(nominal_uv + p->voltage_uv.max, 1000)};
      c.stock_voltage_mv = static_cast<int>(std::lround(nominal_uv / 1000.0));
      c.voltage_step_mv = 0;  // NVAPI reports no step; SafetyPolicy uses its documented fallback
      if (!c.voltage_mv.valid()) c.voltage_control = false;
    }
  }
  return c;
}

NvidiaBackend::NvidiaBackend(std::unique_ptr<INvmlApi> nvml, std::unique_ptr<INvapiApi> nvapi)
    : nvml_(std::move(nvml)), nvapi_(std::move(nvapi)) {}

NvmlFacts NvidiaBackend::gather(unsigned i) {
  NvmlFacts f;
  f.driver_version = driver_;
  auto c = nvml_->power_limit_constraints_mw(i);
  auto def = nvml_->power_limit_default_mw(i);
  f.power_ok = c && def && nvml_->has("nvmlDeviceSetPowerManagementLimit") && nvml_->has("nvmlDeviceGetPowerManagementLimit");
  if (c) {
    f.power_min_mw = c.value().first;
    f.power_max_mw = c.value().second;
  }
  if (def) f.power_default_mw = def.value();
  auto fans = nvml_->fan_count(i);
  auto mm = nvml_->fan_min_max_pct(i);
  f.fans_ok = fans && fans.value() > 0 && mm && nvml_->has("nvmlDeviceSetFanSpeed_v2") &&
              nvml_->has("nvmlDeviceSetDefaultFanSpeed_v2");
  if (fans) f.fan_count = fans.value();
  if (mm) {
    f.fan_min_pct = mm.value().first;
    f.fan_max_pct = mm.value().second;
  }
  f.ecc_ok = nvml_->ecc_uncorrected(i).ok();
  f.locked_clocks_fn = nvml_->has("nvmlDeviceSetGpuLockedClocks");
  return f;
}

Result<void> NvidiaBackend::init() {
  std::lock_guard lk(mu_);
  GPUOC_TRY(nvml_->init());
  GPUOC_TRY_ASSIGN(driver_, nvml_->driver_version());
  GPUOC_TRY_ASSIGN(unsigned n, nvml_->device_count());
  std::vector<NvapiGpu> nv;
  if (nvapi_) {
    auto e = nvapi_->enumerate();
    if (e) {
      nv = e.value();
    } else {
      log::warn("NVAPI enumeration failed: {}; clock offsets and voltage unavailable", e.error().message);
    }
  }
  devices_.clear();
  for (unsigned i = 0; i < n; ++i) {
    Device d;
    GPUOC_TRY_ASSIGN(d.info, nvml_->device_info(i));
    // NVML and NVAPI enumerate in different orders; match by PCI bus.
    int matches = 0;
    for (const auto& g : nv) {
      if (g.pci_bus == d.info.pci_bus) {
        d.nvapi = g.handle;
        ++matches;
      }
    }
    if (matches > 1) {
      log::warn("[{}] ambiguous NVAPI mapping (bus {}); offsets disabled for this GPU", d.info.uuid, d.info.pci_bus);
      d.nvapi.reset();
    }
    d.facts = gather(i);
    devices_.push_back(std::move(d));
  }
  return success();
}

Result<NvidiaBackend::Device*> NvidiaBackend::find(const GpuId& id) {
  for (auto& d : devices_) {
    if (d.info.uuid == id.uuid) return &d;
  }
  return make_error(ErrorCode::NotFound, "no NVIDIA GPU with UUID " + id.uuid);
}

std::optional<Pstates20> NvidiaBackend::pstates(const Device& d) {
  if (!nvapi_ || !d.nvapi) return std::nullopt;
  auto p = nvapi_->get_pstates20(*d.nvapi);
  if (!p) {
    log::debug("[{}] GetPstates20: {}", d.info.uuid, p.error().message);
    return std::nullopt;
  }
  return p.value();
}

Result<std::vector<GpuId>> NvidiaBackend::enumerate() {
  std::lock_guard lk(mu_);
  std::vector<GpuId> out;
  for (const auto& d : devices_) out.push_back(GpuId{d.info.uuid, d.info.name, static_cast<int>(d.info.index)});
  return out;
}

Result<Capabilities> NvidiaBackend::capabilities(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(Device * d, find(id));
  return derive_capabilities(d->facts, pstates(*d), nvapi_ && nvapi_->can_set_pstates20());
}

Result<Telemetry> NvidiaBackend::read_telemetry(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(Device * d, find(id));
  const unsigned i = d->info.index;
  Telemetry t;
  t.ts = std::chrono::steady_clock::now();
  // Temperature and power are mandatory for safety; their failure is fatal
  // for this sample (GpuLost propagates as such).
  GPUOC_TRY_ASSIGN(unsigned temp, nvml_->temperature_c(i));
  t.temp_c = temp;
  GPUOC_TRY_ASSIGN(unsigned mw, nvml_->power_mw(i));
  t.power_w = mw / 1000.0;
  if (auto c = nvml_->clock_mhz(i, NvmlClock::Graphics)) t.core_mhz = c.value();
  if (auto c = nvml_->clock_mhz(i, NvmlClock::Memory)) t.mem_mhz = c.value();
  if (auto u = nvml_->utilization_pct(i)) t.util_pct = u.value();
  if (d->facts.fan_count > 0) {
    double sum = 0;
    int n = 0;
    for (unsigned f = 0; f < d->facts.fan_count; ++f) {
      if (auto s = nvml_->fan_speed_pct(i, f)) {
        sum += s.value();
        ++n;
      }
    }
    if (n) t.fan_pct = sum / n;
  }
  if (auto e = nvml_->ecc_uncorrected(i)) t.ecc_uncorrected = e.value();  // 0 if unsupported
  if (auto r = nvml_->clock_event_reasons(i)) t.limit_flags = limit_flags_from_reasons(r.value());
  if (auto p = pstates(*d); p && p->has_core_voltage) t.voltage_mv = p->base_voltage_uv / 1000.0;
  return t;
}

Result<Settings> NvidiaBackend::read_settings(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(Device * d, find(id));
  Settings s;
  if (auto p = pstates(*d)) {
    if (p->graphics_khz.present) s.core_offset_mhz = static_cast<int>(std::lround(p->graphics_khz.value / 1000.0));
    if (p->memory_khz.present) s.mem_offset_mhz = static_cast<int>(std::lround(p->memory_khz.value / 1000.0));
    if (p->has_core_voltage) s.voltage_mv = static_cast<int>(std::lround(p->base_voltage_uv / 1000.0));
  }
  if (d->facts.power_ok) {
    GPUOC_TRY_ASSIGN(unsigned mw, nvml_->power_limit_mw(d->info.index));
    s.power_limit_w = static_cast<int>(std::lround(mw / 1000.0));
  }
  if (d->facts.fans_ok) s.fan_curve = d->fan;
  return s;
}

Result<void> NvidiaBackend::set_fans(Device& d, const FanCurve& curve) {
  const unsigned i = d.info.index;
  if (curve.automatic()) {
    for (unsigned f = 0; f < d.facts.fan_count; ++f) GPUOC_TRY(nvml_->set_fan_default(i, f));
    d.fan = curve;
    return success();
  }
  GPUOC_TRY_ASSIGN(unsigned temp, nvml_->temperature_c(i));
  int pct = curve.evaluate(temp);
  // The hot-fan floor is re-applied here so no write can break it.
  if (temp > HardLimits::kFanHotThresholdC) pct = std::max(pct, HardLimits::kMinFanPctWhenHot);
  pct = std::clamp(pct, static_cast<int>(d.facts.fan_min_pct), static_cast<int>(d.facts.fan_max_pct));
  for (unsigned f = 0; f < d.facts.fan_count; ++f) {
    if (auto r = nvml_->set_fan_speed_pct(i, f, static_cast<unsigned>(pct)); !r) {
      // Could not apply the curve: return every fan to driver control.
      for (unsigned g = 0; g < d.facts.fan_count; ++g) (void)nvml_->set_fan_default(i, g);
      d.fan = FanCurve::driver_auto();
      return r.error();
    }
  }
  d.fan = curve;
  return success();
}

Result<void> NvidiaBackend::apply(const GpuId& id, const Settings& s) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(Device * d, find(id));
  const auto p = pstates(*d);
  const bool can_set = nvapi_ && nvapi_->can_set_pstates20() && d->nvapi;
  auto need_nvapi = [&](const char* what) -> Result<void> {
    if (!can_set || !p) return make_error(ErrorCode::Unsupported, std::format("{} needs NVAPI P-state 2.0 support", what));
    return success();
  };
  if (s.core_offset_mhz) {
    GPUOC_TRY(need_nvapi("core offset"));
    GPUOC_TRY(nvapi_->set_clock_delta_khz(*d->nvapi, false, *s.core_offset_mhz * 1000));
  }
  if (s.mem_offset_mhz) {
    GPUOC_TRY(need_nvapi("memory offset"));
    GPUOC_TRY(nvapi_->set_clock_delta_khz(*d->nvapi, true, *s.mem_offset_mhz * 1000));
  }
  if (s.voltage_mv) {
    GPUOC_TRY(need_nvapi("voltage"));
    if (!p->has_core_voltage) return make_error(ErrorCode::Unsupported, "no core voltage domain");
    const int nominal_uv = p->base_voltage_uv - p->voltage_uv.value;
    GPUOC_TRY(nvapi_->set_core_voltage_delta_uv(*d->nvapi, *s.voltage_mv * 1000 - nominal_uv));
  }
  if (s.power_limit_w) {
    if (!d->facts.power_ok) return make_error(ErrorCode::Unsupported, "power limit control unavailable");
    GPUOC_TRY(nvml_->set_power_limit_mw(d->info.index, static_cast<unsigned>(*s.power_limit_w) * 1000u));
  }
  if (s.fan_curve) {
    if (!d->facts.fans_ok) return make_error(ErrorCode::Unsupported, "fan control unavailable");
    GPUOC_TRY(set_fans(*d, *s.fan_curve));
  }
  return success();
}

Result<void> NvidiaBackend::reset_to_stock(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(Device * d, find(id));
  // Try every part even if one fails, then report the first failure.
  std::optional<Error> first;
  auto keep = [&](Result<void> r) {
    if (!r && !first) first = r.error();
  };
  const auto p = pstates(*d);
  if (nvapi_ && nvapi_->can_set_pstates20() && d->nvapi && p) {
    if (p->graphics_khz.present && p->graphics_khz.editable) keep(nvapi_->set_clock_delta_khz(*d->nvapi, false, 0));
    if (p->memory_khz.present && p->memory_khz.editable) keep(nvapi_->set_clock_delta_khz(*d->nvapi, true, 0));
    if (p->has_core_voltage && p->voltage_uv.editable) keep(nvapi_->set_core_voltage_delta_uv(*d->nvapi, 0));
  }
  if (d->facts.power_ok) keep(nvml_->set_power_limit_mw(d->info.index, d->facts.power_default_mw));
  if (d->facts.fans_ok) keep(set_fans(*d, FanCurve::driver_auto()));
  if (first) return *first;
  return success();
}

}  // namespace nvidia

Result<std::shared_ptr<IGpuBackend>> make_nvidia_backend() {
  GPUOC_TRY_ASSIGN(auto nvml, nvidia::load_nvml());
  std::unique_ptr<nvidia::INvapiApi> nvapi;
  if (auto n = nvidia::load_nvapi()) {
    nvapi = std::move(n).value();
  } else {
    log::warn("NVAPI unavailable ({}); clock offsets and voltage are unsupported", n.error().message);
  }
  auto b = std::make_shared<nvidia::NvidiaBackend>(std::move(nvml), std::move(nvapi));
  GPUOC_TRY(b->init());
  return std::shared_ptr<IGpuBackend>(b);
}

nlohmann::json nvidia_diagnostics() {
  using nlohmann::json;
  json out;
  auto nvml = nvidia::load_nvml();
  if (!nvml) {
    out["nvml"] = json{{"loaded", false}, {"error", nvml.error().message}};
  } else {
    auto& api = *nvml.value();
    json j{{"loaded", true}, {"path", api.library_path()}, {"file_version", api.library_version()}};
    if (auto r = api.init(); !r) {
      j["init_error"] = r.error().message;
    } else {
      if (auto v = api.driver_version()) j["driver_version"] = v.value();
      if (auto n = api.device_count()) j["device_count"] = n.value();
    }
    json fns = json::object();
    for (const char* f : {"nvmlDeviceSetPowerManagementLimit", "nvmlDeviceSetFanSpeed_v2", "nvmlDeviceSetDefaultFanSpeed_v2",
                          "nvmlDeviceGetTotalEccErrors", "nvmlDeviceGetCurrentClocksEventReasons", "nvmlDeviceSetGpuLockedClocks"})
      fns[f] = api.has(f);
    j["functions"] = fns;
    out["nvml"] = j;
  }
  auto nvapi = nvidia::load_nvapi();
  if (!nvapi) {
    out["nvapi"] = json{{"loaded", false}, {"error", nvapi.error().message}};
  } else {
    json fns = json::object();
    for (const auto& [k, v] : nvapi.value()->functions()) fns[k] = v;
    out["nvapi"] = json{{"loaded", true},
                        {"path", nvapi.value()->library_path()},
                        {"file_version", nvapi.value()->library_version()},
                        {"functions", fns}};
  }
  return out;
}

}  // namespace gpuoc
