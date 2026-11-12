// Narrow, fake-able view of NVAPI's P-state 2.0 interface. The real
// implementation (Windows only) loads nvapi64.dll at runtime and resolves
// every function through nvapi_QueryInterface; a function that does not
// resolve makes its capability unsupported.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gpuoc/result.hpp"

namespace gpuoc::nvidia {

// A delta parameter from NV_GPU_PERF_PSTATES20_PARAM_DELTA (kHz or uV).
struct PstateDelta {
  bool present = false;
  bool editable = false;
  int value = 0;
  int min = 0;
  int max = 0;
};

// The P0 view of NV_GPU_PERF_PSTATES20_INFO.
struct Pstates20 {
  bool editable = false;      // global bIsEditable
  bool p0_editable = false;   // P0 bIsEditable
  PstateDelta graphics_khz;   // NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS
  PstateDelta memory_khz;     // NVAPI_GPU_PUBLIC_CLOCK_MEMORY
  bool has_core_voltage = false;
  int base_voltage_uv = 0;    // current base voltage (nominal + delta)
  PstateDelta voltage_uv;     // core voltage delta
};

struct NvapiGpu {
  int handle = 0;  // index into the enumerated physical GPU handles
  unsigned pci_bus = 0;
  std::string name;
};

class INvapiApi {
 public:
  virtual ~INvapiApi() = default;
  virtual Result<std::vector<NvapiGpu>> enumerate() = 0;
  virtual Result<Pstates20> get_pstates20(int handle) = 0;
  [[nodiscard]] virtual bool can_set_pstates20() const = 0;
  virtual Result<void> set_clock_delta_khz(int handle, bool memory, int delta_khz) = 0;
  virtual Result<void> set_core_voltage_delta_uv(int handle, int delta_uv) = 0;
  // Function name -> available (doctor).
  [[nodiscard]] virtual std::map<std::string, bool> functions() const = 0;
  [[nodiscard]] virtual std::string library_path() const { return {}; }
  [[nodiscard]] virtual std::string library_version() const { return {}; }
};

// NvAPI_Status -> Error (pure; tested). Values from nvapi_lite_common.h.
Error nvapi_error(int status, const char* function);

// Interface ID of NvAPI_GPU_SetPstates20. It is not in the public NVAPI SDK
// headers; see docs/DECISIONS.md (D-020).
inline constexpr unsigned kNvapiSetPstates20Id = 0x0F4DAE6Bu;

// Windows: load nvapi64.dll. Other platforms: Unavailable.
Result<std::unique_ptr<INvapiApi>> load_nvapi();

}  // namespace gpuoc::nvidia
