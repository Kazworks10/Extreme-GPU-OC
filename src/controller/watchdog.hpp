// Watchdog thread. If the heartbeat is not fed within the timeout it reverts
// every guarded GPU to stock and exits the process; the Service Control
// Manager then restarts the service under its failure-action policy, and the
// restart path reverts to stock again before anything else.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace gpuoc {

class Watchdog {
 public:
  struct Actions {
    std::function<void(const char* reason)> revert;  // default: StockRegistry::restore_all
    std::function<void(int code)> exit;              // default: std::_Exit
  };

  Watchdog(std::chrono::milliseconds timeout, Actions actions = {});
  ~Watchdog();
  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  void start();
  void stop();
  [[nodiscard]] bool fired() const noexcept { return fired_.load(); }

  static constexpr int kExitCode = 3;

 private:
  void loop();

  std::chrono::milliseconds timeout_;
  Actions actions_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> fired_{false};
};

}  // namespace gpuoc
