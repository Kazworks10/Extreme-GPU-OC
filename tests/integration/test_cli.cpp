// gpuoc.exe end to end against the mock backend: exit codes, JSON output,
// limit enforcement, profiles.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <thread>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

struct CliResult {
  int code = -1;
  std::string out;
};

class Cli : public ::testing::Test {
 protected:
  void SetUp() override {
    pd_ = std::make_unique<test::ScopedEnv>(platform::kEnvProgramData, dir_.str("pd"));
    ad_ = std::make_unique<test::ScopedEnv>(platform::kEnvAppData, dir_.str("ad"));
  }
  CliResult run(std::vector<std::string> args) {
    args.insert(args.begin(), GPUOC_CLI_PATH);
    auto child = platform::ChildProcess::spawn(args);
    EXPECT_TRUE(child.ok()) << child.error().message;
    CliResult r;
    if (!child) return r;
    for (;;) {
      auto line = child.value()->read_line(Ms(60000));
      if (!line || !line.value()) break;
      r.out += *line.value() + "\n";
    }
    auto e = child.value()->wait(Ms(60000));
    r.code = e ? e->code : -1;
    return r;
  }
  test::TempDir dir_;
  std::unique_ptr<test::ScopedEnv> pd_, ad_;
};

TEST_F(Cli, ListJson) {
  auto r = run({"--backend", "mock", "--json", "list"});
  ASSERT_EQ(r.code, 0) << r.out;
  auto j = nlohmann::json::parse(r.out);
  ASSERT_EQ(j.size(), 1u);
  EXPECT_EQ(j[0]["index"], 0);
}

TEST_F(Cli, UsageErrorsExitOne) {
  EXPECT_EQ(run({"frobnicate"}).code, 1);
  EXPECT_EQ(run({"--backend", "mock", "set", "--core", "+30"}).code, 1) << "set needs --gpu";
  EXPECT_EQ(run({"--backend", "mock", "set", "--gpu", "0", "--core", "lots"}).code, 1);
  EXPECT_EQ(run({"--no-such-flag", "list"}).code, 1);
  EXPECT_EQ(run({"--backend", "bogus", "list"}).code, 1);
}

TEST_F(Cli, LimitsAndGatesExitThree) {
  EXPECT_EQ(run({"--backend", "mock", "set", "--gpu", "0", "--core", "+5000", "--for", "0"}).code, 3);
  EXPECT_EQ(run({"--backend", "mock", "set", "--gpu", "0", "--voltage", "1010", "--for", "0"}).code, 3);
  EXPECT_EQ(run({"--backend", "mock", "set", "--gpu", "0", "--power", "390", "--for", "0"}).code, 3)
      << "above stock TDP without allow_power_above_stock";
  EXPECT_EQ(run({"--backend", "mock", "tune"}).code, 3) << "tuning needs acknowledge_risk";
}

TEST_F(Cli, SetWithinLimitsSucceeds) {
  auto r = run({"--backend", "mock", "--json", "set", "--gpu", "0", "--core", "+45", "--mem", "+100", "--for", "0"});
  ASSERT_EQ(r.code, 0) << r.out;
  auto j = nlohmann::json::parse(r.out);
  EXPECT_EQ(j[0]["applied"]["core_offset_mhz"], 45);
}

TEST_F(Cli, StressReportsStable) {
  auto r = run({"--backend", "mock", "--json", "stress", "--duration", "2"});
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_EQ(nlohmann::json::parse(r.out)[0]["verdict"], "stable");
}

TEST_F(Cli, ProfileLifecycle) {
  auto c = run({"--backend", "mock", "profile", "create", "night", "--power-pct", "70", "--match-exe", "render*.exe"});
  ASSERT_EQ(c.code, 0) << c.out;
  auto l = run({"--json", "profile", "list"});
  ASSERT_EQ(l.code, 0);
  bool found = false;
  const auto listed = nlohmann::json::parse(l.out);
  for (const auto& p : listed["profiles"]) found |= p["name"] == "night";
  EXPECT_TRUE(found);
  auto s = run({"profile", "show", "night"});
  EXPECT_NE(s.out.find("power_limit_pct = 70"), std::string::npos) << s.out;
  EXPECT_EQ(run({"profile", "edit", "night", "--set", "power_limit_pct=500"}).code, 1) << "out of range rejected";
  EXPECT_EQ(run({"profile", "edit", "night", "--set", "power_limit_pct=85"}).code, 0);
  EXPECT_NE(run({"profile", "show", "night"}).out.find("power_limit_pct = 85"), std::string::npos);
  EXPECT_EQ(run({"profile", "delete", "stock"}).code, 1);
  EXPECT_EQ(run({"profile", "delete", "night"}).code, 0);
  EXPECT_EQ(run({"profile", "show", "night"}).code, 1);
}

TEST_F(Cli, InvalidProfileFileIsReported) {
  ASSERT_TRUE(platform::write_file_atomic(dir_.path() / "ad" / "ExtremeGpuOc" / "profiles.d" / "bad.toml",
                                          "name = \"bad\"\n[target]\ncore_offset_mhz = 99999\n")
                  .ok());
  auto l = run({"--json", "profile", "list"});
  ASSERT_EQ(l.code, 0);
  auto errors = nlohmann::json::parse(l.out)["errors"];
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].get<std::string>().find("core_offset_mhz"), std::string::npos);
}

// Ctrl+C while `gpuoc set` holds settings: stock is restored before exit.
TEST_F(Cli, CtrlCRestoresStock) {
  const std::string hw = dir_.str("hw.json");
  test::ScopedEnv state("GPUOC_MOCK_STATE", hw);
  auto child = platform::ChildProcess::spawn({GPUOC_CLI_PATH, "--backend", "mock", "set", "--gpu", "0", "--core", "+45"});
  ASSERT_TRUE(child.ok());
  auto core_now = [&]() -> int {
    auto text = platform::read_file(platform::u8path(hw));
    if (!text) return -1;
    auto j = nlohmann::json::parse(text.value(), nullptr, false);
    return j.is_discarded() ? -1 : j["gpus"][0].value("core", -1);
  };
  for (int i = 0; i < 200 && core_now() != 45; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  ASSERT_EQ(core_now(), 45) << "settings applied and held";
  auto sent = child.value()->interrupt();
  if (!sent) GTEST_SKIP() << sent.error().message;  // Windows: no shared console in tests
  auto e = child.value()->wait(Ms(10000));
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->code, 0);
  EXPECT_EQ(core_now(), 0) << "Ctrl+C restored stock";
}

TEST_F(Cli, DoctorProducesJson) {
  auto r = run({"--json", "doctor"});
  EXPECT_TRUE(r.code == 0 || r.code == 2) << r.out;
  auto j = nlohmann::json::parse(r.out);
  EXPECT_GE(j["checks"].size(), 5u);
}

}  // namespace
}  // namespace gpuoc
