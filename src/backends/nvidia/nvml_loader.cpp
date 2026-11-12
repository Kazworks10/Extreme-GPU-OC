// Runtime-loaded NVML. Function pointer types come from the official nvml.h;
// nvml.lib is never linked.
#include <cstring>
#include <format>
#include <nvml.h>
#include <vector>

#include "backends/nvidia/nvml_api.hpp"
#include "platform/dynlib.hpp"
#include "platform/fs.hpp"

namespace gpuoc::nvidia {
namespace {

#define GPUOC_NVML_REQUIRED(X)                  \
  X(nvmlInit_v2)                                \
  X(nvmlShutdown)                               \
  X(nvmlSystemGetDriverVersion)                 \
  X(nvmlDeviceGetCount_v2)                      \
  X(nvmlDeviceGetHandleByIndex_v2)              \
  X(nvmlDeviceGetUUID)                          \
  X(nvmlDeviceGetName)                          \
  X(nvmlDeviceGetPciInfo_v3)                    \
  X(nvmlDeviceGetClockInfo)                     \
  X(nvmlDeviceGetTemperature)                   \
  X(nvmlDeviceGetPowerUsage)                    \
  X(nvmlDeviceGetUtilizationRates)

#define GPUOC_NVML_OPTIONAL(X)                  \
  X(nvmlDeviceGetNumFans)                       \
  X(nvmlDeviceGetFanSpeed_v2)                   \
  X(nvmlDeviceGetMinMaxFanSpeed)                \
  X(nvmlDeviceSetFanSpeed_v2)                   \
  X(nvmlDeviceSetDefaultFanSpeed_v2)            \
  X(nvmlDeviceGetTotalEccErrors)                \
  X(nvmlDeviceGetPowerManagementLimitConstraints) \
  X(nvmlDeviceGetPowerManagementDefaultLimit)   \
  X(nvmlDeviceGetPowerManagementLimit)          \
  X(nvmlDeviceSetPowerManagementLimit)          \
  X(nvmlDeviceGetCurrentClocksEventReasons)     \
  X(nvmlDeviceSetGpuLockedClocks)

class RealNvml final : public INvmlApi {
 public:
  explicit RealNvml(std::shared_ptr<platform::DynLib> lib) : lib_(std::move(lib)) {
#define X(name) name##_ = lib_->get<decltype(&::name)>(#name);
    GPUOC_NVML_REQUIRED(X)
    GPUOC_NVML_OPTIONAL(X)
#undef X
    // Drivers before R535 export the older name with the same signature.
    if (!nvmlDeviceGetCurrentClocksEventReasons_)
      nvmlDeviceGetCurrentClocksEventReasons_ =
          lib_->get<decltype(&::nvmlDeviceGetCurrentClocksEventReasons)>("nvmlDeviceGetCurrentClocksThrottleReasons");
  }

