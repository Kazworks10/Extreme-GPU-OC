// Runtime-loaded NVAPI (Windows only). nvapi64.dll exports a single entry
// point, nvapi_QueryInterface; every function is resolved through it by
// interface ID. IDs for public functions come from the official
// nvapi_interface.h. nvapi64.lib is never linked.
#include "backends/nvidia/nvapi_api.hpp"

// MSVC and clang-cl only: the NVAPI headers rely on the MSVC SAL headers.
// MinGW development cross-builds use the stub at the end of this file.
#if defined(_WIN32) && defined(_MSC_VER)

#include <windows.h>

#include <cstring>
#include <format>

// The official headers. NVAPI_INTERFACE declarations are only used for
// decltype; nothing is linked.
#include <nvapi.h>
#include <nvapi_interface.h>

#include "platform/dynlib.hpp"

namespace gpuoc::nvidia {
namespace {

using QueryInterfaceFn = void*(__cdecl*)(unsigned int);

unsigned public_id(const char* name) {
  for (const auto& e : nvapi_interface_table) {
    if (e.func && std::strcmp(e.func, name) == 0) return e.id;
  }
  return 0;
}

class RealNvapi final : public INvapiApi {
 public:
  RealNvapi(std::shared_ptr<platform::DynLib> lib, QueryInterfaceFn qi) : lib_(std::move(lib)), qi_(qi) {
    initialize_ = resolve<decltype(&NvAPI_Initialize)>("NvAPI_Initialize");
    enum_gpus_ = resolve<decltype(&NvAPI_EnumPhysicalGPUs)>("NvAPI_EnumPhysicalGPUs");
    bus_id_ = resolve<decltype(&NvAPI_GPU_GetBusId)>("NvAPI_GPU_GetBusId");
    full_name_ = resolve<decltype(&NvAPI_GPU_GetFullName)>("NvAPI_GPU_GetFullName");
    get_pstates_ = resolve<decltype(&NvAPI_GPU_GetPstates20)>("NvAPI_GPU_GetPstates20");
    // Not in the public SDK: resolved by ID; null means unsupported.
    set_pstates_ = reinterpret_cast<SetPstatesFn>(qi_(kNvapiSetPstates20Id));
    fns_["NvAPI_GPU_SetPstates20"] = set_pstates_ != nullptr;
  }

  Result<void> init() {
    if (!initialize_) return make_error(ErrorCode::Unavailable, "NvAPI_Initialize unavailable");
    if (const NvAPI_Status s = initialize_(); s != NVAPI_OK) return nvapi_error(s, "NvAPI_Initialize");
    if (!enum_gpus_) return make_error(ErrorCode::Unavailable, "NvAPI_EnumPhysicalGPUs unavailable");
    NvU32 n = 0;
    if (const NvAPI_Status s = enum_gpus_(handles_, &n); s != NVAPI_OK) return nvapi_error(s, "NvAPI_EnumPhysicalGPUs");
    count_ = static_cast<int>(n);
    return success();
  }

  Result<std::vector<NvapiGpu>> enumerate() override {
    std::vector<NvapiGpu> out;
    for (int i = 0; i < count_; ++i) {
      NvapiGpu g;
      g.handle = i;
      if (bus_id_) {
        NvU32 bus = 0;
        if (bus_id_(handles_[i], &bus) == NVAPI_OK) g.pci_bus = bus;
      }
      if (full_name_) {
        NvAPI_ShortString name = {};
        if (full_name_(handles_[i], name) == NVAPI_OK) g.name = name;
      }
      out.push_back(g);
    }
    return out;
  }

  Result<Pstates20> get_pstates20(int handle) override {
    if (!get_pstates_) return make_error(ErrorCode::Unsupported, "NvAPI_GPU_GetPstates20 unavailable");
    GPUOC_TRY_ASSIGN(NvPhysicalGpuHandle h, gpu(handle));
    NV_GPU_PERF_PSTATES20_INFO info{};
    info.version = NV_GPU_PERF_PSTATES20_INFO_VER;
    if (const NvAPI_Status s = get_pstates_(h, &info); s != NVAPI_OK) return nvapi_error(s, "NvAPI_GPU_GetPstates20");
    Pstates20 p;
    p.editable = info.bIsEditable != 0;
    for (NvU32 i = 0; i < info.numPstates && i < NVAPI_MAX_GPU_PSTATE20_PSTATES; ++i) {
      const auto& ps = info.pstates[i];
      if (ps.pstateId != NVAPI_GPU_PERF_PSTATE_P0) continue;
      p.p0_editable = ps.bIsEditable != 0;
      for (NvU32 c = 0; c < info.numClocks && c < NVAPI_MAX_GPU_PSTATE20_CLOCKS; ++c) {
        const auto& clk = ps.clocks[c];
        PstateDelta* d = clk.domainId == NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS ? &p.graphics_khz
                         : clk.domainId == NVAPI_GPU_PUBLIC_CLOCK_MEMORY ? &p.memory_khz
                                                                          : nullptr;
        if (!d) continue;
        d->present = true;
        d->editable = clk.bIsEditable != 0;
        d->value = clk.freqDelta_kHz.value;
        d->min = clk.freqDelta_kHz.valueRange.min;
        d->max = clk.freqDelta_kHz.valueRange.max;
      }
      for (NvU32 v = 0; v < info.numBaseVoltages && v < NVAPI_MAX_GPU_PSTATE20_BASE_VOLTAGES; ++v) {
        const auto& bv = ps.baseVoltages[v];
        if (bv.domainId != NVAPI_GPU_PERF_VOLTAGE_INFO_DOMAIN_CORE) continue;
        p.has_core_voltage = true;
        p.base_voltage_uv = static_cast<int>(bv.volt_uV);
        p.voltage_uv.present = true;
        p.voltage_uv.editable = bv.bIsEditable != 0;
        p.voltage_uv.value = bv.voltDelta_uV.value;
        p.voltage_uv.min = bv.voltDelta_uV.valueRange.min;
        p.voltage_uv.max = bv.voltDelta_uV.valueRange.max;
      }
    }
    return p;
  }

