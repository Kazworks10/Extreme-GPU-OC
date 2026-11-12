// NVIDIA backend without hardware: status-code parsing, capability checks,
// telemetry conversion, write paths, and NVML <-> NVAPI mapping, all against
// fake NVML/NVAPI function tables.
#include <gtest/gtest.h>

#include <map>
#include <nvml.h>

#include "backends/nvidia/nvidia_backend.hpp"
#include "backends/nvidia/nvidia_backend_impl.hpp"
#include "test_util.hpp"

namespace gpuoc::nvidia {
namespace {

// ---- Fakes -----------------------------------------------------------------------------

struct FakeDevice {
  NvmlDeviceInfo info;
  unsigned temp = 55, power_mw = 210000, util = 97, gfx = 2700, mem = 10500;
  std::vector<unsigned> fans = {40, 42};
  std::pair<unsigned, unsigned> fan_range{30, 100};
  bool fans_supported = true;
  std::optional<std::uint64_t> ecc;  // nullopt: unsupported
  unsigned pl_min = 100000, pl_max = 400000, pl_default = 320000, pl = 320000;
  bool power_supported = true;
  std::uint64_t reasons = 0;
  int temp_status = NVML_SUCCESS;
  std::vector<int> fan_writes;  // -1 = default
};

class FakeNvml final : public INvmlApi {
 public:
  std::vector<FakeDevice> devs;
  std::set<std::string> missing;
  Result<void> init() override { return success(); }
  void shutdown() override {}
  Result<std::string> driver_version() override { return std::string("566.36"); }
  Result<unsigned> device_count() override { return static_cast<unsigned>(devs.size()); }
  Result<NvmlDeviceInfo> device_info(unsigned i) override { return devs.at(i).info; }
  Result<unsigned> clock_mhz(unsigned i, NvmlClock c) override { return c == NvmlClock::Graphics ? devs.at(i).gfx : devs.at(i).mem; }
  Result<unsigned> temperature_c(unsigned i) override {
    if (devs.at(i).temp_status != NVML_SUCCESS) return nvml_error(devs.at(i).temp_status, "nvmlDeviceGetTemperature");
    return devs.at(i).temp;
  }
  Result<unsigned> power_mw(unsigned i) override { return devs.at(i).power_mw; }
  Result<unsigned> utilization_pct(unsigned i) override { return devs.at(i).util; }
  Result<unsigned> fan_count(unsigned i) override {
    if (!devs.at(i).fans_supported) return nvml_error(NVML_ERROR_NOT_SUPPORTED, "nvmlDeviceGetNumFans");
    return static_cast<unsigned>(devs.at(i).fans.size());
  }
  Result<unsigned> fan_speed_pct(unsigned i, unsigned f) override { return devs.at(i).fans.at(f); }
  Result<std::pair<unsigned, unsigned>> fan_min_max_pct(unsigned i) override {
    if (!devs.at(i).fans_supported) return nvml_error(NVML_ERROR_NOT_SUPPORTED, "nvmlDeviceGetMinMaxFanSpeed");
    return devs.at(i).fan_range;
  }
  Result<void> set_fan_speed_pct(unsigned i, unsigned f, unsigned pct) override {
    devs.at(i).fans.at(f) = pct;
    devs.at(i).fan_writes.push_back(static_cast<int>(pct));
    return success();
  }
  Result<void> set_fan_default(unsigned i, unsigned) override {
    devs.at(i).fan_writes.push_back(-1);
    return success();
  }
  Result<std::uint64_t> ecc_uncorrected(unsigned i) override {
    if (!devs.at(i).ecc) return nvml_error(NVML_ERROR_NOT_SUPPORTED, "nvmlDeviceGetTotalEccErrors");
    return *devs.at(i).ecc;
  }
  Result<std::pair<unsigned, unsigned>> power_limit_constraints_mw(unsigned i) override {
    if (!devs.at(i).power_supported) return nvml_error(NVML_ERROR_NOT_SUPPORTED, "constraints");
    return std::make_pair(devs.at(i).pl_min, devs.at(i).pl_max);
  }
  Result<unsigned> power_limit_default_mw(unsigned i) override { return devs.at(i).pl_default; }
  Result<unsigned> power_limit_mw(unsigned i) override { return devs.at(i).pl; }
  Result<void> set_power_limit_mw(unsigned i, unsigned mw) override {
    if (mw < devs.at(i).pl_min || mw > devs.at(i).pl_max) return nvml_error(NVML_ERROR_INVALID_ARGUMENT, "set");
    devs.at(i).pl = mw;
    return success();
  }
  Result<std::uint64_t> clock_event_reasons(unsigned i) override { return devs.at(i).reasons; }
  [[nodiscard]] bool has(const char* f) const override { return !missing.count(f); }
};

class FakeNvapi final : public INvapiApi {
 public:
  std::vector<NvapiGpu> gpus;
  std::map<int, Pstates20> ps;
  bool can_set = true;
  std::vector<std::string> writes;
  Result<std::vector<NvapiGpu>> enumerate() override { return gpus; }
  Result<Pstates20> get_pstates20(int h) override { return ps.at(h); }
  [[nodiscard]] bool can_set_pstates20() const override { return can_set; }
  Result<void> set_clock_delta_khz(int h, bool memory, int khz) override {
    writes.push_back(std::format("{}:{}:{}", h, memory ? "mem" : "core", khz));
    (memory ? ps.at(h).memory_khz : ps.at(h).graphics_khz).value = khz;
    return success();
  }
  Result<void> set_core_voltage_delta_uv(int h, int uv) override {
    writes.push_back(std::format("{}:volt:{}", h, uv));
    auto& p = ps.at(h);
    p.base_voltage_uv += uv - p.voltage_uv.value;
    p.voltage_uv.value = uv;
    return success();
  }
  [[nodiscard]] std::map<std::string, bool> functions() const override { return {{"NvAPI_GPU_SetPstates20", can_set}}; }
};

Pstates20 editable_pstates() {
  Pstates20 p;
  p.editable = p.p0_editable = true;
  p.graphics_khz = PstateDelta{true, true, 0, -1000000, 1000000};
  p.memory_khz = PstateDelta{true, true, 0, -1000000, 3000000};
  p.has_core_voltage = true;
  p.base_voltage_uv = 1050000;
  p.voltage_uv = PstateDelta{true, true, 0, 0, 100000};
  return p;
}

struct NvRig {
  FakeNvml* nvml;
  FakeNvapi* nvapi;
  std::shared_ptr<NvidiaBackend> backend;
  explicit NvRig(int gpus = 1, bool with_nvapi = true) {
    auto n = std::make_unique<FakeNvml>();
    auto a = std::make_unique<FakeNvapi>();
    for (int i = 0; i < gpus; ++i) {
      FakeDevice d;
      d.info = NvmlDeviceInfo{static_cast<unsigned>(i), std::format("GPU-{:08x}-1111-2222-3333-444455556666", i + 1),
                              std::format("NVIDIA Test {}", i), 0, static_cast<unsigned>(10 + i), 0};
      n->devs.push_back(d);
      // NVAPI enumerates in reverse order: mapping must use the PCI bus.
      a->gpus.insert(a->gpus.begin(), NvapiGpu{gpus - 1 - i, static_cast<unsigned>(10 + i), d.info.name});
    }
    for (int i = 0; i < gpus; ++i) a->ps[i] = editable_pstates();
    nvml = n.get();
    nvapi = a.get();
    backend = std::make_shared<NvidiaBackend>(std::move(n), with_nvapi ? std::move(a) : nullptr);
    if (!with_nvapi) nvapi = nullptr;
    EXPECT_TRUE(backend->init().ok());
  }
  GpuId id(int i = 0) { return backend->enumerate().value().at(static_cast<std::size_t>(i)); }
};

// ---- Status parsing ----------------------------------------------------------------------

TEST(NvidiaStatus, NvmlCodes) {
  EXPECT_EQ(nvml_error(NVML_ERROR_NOT_SUPPORTED, "f").code, ErrorCode::Unsupported);
  EXPECT_EQ(nvml_error(NVML_ERROR_NO_PERMISSION, "f").code, ErrorCode::PermissionDenied);
  EXPECT_EQ(nvml_error(NVML_ERROR_GPU_IS_LOST, "f").code, ErrorCode::GpuLost);
  EXPECT_EQ(nvml_error(NVML_ERROR_INVALID_ARGUMENT, "f").code, ErrorCode::InvalidArgument);
  EXPECT_EQ(nvml_error(NVML_ERROR_DRIVER_NOT_LOADED, "f").code, ErrorCode::Unavailable);
  EXPECT_EQ(nvml_error(NVML_ERROR_FUNCTION_NOT_FOUND, "f").code, ErrorCode::Unsupported);
  EXPECT_EQ(nvml_error(NVML_ERROR_UNKNOWN, "f").code, ErrorCode::HardwareError);
  const Error e = nvml_error(NVML_ERROR_NO_PERMISSION, "nvmlDeviceSetPowerManagementLimit");
  EXPECT_NE(e.message.find("nvmlDeviceSetPowerManagementLimit"), std::string::npos);
  EXPECT_NE(e.message.find("status 4"), std::string::npos);
}

TEST(NvidiaStatus, NvapiCodes) {
  EXPECT_EQ(nvapi_error(-104, "f").code, ErrorCode::Unsupported);       // NVAPI_NOT_SUPPORTED
  EXPECT_EQ(nvapi_error(-3, "f").code, ErrorCode::Unsupported);         // NVAPI_NO_IMPLEMENTATION
  EXPECT_EQ(nvapi_error(-137, "f").code, ErrorCode::PermissionDenied);  // NVAPI_INVALID_USER_PRIVILEGE
  EXPECT_EQ(nvapi_error(-9, "f").code, ErrorCode::Unsupported);         // NVAPI_INCOMPATIBLE_STRUCT_VERSION
  EXPECT_EQ(nvapi_error(-10, "f").code, ErrorCode::GpuLost);            // NVAPI_HANDLE_INVALIDATED
  EXPECT_EQ(nvapi_error(-158, "f").code, ErrorCode::SafetyRefusal);     // NVAPI_SET_NOT_ALLOWED
  EXPECT_EQ(nvapi_error(-1, "f").code, ErrorCode::HardwareError);       // NVAPI_ERROR
}

TEST(NvidiaStatus, ClockEventReasonsToLimitFlags) {
  EXPECT_EQ(limit_flags_from_reasons(0), kLimitNone);
  EXPECT_EQ(limit_flags_from_reasons(nvmlClocksEventReasonGpuIdle), kLimitNone);
  EXPECT_EQ(limit_flags_from_reasons(nvmlClocksEventReasonSwPowerCap), kLimitPower);
  EXPECT_EQ(limit_flags_from_reasons(nvmlClocksEventReasonSwThermalSlowdown), kLimitThermal);
  EXPECT_EQ(limit_flags_from_reasons(nvmlClocksThrottleReasonHwThermalSlowdown), kLimitThermal);
  EXPECT_EQ(limit_flags_from_reasons(nvmlClocksThrottleReasonHwPowerBrakeSlowdown), kLimitHwSlowdown);
  EXPECT_EQ(limit_flags_from_reasons(nvmlClocksEventReasonSwPowerCap | nvmlClocksThrottleReasonHwSlowdown),
            kLimitPower | kLimitHwSlowdown);
}

// ---- Capability checks -------------------------------------------------------------------

NvmlFacts full_facts() {
  NvmlFacts f;
  f.power_ok = true;
  f.power_min_mw = 100500;
  f.power_max_mw = 450700;
  f.power_default_mw = 320000;
  f.fans_ok = true;
  f.fan_count = 2;
  f.fan_min_pct = 30;
  f.fan_max_pct = 100;
  f.driver_version = "566.36";
  return f;
}

TEST(NvidiaCapabilities, FullSupport) {
  auto c = derive_capabilities(full_facts(), editable_pstates(), true);
  EXPECT_TRUE(c.power_limit);
  EXPECT_EQ(c.power_limit_w, (Range{101, 450})) << "driver range rounded inward";
  EXPECT_EQ(c.stock_power_limit_w, 320);
  EXPECT_TRUE(c.fan_control);
  EXPECT_EQ(c.fan_pct, (Range{30, 100}));
  EXPECT_TRUE(c.core_offset);
  EXPECT_EQ(c.core_offset_mhz, (Range{-1000, 1000}));
  EXPECT_TRUE(c.mem_offset);
  EXPECT_EQ(c.mem_offset_mhz, (Range{-1000, 3000}));
  EXPECT_TRUE(c.voltage_control);
  EXPECT_EQ(c.voltage_mv, (Range{1050, 1150}));
  EXPECT_EQ(c.stock_voltage_mv, 1050);
  EXPECT_EQ(c.voltage_step_mv, 0) << "NVAPI reports no step";
  EXPECT_EQ(c.driver_version, "566.36");
}

TEST(NvidiaCapabilities, MissingSetterMeansNoOffsetsOrVoltage) {
  auto c = derive_capabilities(full_facts(), editable_pstates(), /*can_set=*/false);
  EXPECT_FALSE(c.core_offset);
  EXPECT_FALSE(c.mem_offset);
  EXPECT_FALSE(c.voltage_control);
  EXPECT_TRUE(c.power_limit) << "NVML capabilities are independent";
}

TEST(NvidiaCapabilities, NoNvapiOrNotEditable) {
  EXPECT_FALSE(derive_capabilities(full_facts(), std::nullopt, true).core_offset);
  auto p = editable_pstates();
  p.editable = false;
  EXPECT_FALSE(derive_capabilities(full_facts(), p, true).core_offset);
  p = editable_pstates();
  p.graphics_khz.editable = false;
  p.voltage_uv.editable = false;
  auto c = derive_capabilities(full_facts(), p, true);
  EXPECT_FALSE(c.core_offset);
  EXPECT_TRUE(c.mem_offset);
  EXPECT_FALSE(c.voltage_control) << "voltage reported unsupported, not guessed";
}

TEST(NvidiaCapabilities, UnsupportedNvmlFeatures) {
  auto f = full_facts();
  f.power_ok = false;
  f.fans_ok = false;
  auto c = derive_capabilities(f, std::nullopt, false);
  EXPECT_FALSE(c.power_limit);
  EXPECT_FALSE(c.fan_control);
  f = full_facts();
  f.power_default_mw = 0;  // stock TDP unknown
  EXPECT_FALSE(derive_capabilities(f, std::nullopt, false).power_limit);
}

TEST(NvidiaCapabilities, BackendGathersFacts) {
  NvRig r;
  r.nvml->missing.insert("nvmlDeviceSetFanSpeed_v2");
  NvRig fresh;  // all present
  auto caps = fresh.backend->capabilities(fresh.id()).value();
  EXPECT_TRUE(caps.fan_control);
  EXPECT_TRUE(caps.core_offset);
  EXPECT_FALSE(caps.ecc_supported);
  // Missing fan setter -> fan control unsupported (re-init to re-gather).
  FakeNvml* n = r.nvml;
  ASSERT_TRUE(r.backend->init().ok());
  (void)n;
  EXPECT_FALSE(r.backend->capabilities(r.id()).value().fan_control);
}

TEST(NvidiaCapabilities, NoNvapiLibrary) {
  NvRig r(1, /*with_nvapi=*/false);
  auto caps = r.backend->capabilities(r.id()).value();
  EXPECT_FALSE(caps.core_offset);
  EXPECT_FALSE(caps.voltage_control);
  EXPECT_TRUE(caps.power_limit);
  Settings s;
  s.core_offset_mhz = 30;
  EXPECT_EQ(r.backend->apply(r.id(), s).error().code, ErrorCode::Unsupported);
}

// ---- Telemetry and settings parsing ---------------------------------------------------------

TEST(NvidiaTelemetry, Conversion) {
  NvRig r;
  r.nvml->devs[0].reasons = nvmlClocksEventReasonSwPowerCap;
  r.nvml->devs[0].ecc = 3;
  ASSERT_TRUE(r.backend->init().ok());
  auto t = r.backend->read_telemetry(r.id()).value();
  EXPECT_DOUBLE_EQ(t.power_w, 210.0);
  EXPECT_DOUBLE_EQ(t.temp_c, 55);
  EXPECT_DOUBLE_EQ(t.fan_pct, 41);
  EXPECT_DOUBLE_EQ(t.core_mhz, 2700);
  EXPECT_EQ(t.limit_flags, kLimitPower);
  EXPECT_EQ(t.ecc_uncorrected, 3u);
  ASSERT_TRUE(t.voltage_mv.has_value());
  EXPECT_DOUBLE_EQ(*t.voltage_mv, 1050);
}

TEST(NvidiaTelemetry, EccUnsupportedIsZeroAndGpuLostPropagates) {
  NvRig r;
  EXPECT_EQ(r.backend->read_telemetry(r.id()).value().ecc_uncorrected, 0u);
  r.nvml->devs[0].temp_status = NVML_ERROR_GPU_IS_LOST;
  auto t = r.backend->read_telemetry(r.id());
  ASSERT_FALSE(t.ok());
  EXPECT_EQ(t.error().code, ErrorCode::GpuLost);
}

// ---- Write paths -------------------------------------------------------------------------------

TEST(NvidiaApply, UnitsAndMapping) {
  NvRig r(2);
  Settings s;
  s.core_offset_mhz = 105;
  s.mem_offset_mhz = 500;
  s.power_limit_w = 300;
  s.voltage_mv = 1075;
  ASSERT_TRUE(r.backend->apply(r.id(1), s).ok());
  // NVML device 1 (bus 11) is NVAPI handle 0 (reverse enumeration).
  EXPECT_EQ(r.nvapi->writes, (std::vector<std::string>{"0:core:105000", "0:mem:500000", "0:volt:25000"}));
  EXPECT_EQ(r.nvml->devs[1].pl, 300000u);
  auto back = r.backend->read_settings(r.id(1)).value();
  EXPECT_EQ(*back.core_offset_mhz, 105);
  EXPECT_EQ(*back.mem_offset_mhz, 500);
  EXPECT_EQ(*back.power_limit_w, 300);
  EXPECT_EQ(*back.voltage_mv, 1075);
  EXPECT_EQ(*r.backend->read_settings(r.id(0)).value().core_offset_mhz, 0) << "other GPU untouched";
}

TEST(NvidiaApply, DriverRejectionIsAnError) {
  NvRig r;
  Settings s;
  s.power_limit_w = 900;
  EXPECT_EQ(r.backend->apply(r.id(), s).error().code, ErrorCode::InvalidArgument);
}

TEST(NvidiaApply, SoftwareFanCurveAndFloor) {
  NvRig r;
  Settings s;
  s.fan_curve = FanCurve{{{40, 20}, {50, 25}, {90, 100}}};
  r.nvml->devs[0].temp = 62;  // above 60 C: floor applies even though the curve says ~34
  ASSERT_TRUE(r.backend->apply(r.id(), s).ok());
  ASSERT_EQ(r.nvml->devs[0].fan_writes.size(), 2u);
  EXPECT_GE(r.nvml->devs[0].fan_writes[0], 30);
  EXPECT_EQ(*r.backend->read_settings(r.id()).value().fan_curve, *s.fan_curve);
  s.fan_curve = FanCurve::driver_auto();
  ASSERT_TRUE(r.backend->apply(r.id(), s).ok());
  EXPECT_EQ(r.nvml->devs[0].fan_writes.back(), -1) << "driver-automatic restored";
}

TEST(NvidiaApply, ResetToStock) {
  NvRig r;
  Settings s;
  s.core_offset_mhz = 60;
  s.power_limit_w = 250;
  s.fan_curve = FanCurve{{{40, 40}, {80, 100}}};
  ASSERT_TRUE(r.backend->apply(r.id(), s).ok());
  ASSERT_TRUE(r.backend->reset_to_stock(r.id()).ok());
  auto back = r.backend->read_settings(r.id()).value();
  EXPECT_EQ(*back.core_offset_mhz, 0);
  EXPECT_EQ(*back.mem_offset_mhz, 0);
  EXPECT_EQ(*back.power_limit_w, 320);
  EXPECT_EQ(*back.voltage_mv, 1050);
  EXPECT_TRUE(back.fan_curve->automatic());
}

TEST(NvidiaApply, ThroughTheGpuFacadeWithSafety) {
  NvRig r;
  auto policy = std::make_shared<SafetyPolicy>(SafetyConfig{});
  Gpu gpu(r.backend, r.id(), policy);
  Settings s;
  s.voltage_mv = 1060;
  EXPECT_EQ(gpu.apply(s).error().code, ErrorCode::SafetyRefusal);
  EXPECT_TRUE(r.nvapi->writes.empty());
  Settings ok;
  ok.core_offset_mhz = 45;
  ASSERT_TRUE(gpu.apply(ok).ok());
  ASSERT_TRUE(gpu.reset_to_stock().ok());
}

// ---- Library loading ---------------------------------------------------------------------------

TEST(NvidiaLoading, MissingLibrariesReportUnavailable) {
  auto b = make_nvidia_backend();
  if (b.ok()) GTEST_SKIP() << "an NVIDIA driver is installed here";
  EXPECT_EQ(b.error().code, ErrorCode::Unavailable);
  auto diag = nvidia_diagnostics();
  EXPECT_FALSE(diag["nvml"]["loaded"].get<bool>());
}

}  // namespace
}  // namespace gpuoc::nvidia
