// Real-hardware tests. Skipped unless GPUOC_HW_TESTS=1.
//
// They never write to the GPU: reads go to the NVIDIA driver; every write
// goes through the dry-run backend, so nothing is applied.
#include <gtest/gtest.h>

#include "backends/dry_run_backend.hpp"
#include "backends/nvidia/nvidia_backend.hpp"
#include "bench/vulkan_kernel.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

bool hw_enabled() { return platform::getenv_utf8("GPUOC_HW_TESTS") == "1"; }

#define GPUOC_REQUIRE_HW() \
  if (!hw_enabled()) GTEST_SKIP() << "set GPUOC_HW_TESTS=1 to run hardware tests"

TEST(Hardware, EnumerateAndReadCapabilities) {
  GPUOC_REQUIRE_HW();
  auto b = make_nvidia_backend();
  ASSERT_TRUE(b.ok()) << b.error().message;
  auto ids = b.value()->enumerate();
  ASSERT_TRUE(ids.ok());
  ASSERT_FALSE(ids.value().empty());
  for (const auto& id : ids.value()) {
    EXPECT_TRUE(id.uuid.starts_with("GPU-")) << id.uuid;
    auto c = b.value()->capabilities(id);
    ASSERT_TRUE(c.ok()) << c.error().message;
    EXPECT_FALSE(c.value().driver_version.empty());
    RecordProperty("capabilities_" + std::to_string(id.index), c.value().core_offset ? "offsets" : "no-offsets");
  }
}

TEST(Hardware, TelemetryIsPlausible) {
  GPUOC_REQUIRE_HW();
  auto b = make_nvidia_backend();
  ASSERT_TRUE(b.ok()) << b.error().message;
  for (const auto& id : b.value()->enumerate().value()) {
    auto t = b.value()->read_telemetry(id);
    ASSERT_TRUE(t.ok()) << t.error().message;
    EXPECT_GT(t.value().temp_c, 5);
    EXPECT_LT(t.value().temp_c, 110);
    EXPECT_GE(t.value().power_w, 0);
  }
}

TEST(Hardware, DryRunWritesNothing) {
  GPUOC_REQUIRE_HW();
  auto real = make_nvidia_backend();
  ASSERT_TRUE(real.ok()) << real.error().message;
  auto dry = std::make_shared<DryRunBackend>(real.value());
  auto id = dry->enumerate().value().at(0);
  const Settings before = real.value()->read_settings(id).value();
  Gpu gpu(dry, id, std::make_shared<SafetyPolicy>(SafetyConfig{}));
  Settings s;
  if (gpu.capabilities().value().core_offset) s.core_offset_mhz = 15;
  if (gpu.capabilities().value().power_limit) s.power_limit_w = gpu.capabilities().value().stock_power_limit_w - 10;
  if (!s.empty()) {
    ASSERT_TRUE(gpu.apply(s).ok());
    EXPECT_EQ(real.value()->read_settings(id).value(), before) << "dry-run must not touch the GPU";
  }
}

TEST(Hardware, VulkanUuidMatchesNvml) {
  GPUOC_REQUIRE_HW();
  auto b = make_nvidia_backend();
  ASSERT_TRUE(b.ok());
  auto devs = VulkanKernel::list_devices();
  ASSERT_TRUE(devs.ok()) << devs.error().message;
  for (const auto& id : b.value()->enumerate().value()) {
    bool found = false;
    for (const auto& d : devs.value()) found |= d.uuid == id.uuid;
    EXPECT_TRUE(found) << "no Vulkan device with NVML UUID " << id.uuid;
  }
}

}  // namespace
}  // namespace gpuoc
