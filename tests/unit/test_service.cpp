// Service control, watchdog, pipe, and JSON-RPC.
#include <gtest/gtest.h>

#include <thread>

#include "controller/watchdog.hpp"
#include "core/heartbeat.hpp"
#include "platform/pipe.hpp"
#include "platform/service.hpp"
#include "rpc/jsonrpc.hpp"
#include "test_util.hpp"

namespace gpuoc {
namespace {

using test::make_rig;
using test::permissive_safety;

// Records every status report together with the GPU's core offset at that
// moment, so the test can prove stock was restored before "stopped".
class RecordingReporter final : public platform::IStatusReporter {
 public:
  explicit RecordingReporter(mock::MockBackend& hw) : hw_(hw) {}
  void report(platform::ServiceStatus s, int, Ms) override {
    std::lock_guard lk(mu_);
    events_.push_back({s, *hw_.raw_settings(0).core_offset_mhz});
  }
  std::vector<std::pair<platform::ServiceStatus, int>> events() {
    std::lock_guard lk(mu_);
    return events_;
  }
  bool saw(platform::ServiceStatus s) {
    std::lock_guard lk(mu_);
    for (auto& e : events_) {
      if (e.first == s) return true;
    }
    return false;
  }

 private:
  mock::MockBackend& hw_;
  std::mutex mu_;
  std::vector<std::pair<platform::ServiceStatus, int>> events_;
};

void run_control_scenario(platform::ServiceControl control) {
  auto rig = make_rig(41, permissive_safety());
  RecordingReporter reporter(*rig.backend);
  std::unique_ptr<StockGuard> guard;
  platform::ServiceLifecycle::Hooks hooks;
  hooks.start = [&]() -> Result<void> {
    guard = std::make_unique<StockGuard>(*rig.gpu);
    Settings s;
    s.core_offset_mhz = 90;
    GPUOC_TRY(rig.gpu->apply(s));
    return success();
  };
  hooks.run = [](const std::atomic<bool>& stop) {
    while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  };
  hooks.stop = [&](const std::string&) {
    // Synchronous restore, like Engine::shutdown.
    ASSERT_TRUE(guard->restore().ok());
  };
  platform::ServiceLifecycle life(reporter, hooks);
  std::thread svc([&] { EXPECT_EQ(life.run(), 0); });
  for (int i = 0; i < 400 && !reporter.saw(platform::ServiceStatus::Running); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(reporter.saw(platform::ServiceStatus::Running));
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 90);
  life.control(control);
  svc.join();

  const auto ev = reporter.events();
  ASSERT_GE(ev.size(), 4u);
  EXPECT_EQ(ev.back().first, platform::ServiceStatus::Stopped);
  EXPECT_EQ(ev.back().second, 0) << "stock restored before reporting stopped";
  EXPECT_EQ(ev[ev.size() - 2].first, platform::ServiceStatus::StopPending);
  EXPECT_EQ(ev[ev.size() - 2].second, 90) << "stop pending reported first, while the restore was still to come";
}

TEST(ServiceControl, StopRestoresStockBeforeReportingStopped) { run_control_scenario(platform::ServiceControl::Stop); }

TEST(ServiceControl, PreshutdownRestoresStockBeforeReportingStopped) {
  run_control_scenario(platform::ServiceControl::Preshutdown);
}

TEST(ServiceControl, StartFailureStillRestoresAndReportsStopped) {
  auto rig = make_rig(42, permissive_safety());
  RecordingReporter reporter(*rig.backend);
  bool stopped_called = false;
  platform::ServiceLifecycle::Hooks hooks;
  hooks.start = [] { return Result<void>(make_error(ErrorCode::Unavailable, "no GPU")); };
  hooks.stop = [&](const std::string&) { stopped_called = true; };
  platform::ServiceLifecycle life(reporter, hooks);
  EXPECT_EQ(life.run(), 1);
  EXPECT_TRUE(stopped_called);
  EXPECT_EQ(reporter.events().back().first, platform::ServiceStatus::Stopped);
}

// ---- Watchdog: a stalled watchdog triggers revert and exit.

TEST(Watchdog, StallTriggersRevertThenExit) {
  std::atomic<int> reverts{0};
  std::atomic<int> exit_code{-1};
  std::atomic<bool> reverted_before_exit{false};
  Watchdog::Actions a;
  a.revert = [&](const char*) { reverts++; };
  a.exit = [&](int code) {
    reverted_before_exit = reverts.load() > 0;
    exit_code = code;
  };
  Watchdog wd(std::chrono::milliseconds(150), a);
  wd.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(600));  // no heartbeat
  EXPECT_TRUE(wd.fired());
  EXPECT_EQ(reverts.load(), 1);
  EXPECT_EQ(exit_code.load(), Watchdog::kExitCode);
  EXPECT_TRUE(reverted_before_exit.load());
}

TEST(Watchdog, FedWatchdogDoesNotFire) {
  std::atomic<int> reverts{0};
  Watchdog::Actions a;
  a.revert = [&](const char*) { reverts++; };
  a.exit = [](int) {};
  Watchdog wd(std::chrono::milliseconds(200), a);
  wd.start();
  for (int i = 0; i < 30; ++i) {
    heartbeat::beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  wd.stop();
  EXPECT_FALSE(wd.fired());
  EXPECT_EQ(reverts.load(), 0);
}

void stall_with_watchdog(const std::string& hw) {
  auto rig = make_rig(43, permissive_safety(), {}, hw);
  StockGuard guard(*rig.gpu);
  Settings s;
  s.core_offset_mhz = 120;
  (void)rig.gpu->apply(s);
  Watchdog wd(std::chrono::milliseconds(200));
  wd.start();
  std::this_thread::sleep_for(std::chrono::seconds(10));  // stalled: never feeds
  std::_Exit(0);
}

TEST(WatchdogDeathTest, RealStallRestoresStockAndExits) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  test::SharedDeathDir dir("GPUOC_TEST_WATCHDOG_DIR");
  const std::string hw = platform::path_utf8(dir.path() / "hw.json");
  EXPECT_EXIT(stall_with_watchdog(hw), ::testing::ExitedWithCode(Watchdog::kExitCode), "");
  ASSERT_TRUE(std::filesystem::exists(platform::u8path(hw))) << "the child wrote the mock hardware state";
  auto rig = make_rig(43, permissive_safety(), {}, hw);
  EXPECT_EQ(*rig.backend->raw_settings(0).core_offset_mhz, 0) << "watchdog restored stock before exiting";
  EXPECT_GE(rig.backend->reset_count(0), 0);
}

// ---- JSON-RPC dispatcher and the pipe transport.

rpc::Dispatcher sample_dispatcher() {
  rpc::Dispatcher d;
  d.add("status", false, [](const nlohmann::json&, const platform::CallerInfo& c) -> Result<nlohmann::json> {
    return nlohmann::json{{"ok", true}, {"admin", c.is_admin}, {"pid", c.pid}};
  });
  d.add("set", true, [](const nlohmann::json& p, const platform::CallerInfo&) -> Result<nlohmann::json> {
    if (!p.contains("core")) return make_error(ErrorCode::InvalidArgument, "core missing");
    return nlohmann::json{{"applied", p["core"]}};
  });
  return d;
}

TEST(JsonRpc, WriteMethodsRequireAdministrator) {
  auto d = sample_dispatcher();
  platform::CallerInfo user{false, 1234};
  platform::CallerInfo admin{true, 1};
  auto j = nlohmann::json::parse(d.handle(rpc::make_request("status", {}).dump(), user));
  EXPECT_EQ(j["result"]["ok"], true);
  j = nlohmann::json::parse(d.handle(rpc::make_request("set", {{"core", 30}}).dump(), user));
  EXPECT_EQ(j["error"]["code"], rpc::code_for(ErrorCode::PermissionDenied));
  j = nlohmann::json::parse(d.handle(rpc::make_request("set", {{"core", 30}}).dump(), admin));
  EXPECT_EQ(j["result"]["applied"], 30);
}

TEST(JsonRpc, ProtocolErrors) {
  auto d = sample_dispatcher();
  platform::CallerInfo admin{true, 1};
  EXPECT_EQ(nlohmann::json::parse(d.handle("{not json", admin))["error"]["code"], rpc::kParseError);
  EXPECT_EQ(nlohmann::json::parse(d.handle(R"({"id":1,"method":"status"})", admin))["error"]["code"],
            rpc::kInvalidRequest);
  EXPECT_EQ(nlohmann::json::parse(d.handle(rpc::make_request("nope", {}).dump(), admin))["error"]["code"],
            rpc::kMethodNotFound);
  auto j = nlohmann::json::parse(d.handle(rpc::make_request("set", {{"x", 1}}).dump(), admin));
  EXPECT_EQ(rpc::error_code_from(j["error"]["code"]), ErrorCode::InvalidArgument);
  EXPECT_EQ(j["id"], 1);
}

TEST(Pipe, RoundTripOverLocalTransport) {
  test::TempDir dir;
  test::ScopedEnv sock("GPUOC_SOCKET_DIR", dir.str());
  const std::string name = "gpuoc-test-" + std::to_string(std::random_device{}());
  auto d = sample_dispatcher();
  auto server = platform::PipeServer::start(name, [&](const std::string& req, const platform::CallerInfo& c) {
    return d.handle(req, c);
  });
  ASSERT_TRUE(server.ok()) << server.error().message;
  for (int i = 0; i < 3; ++i) {
    auto r = rpc::call(name, "status", nlohmann::json::object(), Ms(5000));
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value()["ok"], true);
    EXPECT_NE(r.value()["pid"].get<unsigned long>(), 0ul);
  }
  server.value()->stop();
  auto gone = rpc::call(name, "status", nlohmann::json::object(), Ms(1000));
  ASSERT_FALSE(gone.ok());
}

TEST(Pipe, SecondServerCannotSquatTheName) {
#ifndef _WIN32
  GTEST_SKIP() << "FILE_FLAG_FIRST_PIPE_INSTANCE is Windows-specific";
#else
  if (test::running_under_wine()) GTEST_SKIP() << "wine does not enforce FILE_FLAG_FIRST_PIPE_INSTANCE";
  const std::string name = "gpuoc-test-squat-" + std::to_string(std::random_device{}());
  auto d = sample_dispatcher();
  auto h = [&](const std::string& req, const platform::CallerInfo& c) { return d.handle(req, c); };
  auto first = platform::PipeServer::start(name, h);
  ASSERT_TRUE(first.ok());
  auto second = platform::PipeServer::start(name, h);
  EXPECT_FALSE(second.ok());
#endif
}

}  // namespace
}  // namespace gpuoc
