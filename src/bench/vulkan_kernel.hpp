// Runs the deterministic stress kernel with Vulkan compute. Needs only the
// installed GPU driver (vulkan-1.dll is loaded at runtime); no CUDA toolkit.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bench/kernel_reference.hpp"
#include "gpuoc/result.hpp"

namespace gpuoc {

struct VulkanDeviceInfo {
  std::string name;
  std::string uuid;  // "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (matches NVML)
  std::uint32_t vendor_id = 0;
  std::uint32_t index = 0;
};

struct DeviceSelector {
  std::string uuid;           // preferred
  std::optional<int> index;   // fallback
  bool prefer_nvidia = true;  // when neither is given
};

struct KernelRunResult {
  enum class Status { Ok, Timeout, DeviceLost, Error } status = Status::Ok;
  std::uint64_t checksum = 0;
  double seconds = 0;
  std::string detail;
};

class VulkanKernel {
 public:
  static Result<std::vector<VulkanDeviceInfo>> list_devices();
  static Result<std::unique_ptr<VulkanKernel>> create(const DeviceSelector& sel, const KernelParams& params);
  ~VulkanKernel();
  VulkanKernel(const VulkanKernel&) = delete;
  VulkanKernel& operator=(const VulkanKernel&) = delete;

  // One full submission (all passes). Blocks up to `timeout`.
  KernelRunResult run_once(std::chrono::milliseconds timeout);
  [[nodiscard]] const VulkanDeviceInfo& device() const noexcept { return info_; }
  [[nodiscard]] const KernelParams& params() const noexcept { return params_; }

  struct Impl;

 private:
  explicit VulkanKernel(std::unique_ptr<Impl> impl, VulkanDeviceInfo info, KernelParams params);
  std::unique_ptr<Impl> impl_;
  VulkanDeviceInfo info_;
  KernelParams params_;
};

std::string format_uuid(const std::array<std::uint8_t, 16>& bytes);

}  // namespace gpuoc
