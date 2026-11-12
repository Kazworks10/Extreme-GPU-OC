// Minimal thread-safe logger: stderr and an optional file sink.
#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace gpuoc::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

const char* level_name(Level l) noexcept;

// Configure sinks. Safe to call at any time.
void set_level(Level l);
Level level();
void set_stderr(bool enabled);
bool open_file(const std::string& path_utf8);
void close_file();
// Test hook: capture every formatted line.
using Capture = std::function<void(Level, const std::string&)>;
void set_capture(Capture cap);

void write(Level l, std::string_view msg);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Debug) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Info) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Warn) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Error) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace gpuoc::log
