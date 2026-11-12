#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

#include "gpuoc/api.hpp"

namespace gpuoc {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::OutOfRange: return "out_of_range";
    case ErrorCode::SafetyRefusal: return "safety_refusal";
    case ErrorCode::HardwareError: return "hardware_error";
    case ErrorCode::GpuLost: return "gpu_lost";
    case ErrorCode::VerifyFailed: return "verify_failed";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::PermissionDenied: return "permission_denied";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::ParseError: return "parse_error";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::Aborted: return "aborted";
    case ErrorCode::Unavailable: return "unavailable";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

std::string Error::to_string() const { return std::format("{}: {}", gpuoc::to_string(code), message); }

Range intersect(const Range& a, const Range& b) noexcept {
  return Range{std::max(a.min, b.min), std::min(a.max, b.max)};
}

int FanCurve::evaluate(double temp_c) const noexcept {
  if (points.empty()) return 0;
  if (temp_c <= points.front().temp_c) return points.front().pct;
  if (temp_c >= points.back().temp_c) return points.back().pct;
  for (std::size_t i = 1; i < points.size(); ++i) {
    const auto& a = points[i - 1];
    const auto& b = points[i];
    if (temp_c <= b.temp_c) {
      const double f = (temp_c - a.temp_c) / static_cast<double>(b.temp_c - a.temp_c);
      return static_cast<int>(std::lround(a.pct + f * (b.pct - a.pct)));
    }
  }
  return points.back().pct;
}

Result<void> FanCurve::validate() const {
  if (points.size() == 1) return make_error(ErrorCode::InvalidArgument, "fan curve needs at least two points");
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto& p = points[i];
    if (p.pct < 0 || p.pct > 100)
      return make_error(ErrorCode::InvalidArgument, std::format("fan curve point {}: pct {} not in 0..100", i, p.pct));
    if (p.temp_c < 0 || p.temp_c > 120)
      return make_error(ErrorCode::InvalidArgument,
                        std::format("fan curve point {}: temp {} C not in 0..120", i, p.temp_c));
    if (i > 0 && p.temp_c <= points[i - 1].temp_c)
      return make_error(ErrorCode::InvalidArgument, "fan curve temperatures must be strictly increasing");
  }
  return success();
}

Settings Settings::merged(const Settings& over) const {
  Settings s = *this;
  if (over.core_offset_mhz) s.core_offset_mhz = over.core_offset_mhz;
  if (over.mem_offset_mhz) s.mem_offset_mhz = over.mem_offset_mhz;
  if (over.power_limit_w) s.power_limit_w = over.power_limit_w;
  if (over.fan_curve) s.fan_curve = over.fan_curve;
  if (over.voltage_mv) s.voltage_mv = over.voltage_mv;
  return s;
}

std::string Settings::to_string() const {
  std::string out;
  auto add = [&](const std::string& part) {
    if (!out.empty()) out += ' ';
    out += part;
  };
  if (core_offset_mhz) add(std::format("core={:+}MHz", *core_offset_mhz));
  if (mem_offset_mhz) add(std::format("mem={:+}MHz", *mem_offset_mhz));
  if (power_limit_w) add(std::format("power={}W", *power_limit_w));
  if (voltage_mv) add(std::format("voltage={}mV", *voltage_mv));
  if (fan_curve) {
    if (fan_curve->automatic()) {
      add("fan=auto");
    } else {
      std::string c = "fan=[";
      for (std::size_t i = 0; i < fan_curve->points.size(); ++i) {
        if (i) c += ',';
        c += std::format("{}C:{}%", fan_curve->points[i].temp_c, fan_curve->points[i].pct);
      }
      add(c + "]");
    }
  }
  return out.empty() ? "(none)" : out;
}

Settings stock_settings_for(const Capabilities& caps) {
  Settings s;
  if (caps.core_offset) s.core_offset_mhz = 0;
  if (caps.mem_offset) s.mem_offset_mhz = 0;
  if (caps.power_limit && caps.stock_power_limit_w > 0) s.power_limit_w = caps.stock_power_limit_w;
  if (caps.fan_control) s.fan_curve = FanCurve::driver_auto();
  if (caps.voltage_control && caps.stock_voltage_mv > 0) s.voltage_mv = caps.stock_voltage_mv;
  return s;
}

// ---- clocks -----------------------------------------------------------------

SteadyTime SystemClock::now() const { return std::chrono::steady_clock::now(); }
WallTime SystemClock::wall_now() const { return std::chrono::system_clock::now(); }
void SystemClock::sleep_for(Ms duration) {
  if (duration.count() > 0) std::this_thread::sleep_for(duration);
}
SystemClock& SystemClock::instance() {
  static SystemClock c;
  return c;
}

SimClock::SimClock(WallTime wall_epoch) : steady_epoch_(SteadyTime(std::chrono::hours(1000))), wall_epoch_(wall_epoch) {}

SteadyTime SimClock::now() const {
  std::lock_guard lk(mu_);
  return steady_epoch_ + elapsed_;
}
WallTime SimClock::wall_now() const {
  std::lock_guard lk(mu_);
  return wall_epoch_ + elapsed_;
}
void SimClock::sleep_for(Ms duration) {
  if (duration.count() <= 0) return;
  std::lock_guard lk(mu_);
  elapsed_ += duration;
}
Ms SimClock::elapsed() const {
  std::lock_guard lk(mu_);
  return elapsed_;
}

bool interruptible_sleep(IClock& clock, Ms duration, const std::atomic<bool>* abort, Ms slice) {
  if (slice.count() <= 0) slice = Ms(250);
  Ms left = duration;
  while (left.count() > 0) {
    if (abort && abort->load()) return false;
    const Ms chunk = std::min(left, slice);
    clock.sleep_for(chunk);
    left -= chunk;
  }
  return !(abort && abort->load());
}

}  // namespace gpuoc
