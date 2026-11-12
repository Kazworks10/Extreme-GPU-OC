// The shipped examples must stay valid.
#include <gtest/gtest.h>

#include "core/config.hpp"
#include "profile/profile.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

const std::filesystem::path kExamples = std::filesystem::path(GPUOC_SOURCE_DIR) / "examples";

TEST(Examples, ConfigParsesWithSpecDefaults) {
  auto c = load_config(kExamples / "gpuoc.toml");
  ASSERT_TRUE(c.ok()) << c.error().message;
  EXPECT_TRUE(c.value().warnings.empty());
  EXPECT_FALSE(c.value().autotune.enabled);
  EXPECT_FALSE(c.value().safety.acknowledge_risk);
  EXPECT_EQ(c.value().tuning.core_step_mhz, 15);
  EXPECT_EQ(c.value().tuning.mem_step_mhz, 50);
  EXPECT_DOUBLE_EQ(c.value().safety.max_temp_c, 83);
}

TEST(Examples, ProfilesParse) {
  int n = 0;
  for (const auto& e : std::filesystem::directory_iterator(kExamples / "profiles")) {
    auto text = platform::read_file(e.path());
    ASSERT_TRUE(text.ok());
    auto p = parse_profile(text.value(), platform::path_utf8(e.path()));
    EXPECT_TRUE(p.ok()) << p.error().message;
    ++n;
  }
  EXPECT_GE(n, 3);
}

}  // namespace
}  // namespace gpuoc

namespace gpuoc {
namespace {

// Section 9 "never implement": no TDR registry edits, no registry writes at
// all, no VBIOS/firmware flashing. Enforced by scanning the product sources.
TEST(NeverImplement, ForbiddenOperationsAbsentFromSource) {
  const std::vector<std::string> forbidden = {"TdrDelay", "TdrLevel", "TdrDdiDelay", "TdrLimit", "RegSetValue",
                                              "RegSetKeyValue", "RegCreateKey", "RegDeleteValue", "nvflash",
                                              "VbiosWrite", "NvAPI_GPU_SetVbios", "WriteFuse"};
  const std::filesystem::path root(GPUOC_SOURCE_DIR);
  int scanned = 0;
  for (const char* dir : {"src", "include"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root / dir)) {
      if (!e.is_regular_file()) continue;
      const auto ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".hpp" && ext != ".inc" && ext != ".comp") continue;
      const std::string text = platform::read_file(e.path()).value_or("");
      ++scanned;
      for (const auto& f : forbidden)
        EXPECT_EQ(text.find(f), std::string::npos) << e.path().string() << " mentions " << f;
    }
  }
  EXPECT_GT(scanned, 50);
}

}  // namespace
}  // namespace gpuoc
