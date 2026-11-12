// Config parsing, LKG store, paths.
#include <gtest/gtest.h>

#include "core/config.hpp"
#include "core/json_codec.hpp"
#include "core/lkg_store.hpp"
#include "platform/paths.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

TEST(Config, DefaultsMatchSpec) {
  auto c = parse_config("", "test").value();
  EXPECT_FALSE(c.autotune.enabled);
  EXPECT_FALSE(c.safety.acknowledge_risk);
  EXPECT_EQ(c.autotune.objective, Objective::MaxPerfPerWatt);
  EXPECT_EQ(c.autotune.budget, Sec(4 * 3600));
  EXPECT_EQ(c.autotune.revalidate_days, 14);
  EXPECT_EQ(c.tuning.core_step_mhz, 15);
  EXPECT_EQ(c.tuning.mem_step_mhz, 50);
  EXPECT_EQ(c.tuning.short_stress, Sec(120));
  EXPECT_EQ(c.tuning.long_stress, Sec(1800));
  EXPECT_EQ(c.benchmark.warmup, Sec(30));
  EXPECT_EQ(c.benchmark.runs, 3);
  EXPECT_DOUBLE_EQ(c.benchmark.max_cv_pct, 3.0);
  EXPECT_DOUBLE_EQ(c.safety.max_temp_c, 83.0);
  EXPECT_DOUBLE_EQ(c.safety.target_temp_c, 75.0);
  EXPECT_EQ(c.monitor.poll, Ms(1000));
  EXPECT_EQ(c.monitor.cooldown, Sec(1800));
  EXPECT_EQ(c.profiles.poll, Ms(2000));
}

TEST(Config, ParsesValues) {
  auto r = parse_config(R"(
[autotune]
enabled = true
objective = "max_performance"
budget_s = 3600
[safety]
acknowledge_risk = true
allow_voltage = true
max_temp_c = 80
max_power_w = 300
[tuning]
core_step_mhz = 10
[monitor]
poll_interval_ms = 500
)",
                        "test");
  ASSERT_TRUE(r.ok()) << r.error().message;
  const auto& c = r.value();
  EXPECT_TRUE(c.autotune.enabled);
  EXPECT_EQ(c.autotune.objective, Objective::MaxPerformance);
  EXPECT_TRUE(c.safety.allow_voltage);
  EXPECT_EQ(*c.safety.max_power_w, 300);
  EXPECT_EQ(c.tuning.core_step_mhz, 10);
  EXPECT_EQ(c.monitor.poll, Ms(500));
}

TEST(Config, RejectsUnknownKeysAndBadTypes) {
  auto typo = parse_config("[safety]\nacknowlege_risk = true\n", "test");
  ASSERT_FALSE(typo.ok());
  EXPECT_NE(typo.error().message.find("acknowlege_risk"), std::string::npos);
  EXPECT_FALSE(parse_config("[safety]\nallow_voltage = \"yes\"\n", "test").ok());
  EXPECT_FALSE(parse_config("[tuning]\ncore_step_mhz = 0\n", "test").ok());
  EXPECT_FALSE(parse_config("[autotune]\nobjective = \"fastest\"\n", "test").ok());
  EXPECT_FALSE(parse_config("[bogus]\n", "test").ok());
  auto syntax = parse_config("[safety\n", "gpuoc.toml");
  ASSERT_FALSE(syntax.ok());
  EXPECT_EQ(syntax.error().code, ErrorCode::ParseError);
  EXPECT_NE(syntax.error().message.find("gpuoc.toml:1"), std::string::npos);
}

TEST(Config, HardCeilingStillAppliesToParsedConfig) {
  auto c = parse_config("[safety]\nmax_temp_c = 99\n", "test").value();
  SafetyPolicy p(c.safety);
  EXPECT_DOUBLE_EQ(p.config().max_temp_c, 90.0);
}

TEST(Config, MissingFileGivesDefaultsWithWarning) {
  test::TempDir dir;
  auto c = load_config(dir.path() / "nope.toml");
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.value().warnings.size(), 1u);
  EXPECT_FALSE(c.value().autotune.enabled);
}

LkgEntry sample_entry(bool validated) {
  LkgEntry e;
  e.gpu_uuid = "GPU-abc";
  e.gpu_name = "Test";
  e.driver_version = "566.36";
  e.objective = Objective::Balanced;
  e.settings.core_offset_mhz = 105;
  e.settings.mem_offset_mhz = 600;
  e.validated = validated;
  e.validated_at = from_epoch_ms(1'800'000'000'000LL);
  e.score = 1234.5;
  return e;
}

