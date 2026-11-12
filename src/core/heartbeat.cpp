#include "core/heartbeat.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace gpuoc::heartbeat {
namespace {

long long now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Registry {
  std::mutex mu;
  std::map<int, long long> slots;  // registered threads
  int next = 1;
  std::atomic<long long> global{now_ms()};
};

Registry& reg() {
  static Registry r;
  return r;
}

thread_local int t_slot = 0;

}  // namespace

void beat() noexcept {
  auto& r = reg();
  const long long t = now_ms();
  r.global.store(t);
  if (t_slot != 0) {
    std::lock_guard lk(r.mu);
    auto it = r.slots.find(t_slot);
    if (it != r.slots.end()) it->second = t;
  }
}

long long ms_since_last() noexcept {
  auto& r = reg();
  const long long t = now_ms();
  std::lock_guard lk(r.mu);
  if (r.slots.empty()) return t - r.global.load();
  long long oldest = t;
  for (const auto& [id, last] : r.slots) {
    (void)id;
    oldest = std::min(oldest, last);
  }
  return t - oldest;
}

ThreadScope::ThreadScope() {
  auto& r = reg();
  std::lock_guard lk(r.mu);
  slot_ = r.next++;
  r.slots[slot_] = now_ms();
  t_slot = slot_;
}

ThreadScope::~ThreadScope() {
  auto& r = reg();
  std::lock_guard lk(r.mu);
  r.slots.erase(slot_);
  t_slot = 0;
}

}  // namespace gpuoc::heartbeat
