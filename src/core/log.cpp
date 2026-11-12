#include "core/log.hpp"

#include <atomic>
#include <chrono>
#include <ctime>

#include "platform/fs.hpp"

namespace gpuoc::log {
namespace {

struct State {
  std::mutex mu;
  std::atomic<int> level{static_cast<int>(Level::Info)};
  bool to_stderr = true;
  std::FILE* file = nullptr;
  Capture capture;
};

State& state() {
  static State s;
  return s;
}

std::string timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  return std::format("{}.{:03}Z", buf, ms);
}

}  // namespace

const char* level_name(Level l) noexcept {
  switch (l) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
  }
  return "?";
}

void set_level(Level l) { state().level.store(static_cast<int>(l)); }
Level level() { return static_cast<Level>(state().level.load()); }

void set_stderr(bool enabled) {
  std::lock_guard lk(state().mu);
  state().to_stderr = enabled;
}

bool open_file(const std::string& path_utf8) {
  auto& s = state();
  std::lock_guard lk(s.mu);
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
  s.file = platform::fopen_utf8(path_utf8, "ab");
  return s.file != nullptr;
}

void close_file() {
  auto& s = state();
  std::lock_guard lk(s.mu);
  if (s.file) std::fclose(s.file);
  s.file = nullptr;
}

void set_capture(Capture cap) {
  std::lock_guard lk(state().mu);
  state().capture = std::move(cap);
}

void write(Level l, std::string_view msg) {
  auto& s = state();
  if (static_cast<int>(l) < s.level.load()) return;
  const std::string line = std::format("{} [{}] {}\n", timestamp(), level_name(l), msg);
  std::lock_guard lk(s.mu);
  if (s.to_stderr) {
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
  }
  if (s.file) {
    std::fputs(line.c_str(), s.file);
    std::fflush(s.file);
  }
  if (s.capture) s.capture(l, std::string(msg));
}

}  // namespace gpuoc::log