  [[nodiscard]] std::string missing_required() const {
    std::string m;
#define X(name) \
  if (!name##_) m += std::string(m.empty() ? "" : ", ") + #name;
    GPUOC_NVML_REQUIRED(X)
#undef X
    return m;
  }

  [[nodiscard]] bool has(const char* function) const override {
#define X(name) \
  if (std::strcmp(function, #name) == 0) return name##_ != nullptr;
    GPUOC_NVML_REQUIRED(X)
    GPUOC_NVML_OPTIONAL(X)
#undef X
    return lib_->symbol(function) != nullptr;
  }
  [[nodiscard]] std::string library_path() const override { return lib_->path(); }
  [[nodiscard]] std::string library_version() const override { return lib_->file_version(); }

  Result<void> init() override {
    if (const nvmlReturn_t r = nvmlInit_v2_(); r != NVML_SUCCESS) return nvml_error(r, "nvmlInit_v2");
    initialized_ = true;
    GPUOC_TRY_ASSIGN(unsigned n, device_count());
    handles_.assign(n, nullptr);
    for (unsigned i = 0; i < n; ++i) {
      if (const nvmlReturn_t r = nvmlDeviceGetHandleByIndex_v2_(i, &handles_[i]); r != NVML_SUCCESS)
        return nvml_error(r, "nvmlDeviceGetHandleByIndex_v2");
    }
    return success();
  }
  void shutdown() override {
    if (initialized_) nvmlShutdown_();
    initialized_ = false;
  }
  ~RealNvml() override { shutdown(); }

  Result<std::string> driver_version() override {
    char buf[NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE] = {};
    if (const nvmlReturn_t r = nvmlSystemGetDriverVersion_(buf, sizeof buf); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlSystemGetDriverVersion");
    return std::string(buf);
  }
  Result<unsigned> device_count() override {
    unsigned n = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetCount_v2_(&n); r != NVML_SUCCESS) return nvml_error(r, "nvmlDeviceGetCount_v2");
    return n;
  }
  Result<NvmlDeviceInfo> device_info(unsigned i) override {
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    NvmlDeviceInfo info;
    info.index = i;
    char uuid[NVML_DEVICE_UUID_V2_BUFFER_SIZE] = {};
    if (const nvmlReturn_t r = nvmlDeviceGetUUID_(d, uuid, sizeof uuid); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetUUID");
    info.uuid = uuid;
    char name[NVML_DEVICE_NAME_V2_BUFFER_SIZE] = {};
    if (const nvmlReturn_t r = nvmlDeviceGetName_(d, name, sizeof name); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetName");
    info.name = name;
    nvmlPciInfo_t pci{};
    if (const nvmlReturn_t r = nvmlDeviceGetPciInfo_v3_(d, &pci); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetPciInfo_v3");
    info.pci_domain = pci.domain;
    info.pci_bus = pci.bus;
    info.pci_device = pci.device;
    return info;
  }
  Result<unsigned> clock_mhz(unsigned i, NvmlClock c) override {
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetClockInfo_(d, c == NvmlClock::Graphics ? NVML_CLOCK_GRAPHICS : NVML_CLOCK_MEM, &v);
        r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetClockInfo");
    return v;
  }
  Result<unsigned> temperature_c(unsigned i) override {
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetTemperature_(d, NVML_TEMPERATURE_GPU, &v); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetTemperature");
    return v;
  }
  Result<unsigned> power_mw(unsigned i) override {
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetPowerUsage_(d, &v); r != NVML_SUCCESS) return nvml_error(r, "nvmlDeviceGetPowerUsage");
    return v;
  }
  Result<unsigned> utilization_pct(unsigned i) override {
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    nvmlUtilization_t u{};
    if (const nvmlReturn_t r = nvmlDeviceGetUtilizationRates_(d, &u); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetUtilizationRates");
    return u.gpu;
  }
  Result<unsigned> fan_count(unsigned i) override {
    if (!nvmlDeviceGetNumFans_) return unsupported("nvmlDeviceGetNumFans");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned n = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetNumFans_(d, &n); r != NVML_SUCCESS) return nvml_error(r, "nvmlDeviceGetNumFans");
    return n;
  }
  Result<unsigned> fan_speed_pct(unsigned i, unsigned fan) override {
    if (!nvmlDeviceGetFanSpeed_v2_) return unsupported("nvmlDeviceGetFanSpeed_v2");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetFanSpeed_v2_(d, fan, &v); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetFanSpeed_v2");
    return v;
  }
  Result<std::pair<unsigned, unsigned>> fan_min_max_pct(unsigned i) override {
    if (!nvmlDeviceGetMinMaxFanSpeed_) return unsupported("nvmlDeviceGetMinMaxFanSpeed");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned lo = 0, hi = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetMinMaxFanSpeed_(d, &lo, &hi); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetMinMaxFanSpeed");
    return std::make_pair(lo, hi);
  }
  Result<void> set_fan_speed_pct(unsigned i, unsigned fan, unsigned pct) override {
    if (!nvmlDeviceSetFanSpeed_v2_) return unsupported("nvmlDeviceSetFanSpeed_v2");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    if (const nvmlReturn_t r = nvmlDeviceSetFanSpeed_v2_(d, fan, pct); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceSetFanSpeed_v2");
    return success();
  }
  Result<void> set_fan_default(unsigned i, unsigned fan) override {
    if (!nvmlDeviceSetDefaultFanSpeed_v2_) return unsupported("nvmlDeviceSetDefaultFanSpeed_v2");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    if (const nvmlReturn_t r = nvmlDeviceSetDefaultFanSpeed_v2_(d, fan); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceSetDefaultFanSpeed_v2");
    return success();
  }
  Result<std::uint64_t> ecc_uncorrected(unsigned i) override {
    if (!nvmlDeviceGetTotalEccErrors_) return unsupported("nvmlDeviceGetTotalEccErrors");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned long long n = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetTotalEccErrors_(d, NVML_MEMORY_ERROR_TYPE_UNCORRECTED, NVML_VOLATILE_ECC, &n);
        r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetTotalEccErrors");
    return static_cast<std::uint64_t>(n);
  }
  Result<std::pair<unsigned, unsigned>> power_limit_constraints_mw(unsigned i) override {
    if (!nvmlDeviceGetPowerManagementLimitConstraints_) return unsupported("nvmlDeviceGetPowerManagementLimitConstraints");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned lo = 0, hi = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetPowerManagementLimitConstraints_(d, &lo, &hi); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetPowerManagementLimitConstraints");
    return std::make_pair(lo, hi);
  }
  Result<unsigned> power_limit_default_mw(unsigned i) override {
    if (!nvmlDeviceGetPowerManagementDefaultLimit_) return unsupported("nvmlDeviceGetPowerManagementDefaultLimit");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetPowerManagementDefaultLimit_(d, &v); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetPowerManagementDefaultLimit");
    return v;
  }
  Result<unsigned> power_limit_mw(unsigned i) override {
    if (!nvmlDeviceGetPowerManagementLimit_) return unsupported("nvmlDeviceGetPowerManagementLimit");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetPowerManagementLimit_(d, &v); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetPowerManagementLimit");
    return v;
  }
  Result<void> set_power_limit_mw(unsigned i, unsigned mw) override {
    if (!nvmlDeviceSetPowerManagementLimit_) return unsupported("nvmlDeviceSetPowerManagementLimit");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    if (const nvmlReturn_t r = nvmlDeviceSetPowerManagementLimit_(d, mw); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceSetPowerManagementLimit");
    return success();
  }
  Result<std::uint64_t> clock_event_reasons(unsigned i) override {
    if (!nvmlDeviceGetCurrentClocksEventReasons_) return unsupported("nvmlDeviceGetCurrentClocksEventReasons");
    GPUOC_TRY_ASSIGN(nvmlDevice_t d, dev(i));
    unsigned long long v = 0;
    if (const nvmlReturn_t r = nvmlDeviceGetCurrentClocksEventReasons_(d, &v); r != NVML_SUCCESS)
      return nvml_error(r, "nvmlDeviceGetCurrentClocksEventReasons");
    return static_cast<std::uint64_t>(v);
  }

 private:
  Result<nvmlDevice_t> dev(unsigned i) const {
    if (i >= handles_.size()) return make_error(ErrorCode::NotFound, std::format("no NVML device {}", i));
    return handles_[i];
  }
  static Error unsupported(const char* fn) {
    return make_error(ErrorCode::Unsupported, std::format("{} is not exported by this NVML", fn));
  }

  std::shared_ptr<platform::DynLib> lib_;
  bool initialized_ = false;
  std::vector<nvmlDevice_t> handles_;
#define X(name) decltype(&::name) name##_ = nullptr;
  GPUOC_NVML_REQUIRED(X)
  GPUOC_NVML_OPTIONAL(X)
#undef X
};

}  // namespace

Result<std::unique_ptr<INvmlApi>> load_nvml() {
  std::vector<std::string> candidates;
#ifdef _WIN32
  candidates.push_back("nvml.dll");  // System32 (current drivers)
  const std::string pf = platform::getenv_utf8("ProgramW6432");
  candidates.push_back((pf.empty() ? std::string("C:\\Program Files") : pf) + "\\NVIDIA Corporation\\NVSMI\\nvml.dll");
#else
  candidates = {"libnvidia-ml.so.1", "libnvidia-ml.so"};
#endif
  GPUOC_TRY_ASSIGN(auto lib, platform::DynLib::open(candidates));
  auto api = std::make_unique<RealNvml>(lib);
  if (const std::string missing = api->missing_required(); !missing.empty())
    return make_error(ErrorCode::Unavailable, "NVML is missing required functions: " + missing);
  return std::unique_ptr<INvmlApi>(std::move(api));
}

}  // namespace gpuoc::nvidia
