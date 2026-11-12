// Process-wide liveness heartbeat. Long-running loops (stress iterations,
// benchmark iterations, monitor ticks, controller steps) call beat(); the
// service watchdog fires if any registered thread stops beating.
//
// Threads that must be watched (controller threads, the main loop) hold a
// ThreadScope; beat() then updates that thread's slot, and the watchdog
// looks at the stalest slot, so one busy thread cannot hide a hung one.
// Without any registered thread a single process-wide slot is used.
#pragma once

#include <chrono>

namespace gpuoc::heartbeat {

void beat() noexcept;
// Milliseconds since the stalest registered thread last beat.
long long ms_since_last() noexcept;

class ThreadScope {
 public:
  ThreadScope();
  ~ThreadScope();
  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;

 private:
  int slot_;
};

}  // namespace gpuoc::heartbeat