  [[nodiscard]] bool can_set_pstates20() const override { return set_pstates_ != nullptr; }

  Result<void> set_clock_delta_khz(int handle, bool memory, int delta_khz) override {
    if (!set_pstates_) return make_error(ErrorCode::Unsupported, "NvAPI_GPU_SetPstates20 unavailable");
    GPUOC_TRY_ASSIGN(NvPhysicalGpuHandle h, gpu(handle));
    NV_GPU_PERF_PSTATES20_INFO_V1 s{};
    s.version = NV_GPU_PERF_PSTATES20_INFO_VER1;
    s.numPstates = 1;
    s.numClocks = 1;
    s.numBaseVoltages = 0;
    s.pstates[0].pstateId = NVAPI_GPU_PERF_PSTATE_P0;
    s.pstates[0].clocks[0].domainId = memory ? NVAPI_GPU_PUBLIC_CLOCK_MEMORY : NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS;
    s.pstates[0].clocks[0].freqDelta_kHz.value = delta_khz;
    if (const NvAPI_Status st = set_pstates_(h, &s); st != NVAPI_OK) return nvapi_error(st, "NvAPI_GPU_SetPstates20");
    return success();
  }

  Result<void> set_core_voltage_delta_uv(int handle, int delta_uv) override {
    if (!set_pstates_) return make_error(ErrorCode::Unsupported, "NvAPI_GPU_SetPstates20 unavailable");
    GPUOC_TRY_ASSIGN(NvPhysicalGpuHandle h, gpu(handle));
    NV_GPU_PERF_PSTATES20_INFO_V1 s{};
    s.version = NV_GPU_PERF_PSTATES20_INFO_VER1;
    s.numPstates = 1;
    s.numClocks = 0;
    s.numBaseVoltages = 1;
    s.pstates[0].pstateId = NVAPI_GPU_PERF_PSTATE_P0;
    s.pstates[0].baseVoltages[0].domainId = NVAPI_GPU_PERF_VOLTAGE_INFO_DOMAIN_CORE;
    s.pstates[0].baseVoltages[0].voltDelta_uV.value = delta_uv;
    if (const NvAPI_Status st = set_pstates_(h, &s); st != NVAPI_OK) return nvapi_error(st, "NvAPI_GPU_SetPstates20");
    return success();
  }

  [[nodiscard]] std::map<std::string, bool> functions() const override { return fns_; }
  [[nodiscard]] std::string library_path() const override { return lib_->path(); }
  [[nodiscard]] std::string library_version() const override { return lib_->file_version(); }

 private:
  using SetPstatesFn = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, NV_GPU_PERF_PSTATES20_INFO_V1*);

  template <class Fn>
  Fn resolve(const char* name) {
    const unsigned id = public_id(name);
    void* p = id ? qi_(id) : nullptr;
    fns_[name] = p != nullptr;
    return reinterpret_cast<Fn>(p);
  }
  Result<NvPhysicalGpuHandle> gpu(int handle) const {
    if (handle < 0 || handle >= count_) return make_error(ErrorCode::NotFound, std::format("no NVAPI GPU {}", handle));
    return handles_[handle];
  }

  std::shared_ptr<platform::DynLib> lib_;
  QueryInterfaceFn qi_;
  std::map<std::string, bool> fns_;
  NvPhysicalGpuHandle handles_[NVAPI_MAX_PHYSICAL_GPUS] = {};
  int count_ = 0;
  decltype(&NvAPI_Initialize) initialize_ = nullptr;
  decltype(&NvAPI_EnumPhysicalGPUs) enum_gpus_ = nullptr;
  decltype(&NvAPI_GPU_GetBusId) bus_id_ = nullptr;
  decltype(&NvAPI_GPU_GetFullName) full_name_ = nullptr;
  decltype(&NvAPI_GPU_GetPstates20) get_pstates_ = nullptr;
  SetPstatesFn set_pstates_ = nullptr;
};

}  // namespace

Result<std::unique_ptr<INvapiApi>> load_nvapi() {
  GPUOC_TRY_ASSIGN(auto lib, platform::DynLib::open({"nvapi64.dll"}));
  auto qi = lib->get<QueryInterfaceFn>("nvapi_QueryInterface");
  if (!qi) return make_error(ErrorCode::Unavailable, "nvapi_QueryInterface not exported by nvapi64.dll");
  auto api = std::make_unique<RealNvapi>(lib, qi);
  GPUOC_TRY(api->init());
  return std::unique_ptr<INvapiApi>(std::move(api));
}

}  // namespace gpuoc::nvidia

#else  // NVAPI exists only on Windows (and is built with MSVC / clang-cl).

namespace gpuoc::nvidia {
Result<std::unique_ptr<INvapiApi>> load_nvapi() {
#ifdef _WIN32
  return make_error(ErrorCode::Unavailable, "this build has no NVAPI support (built without MSVC/clang-cl)");
#else
  return make_error(ErrorCode::Unavailable, "NVAPI is only available on Windows");
#endif
}
}  // namespace gpuoc::nvidia

#endif
