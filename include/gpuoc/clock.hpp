// Time source. Everything that waits or timestamps goes through IClock so the
// whole autonomous pipeline can run against a simulated clock in tests.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

namespace gpuoc {

using Ms = std::chrono::milliseconds;
using Sec = std::chrono::seconds;
using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;

class IClock {
 public:
  virtual SteadyTime now() const = 0;
  virtual WallTime wall_now() const = 0;
  virtual void sleep_for(Ms duration) = 0;
  virtual ~IClock() = default;
};

class SystemClock final : public IClock {
 public:
  SteadyTime now() const override;
  WallTime wall_now() const override;
  void sleep_for(Ms duration) override;
  static SystemClock& instance();
};

// Deterministic simulated clock: sleep_for advances time instantly.
class SimClock final : public IClock {
 public:
  explicit SimClock(WallTime wall_epoch = WallTime(std::chrono::seconds(1'800'000'000)));
  SteadyTime now() const override;
  WallTime wall_now() const override;
  void sleep_for(Ms duration) override;
  void advance(Ms duration) { sleep_for(duration); }
  [[nodiscard]] Ms elapsed() const;

 private:
  mutable std::mutex mu_;
  SteadyTime steady_epoch_;
  WallTime wall_epoch_;
  Ms elapsed_{0};
};

// Seconds between two steady time points as double.
inline double seconds_between(SteadyTime a, SteadyTime b) {
  return std::chrono::duration<double>(b - a).count();
}

// Sleep in slices so an abort flag is honoured promptly.
bool interruptible_sleep(IClock& clock, Ms duration, const std::atomic<bool>* abort, Ms slice = Ms(250));

}  // namespace gpuoc
