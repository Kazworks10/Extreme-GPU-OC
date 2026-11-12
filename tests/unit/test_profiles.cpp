// Profiles: parsing, validation, store precedence, resolution, auto-switching.
#include <gtest/gtest.h>

#include "controller/engine.hpp"
#include "core/json_codec.hpp"
#include "profile/auto_switch.hpp"
#include "profile/profile_store.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

constexpr const char* kGaming = R"(
name = "gaming"
description = "Evening gaming"
objective = "max_performance"
priority = 10

[target]
mode = "fixed"
core_offset_mhz = 90
mem_offset_mhz = 400
power_limit_pct = 90
temp_target_c = 72
fan_curve = [[40, 30], [60, 50], [80, 100]]

[tuning]
budget_s = 7200
margin_steps = 2

[[match]]
exe = "game*.exe"

[[match]]
path = "C:\\Games\\Other\\other.exe"

[gpu."GPU-00000000-0000-4000-8000-0000000f4243"]
core_offset_mhz = 60
)";

TEST(ProfileParse, FullProfile) {
  auto p = parse_profile(kGaming, "gaming.toml");
  ASSERT_TRUE(p.ok()) << p.error().message;
  const Profile& g = p.value();
  EXPECT_EQ(g.name, "gaming");
  EXPECT_EQ(g.objective, Objective::MaxPerformance);
  EXPECT_EQ(g.mode, ProfileMode::Fixed);
  EXPECT_EQ(g.priority, 10);
  EXPECT_EQ(*g.target.core_offset_mhz, 90);
  EXPECT_EQ(*g.target.power_limit_pct, 90);
  EXPECT_DOUBLE_EQ(*g.target.temp_target_c, 72);
  EXPECT_EQ(g.target.fan_curve->points.size(), 3u);
  EXPECT_EQ(g.tuning_budget, Sec(7200));
  EXPECT_EQ(*g.tuning_margin_steps, 2);
  ASSERT_EQ(g.match.size(), 2u);
  EXPECT_EQ(g.match[0].exe, "game*.exe");
  EXPECT_EQ(g.match[1].path, "C:\\Games\\Other\\other.exe");
  ASSERT_EQ(g.gpu_overrides.size(), 1u);
  EXPECT_EQ(*g.gpu_overrides.begin()->second.core_offset_mhz, 60);
}

TEST(ProfileParse, RoundTrip) {
  auto p = parse_profile(kGaming, "gaming.toml").value();
  auto again = parse_profile(serialize_profile(p), "again.toml");
  ASSERT_TRUE(again.ok()) << again.error().message << "\n" << serialize_profile(p);
  EXPECT_EQ(again.value().target, p.target);
  EXPECT_EQ(again.value().match, p.match);
  EXPECT_EQ(again.value().gpu_overrides, p.gpu_overrides);
  EXPECT_EQ(again.value().objective, p.objective);
  EXPECT_EQ(again.value().priority, p.priority);
}

void expect_rejected(const std::string& toml, const std::string& needle) {
  auto p = parse_profile(toml, "bad.toml");
  ASSERT_FALSE(p.ok()) << "accepted:\n" << toml;
  EXPECT_NE(p.error().message.find(needle), std::string::npos) << p.error().message;
  EXPECT_NE(p.error().message.find("bad.toml"), std::string::npos) << "error names the file: " << p.error().message;
}

TEST(ProfileParse, InvalidTomlIsRejectedClearly) {
  expect_rejected("name = \"x\"\n[target\n", "bad.toml:2");
  expect_rejected("name = \"x\"\nobjective = 5\n", "objective");
}

TEST(ProfileParse, OutOfRangeValuesAreRejected) {
  expect_rejected("name = \"x\"\n[target]\ncore_offset_mhz = 99999\n", "core_offset_mhz");
  expect_rejected("name = \"x\"\n[target]\ntemp_target_c = 95\n", "temp_target_c");
  expect_rejected("name = \"x\"\n[target]\npower_limit_pct = 400\n", "power_limit_pct");
  expect_rejected("name = \"x\"\n[target]\nfan_curve = [[60, 50], [40, 30]]\n", "fan_curve");
  expect_rejected("name = \"x\"\n[target]\nfan_curve = [[40, 130], [60, 30]]\n", "fan_curve");
  expect_rejected("name = \"x\"\n[tuning]\nbudget_s = 5\n", "budget_s");
}

TEST(ProfileParse, StructuralErrors) {
  expect_rejected("name = \"bad name!\"\n", "name");
  expect_rejected("name = \"x\"\nbogus = 1\n", "bogus");
  expect_rejected("name = \"x\"\n[target]\nmode = \"turbo\"\n", "mode");
  expect_rejected("name = \"x\"\n[target]\nmode = \"auto\"\ncore_offset_mhz = 10\n", "cannot set");
  expect_rejected("name = \"x\"\n[[match]]\nexe = \"a.exe\"\npath = \"C:\\\\a.exe\"\n", "exactly one");
  expect_rejected("name = \"x\"\n[target]\npower_limit_w = 200\npower_limit_pct = 80\n", "not both");
  expect_rejected("name = \"x\"\n[gpu.\"nope\"]\ncore_offset_mhz = 1\n", "GPU UUID");
}

