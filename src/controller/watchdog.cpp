#include "controller/watchdog.hpp"

#include <cstdlib>

#include "core/heartbeat.hpp"
#include "core/log.hpp"
#include "gpuoc/gpu.hpp"

namespace gpuoc {

Watchdog::Watchdog(std::chrono::milliseconds timeout, Actions actions)
    : timeout_(timeout), actions_(std::move(actions)) {
  if (!actions_.revert) actions_.revert = [](const char* why) { StockRegistry::instance().restore_all(why); };
  if (!actions_.exit) actions_.exit = [](int code) { std::_Exit(code); };
}

Watchdog::~Watchdog() { stop(); }

void Watchdog::start() {
  stop();
  {
    std::lock_guard lk(mu_);
    stop_ = false;
  }
  heartbeat::beat();
  thread_ = std::thread([this] { loop(); });
}

void Watchdog::stop() {
  {
    std::lock_guard lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void Watchdog::loop() {
  const auto check = std::max(std::chrono::milliseconds(10), timeout_ / 10);
  std::unique_lock lk(mu_);
  while (!stop_) {
    cv_.wait_for(lk, check, [this] { return stop_; });
    if (stop_) break;
    if (heartbeat::ms_since_last() > timeout_.count()) {
      fired_.store(true);
      lk.unlock();
      log::error("watchdog: no heartbeat for {} ms; reverting to stock and exiting", heartbeat::ms_since_last());
      actions_.revert("watchdog timeout");
      actions_.exit(kExitCode);
      return;
    }
  }
}

}  // namespace gpuoc