TEST(LkgStore, OnlyValidatedSettingsEnter) {
  test::TempDir dir;
  LkgStore store(dir.path() / "lkg.json");
  ASSERT_TRUE(store.load().ok());
  auto r = store.commit(sample_entry(false));
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::SafetyRefusal);
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "lkg.json"));
  ASSERT_TRUE(store.commit(sample_entry(true)).ok());
  LkgStore reload(dir.path() / "lkg.json");
  ASSERT_TRUE(reload.load().ok());
  auto e = reload.get("GPU-abc", Objective::Balanced);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(*e->settings.core_offset_mhz, 105);
  EXPECT_DOUBLE_EQ(e->score, 1234.5);
}

TEST(LkgStore, UnvalidatedEntriesInFileAreIgnored) {
  test::TempDir dir;
  const auto file = dir.path() / "lkg.json";
  ASSERT_TRUE(platform::write_file_atomic(file, R"({"version":1,"entries":[
    {"gpu_uuid":"GPU-x","objective":"balanced","settings":{"core_offset_mhz":500},"validated":false}]})")
                  .ok());
  LkgStore store(file);
  ASSERT_TRUE(store.load().ok());
  EXPECT_TRUE(store.all().empty());
}

TEST(LkgStore, RevokeHidesEntries) {
  test::TempDir dir;
  LkgStore store(dir.path() / "lkg.json");
  ASSERT_TRUE(store.commit(sample_entry(true)).ok());
  ASSERT_TRUE(store.revoke("GPU-abc", "repeated instability").ok());
  EXPECT_FALSE(store.get("GPU-abc", Objective::Balanced).has_value());
  EXPECT_FALSE(store.latest("GPU-abc").has_value());
  LkgStore reload(dir.path() / "lkg.json");
  ASSERT_TRUE(reload.load().ok());
  ASSERT_EQ(reload.all().size(), 1u);
  EXPECT_TRUE(reload.all()[0].revoked);
}

TEST(LkgStore, ReadOnlyModeWritesNothing) {
  test::TempDir dir;
  LkgStore store(dir.path() / "lkg.json", /*read_only=*/true);
  ASSERT_TRUE(store.commit(sample_entry(true)).ok());
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "lkg.json"));
}

TEST(Paths, EnvironmentOverrides) {
  test::TempDir dir;
  test::ScopedEnv pd(platform::kEnvProgramData, dir.str("pd"));
  test::ScopedEnv ad(platform::kEnvAppData, dir.str("ad"));
  auto p = platform::resolve_paths();
  EXPECT_EQ(p.system_dir, dir.path() / "pd" / "ExtremeGpuOc");
  EXPECT_EQ(p.config_file, dir.path() / "pd" / "ExtremeGpuOc" / "gpuoc.toml");
  EXPECT_EQ(p.lkg_file, dir.path() / "pd" / "ExtremeGpuOc" / "lkg.json");
  EXPECT_EQ(p.logs_dir, dir.path() / "pd" / "ExtremeGpuOc" / "logs");
  EXPECT_EQ(p.system_profiles_dir, dir.path() / "pd" / "ExtremeGpuOc" / "profiles.d");
  EXPECT_EQ(p.user_profiles_dir, dir.path() / "ad" / "ExtremeGpuOc" / "profiles.d");
  EXPECT_EQ(p.tuning_log("GPU-1a/2b"), p.logs_dir / "tune-GPU-1a_2b.jsonl");
}

TEST(Paths, WindowsEnvironmentVariablesWithoutOverride) {
  test::ScopedEnv pd(platform::kEnvProgramData, "");
  test::ScopedEnv ad(platform::kEnvAppData, "");
  auto p = platform::resolve_paths();
#ifdef _WIN32
  // The known folders and the environment variables name the same folders.
  const std::string program_data = platform::getenv_utf8("ProgramData");
  const std::string app_data = platform::getenv_utf8("APPDATA");
  if (!program_data.empty()) {
    EXPECT_TRUE(std::filesystem::equivalent(p.program_data_root, platform::u8path(program_data)));
  }
  if (!app_data.empty()) {
    EXPECT_TRUE(std::filesystem::equivalent(p.app_data_root, platform::u8path(app_data)));
  }
#else
  test::TempDir dir;
  test::ScopedEnv pd2("ProgramData", dir.str("ProgramData"));
  test::ScopedEnv ad2("APPDATA", dir.str("AppData"));
  p = platform::resolve_paths();
  EXPECT_EQ(p.system_dir, dir.path() / "ProgramData" / "ExtremeGpuOc");
  EXPECT_EQ(p.user_dir, dir.path() / "AppData" / "ExtremeGpuOc");
#endif
  EXPECT_EQ(p.system_dir.filename(), "ExtremeGpuOc");
}

}  // namespace
}  // namespace gpuoc