TEST(ProfileBuiltins, PresentAndValidAgainstLimits) {
  auto rig = make_rig(1, SafetyConfig{});  // default (strict) safety config
  auto caps = rig.gpu->capabilities().value();
  LkgStore none(std::filesystem::path("unused-lkg.json"), true);
  std::set<std::string> names;
  for (const auto& p : builtin_profiles()) {
    names.insert(p.name);
    EXPECT_TRUE(p.builtin);
    auto r = resolve_profile(p, rig.id, caps, *rig.policy, &none);
    EXPECT_TRUE(r.ok()) << p.name << ": " << r.error().message;
  }
  EXPECT_EQ(names, (std::set<std::string>{"stock", "quiet", "balanced", "max"}));
}

TEST(ProfileResolve, FixedAutoAndOverrides) {
  auto rig = make_rig(1, permissive_safety());
  auto caps = rig.gpu->capabilities().value();
  test::TempDir dir;
  LkgStore lkg(dir.path() / "lkg.json");
  auto g = parse_profile(kGaming, "gaming.toml").value();
  auto r = resolve_profile(g, rig.id, caps, *rig.policy, &lkg);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(*r.value().settings.core_offset_mhz, 60) << "per-GPU override for this UUID";
  EXPECT_EQ(*r.value().settings.power_limit_w, 288) << "90% of 320 W";
  GpuId other = rig.id;
  other.uuid = "GPU-ffffffff-0000-0000-0000-000000000000";
  EXPECT_EQ(*resolve_profile(g, other, caps, *rig.policy, &lkg).value().settings.core_offset_mhz, 90);

  Profile max = builtin_profiles()[3];
  auto none = resolve_profile(max, rig.id, caps, *rig.policy, &lkg);
  ASSERT_TRUE(none.ok());
  EXPECT_TRUE(none.value().needs_tuning);
  LkgEntry e;
  e.gpu_uuid = rig.id.uuid;
  e.objective = Objective::MaxPerformance;
  e.validated = true;
  e.settings.core_offset_mhz = 105;
  ASSERT_TRUE(lkg.commit(e).ok());
  auto with = resolve_profile(max, rig.id, caps, *rig.policy, &lkg);
  ASSERT_TRUE(with.ok());
  EXPECT_TRUE(with.value().validated);
  EXPECT_EQ(*with.value().settings.core_offset_mhz, 105);
}

TEST(ProfileResolve, LimitsAndGatesApplyToProfiles) {
  auto rig = make_rig(1, SafetyConfig{});  // no voltage, no power above stock
  auto caps = rig.gpu->capabilities().value();
  auto volt = parse_profile("name = \"v\"\n[target]\nvoltage_mv = 1050\n", "v.toml").value();
  auto r = resolve_profile(volt, rig.id, caps, *rig.policy, nullptr);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::SafetyRefusal);
  auto power = parse_profile("name = \"p\"\n[target]\npower_limit_pct = 120\n", "p.toml").value();
  EXPECT_EQ(resolve_profile(power, rig.id, caps, *rig.policy, nullptr).error().code, ErrorCode::SafetyRefusal);
  auto core = parse_profile("name = \"c\"\n[target]\ncore_offset_mhz = 1500\n", "c.toml").value();
  EXPECT_EQ(resolve_profile(core, rig.id, caps, *rig.policy, nullptr).error().code, ErrorCode::OutOfRange);
}

TEST(ProfileStore, PrecedenceErrorsAndBuiltinProtection) {
  test::TempDir dir;
  const auto sys = dir.path() / "system";
  const auto usr = dir.path() / "user";
  ASSERT_TRUE(platform::write_file_atomic(sys / "gaming.toml", "name = \"gaming\"\n[target]\ncore_offset_mhz = 10\n").ok());
  ASSERT_TRUE(platform::write_file_atomic(usr / "gaming.toml", "name = \"gaming\"\n[target]\ncore_offset_mhz = 20\n").ok());
  ASSERT_TRUE(platform::write_file_atomic(usr / "broken.toml", "name = \"broken\"\n[target\n").ok());
  ASSERT_TRUE(platform::write_file_atomic(usr / "stock.toml", "name = \"stock\"\n[target]\ncore_offset_mhz = 500\n").ok());
  ProfileStore store(sys, usr);
  ASSERT_TRUE(store.load().ok());
  EXPECT_EQ(*store.get("gaming")->target.core_offset_mhz, 20) << "user overrides system";
  EXPECT_FALSE(store.get("broken").has_value());
  EXPECT_EQ(store.load_errors().size(), 2u) << "broken.toml and the stock override";
  EXPECT_TRUE(store.get("stock")->builtin) << "stock cannot be overridden";
  EXPECT_FALSE(store.remove("stock").ok());
  EXPECT_FALSE(store.remove("quiet").ok());
  ASSERT_TRUE(store.remove("gaming").ok());
  EXPECT_EQ(*store.get("gaming")->target.core_offset_mhz, 10) << "system profile visible again";

  Profile created;
  created.name = "made";
  created.mode = ProfileMode::Fixed;
  created.target.mem_offset_mhz = 100;
  auto f = store.save(created, true);
  ASSERT_TRUE(f.ok()) << f.error().message;
  EXPECT_TRUE(store.get("made").has_value());
}

// ---- Auto-switching ---------------------------------------------------------------------

class FakeLister final : public IProcessLister {
 public:
  Result<std::vector<ProcessInfo>> list() override { return procs; }
  std::vector<ProcessInfo> procs;
};

TEST(Glob, Matching) {
  EXPECT_TRUE(glob_match("game*.exe", "GameClient.EXE"));
  EXPECT_TRUE(glob_match("*.exe", "a.exe"));
  EXPECT_TRUE(glob_match("a?c.exe", "abc.exe"));
  EXPECT_FALSE(glob_match("game*.exe", "notgame.exe"));
  EXPECT_FALSE(glob_match("a?c.exe", "ac.exe"));
  EXPECT_TRUE(same_path("C:\\Games\\X.exe", "c:/games/x.EXE"));
  EXPECT_FALSE(same_path("C:\\Games\\X.exe", "C:\\Games\\Y.exe"));
}

TEST(AutoSwitch, FirstMatchWinsAndExitReverts) {
  auto a = parse_profile("name = \"alpha\"\npriority = 20\n[[match]]\nexe = \"shared*.exe\"\n", "a").value();
  auto b = parse_profile("name = \"beta\"\npriority = 10\n[[match]]\nexe = \"shared*.exe\"\n", "b").value();
  auto c = parse_profile("name = \"gamma\"\n[[match]]\npath = \"C:\\\\x\\\\tool.exe\"\n", "c").value();
  std::vector<Profile> ordered = {b, a, c};  // store order: priority, then name
  auto lister = std::make_shared<FakeLister>();
  AutoSwitcher sw(lister);
  EXPECT_FALSE(sw.poll(ordered).has_value());
  lister->procs = {{100, "explorer.exe", "C:\\Windows\\explorer.exe"}, {200, "Shared_Game.exe", ""}};
  auto ch = sw.poll(ordered);
  ASSERT_TRUE(ch.has_value());
  EXPECT_EQ(*ch->profile, "beta") << "priority 10 beats 20";
  EXPECT_FALSE(sw.poll(ordered).has_value()) << "no change while it runs";
  lister->procs = {{300, "tool.exe", "c:/X/TOOL.exe"}};
  ch = sw.poll(ordered);
  ASSERT_TRUE(ch.has_value());
  EXPECT_EQ(*ch->profile, "gamma") << "full-path rule";
  lister->procs.clear();
  ch = sw.poll(ordered);
  ASSERT_TRUE(ch.has_value());
  EXPECT_FALSE(ch->profile.has_value()) << "process exited: back to the default";
}

TEST(AutoSwitch, MatchingProcessAppliesProfileAndExitRevertsIt) {
  test::TempDir dir;
  test::ScopedEnv pd(platform::kEnvProgramData, dir.str("pd"));
  test::ScopedEnv ad(platform::kEnvAppData, dir.str("ad"));
  auto rig = make_rig(51, permissive_safety());
  auto lister = std::make_shared<FakeLister>();
  EngineOptions o;
  o.config.safety = permissive_safety();
  o.paths = platform::resolve_paths();
  o.clock = rig.clock;
  o.backend_override = rig.backend;
  o.processes_override = lister;
  ASSERT_TRUE(platform::write_file_atomic(o.paths.user_profiles_dir / "cool.toml",
                                          "name = \"cool\"\n[target]\ncore_offset_mhz = -50\npower_limit_pct = 75\n"
                                          "[[match]]\nexe = \"render*.exe\"\n")
                  .ok());
  ASSERT_TRUE(platform::write_file_atomic(o.paths.user_profiles_dir / "hot.toml",
                                          "name = \"hot\"\npriority = 200\n[target]\ncore_offset_mhz = 100\n"
                                          "[[match]]\nexe = \"bench*.exe\"\n")
                  .ok());
  auto e = Engine::create(o);
  ASSERT_TRUE(e.ok()) << e.error().message;
  ASSERT_TRUE(e.value()->start_controllers().ok());
  Gpu* g = e.value()->gpus().at(0);
  auto* ctl = e.value()->controller_for(g);
  ctl->step();  // Discover -> MonitorOnly (autotune off)

  lister->procs = {{42, "Render_Job.exe", ""}};
  e.value()->poll_auto_switch();
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, -50);
  EXPECT_EQ(*rig.backend->raw_settings(0).power_limit_w, 240);
  EXPECT_EQ(ctl->status()["applied"]["source"], "profile:cool");

  lister->procs.clear();
  e.value()->poll_auto_switch();
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0) << "exit reverts to the default (automatic -> stock)";
  EXPECT_EQ(*rig.backend->raw_settings(0).power_limit_w, 320);

  // Unvalidated overclocks are never applied automatically.
  lister->procs = {{43, "bench.exe", ""}};
  e.value()->poll_auto_switch();
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0);
  e.value()->shutdown("test");
}

}  // namespace
}  // namespace gpuoc
