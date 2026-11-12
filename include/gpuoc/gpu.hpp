// Gpu facade and StockGuard. The rest of the program writes to GPUs only
// through Gpu, which enforces capability checks, driver ranges, safety limits
// and gates, and read-back verification.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gpuoc/backend.hpp"
#include "gpuoc/safety.hpp"

namespace gpuoc {

struct ApplyReport {
  Settings requested;
  Settings applied;                 // what was written (after clamping)
  std::vector<std::string> notes;   // clamping and other adjustments
};

class Gpu {
 public:
  Gpu(std::shared_ptr<IGpuBackend> backend, GpuId id, std::shared_ptr<const SafetyPolicy> policy);

  Gpu(const Gpu&) = delete;
  Gpu& operator=(const Gpu&) = delete;

  [[nodiscard]] const GpuId& id() const noexcept { return id_; }
  [[nodiscard]] IGpuBackend& backend() noexcept { return *backend_; }
  [[nodiscard]] const SafetyPolicy& policy() const noexcept { return *policy_; }
  void set_policy(std::shared_ptr<const SafetyPolicy> policy);

  // Cached after the first successful read.
  Result<Capabilities> capabilities();
  void invalidate_capabilities();
  Result<EffectiveLimits> limits();

  Result<Telemetry> telemetry();
  Result<Settings> settings();
  Result<Settings> stock_settings();

  // Check against capabilities and safety, write, then read back and verify.
  Result<ApplyReport> apply(const Settings& request, ClampMode mode = ClampMode::Reject);

  // Restore stock settings and verify the read-back.
  Result<void> reset_to_stock();

  // Re-evaluate the active software fan curve against the current
  // temperature (NVML exposes only fixed fan speeds). No-op if automatic.
  Result<void> maintain_fan();

  // Compare two settings within backend tolerances. Only fields set in `want`
  // are compared. Returns a description of the first mismatch.
  [[nodiscard]] static std::optional<std::string> mismatch(const Settings& want, const Settings& got,
                                                           const Capabilities& caps);

 private:
  std::shared_ptr<IGpuBackend> backend_;
  GpuId id_;
  std::shared_ptr<const SafetyPolicy> policy_;
  std::recursive_timed_mutex mu_;
  std::optional<Capabilities> caps_;
};

// Snapshots stock settings on construction and restores them on destruction,
// including on error paths. Registered globally so Ctrl+C / service stop /
// watchdog handlers can restore every guarded GPU synchronously.
class StockGuard {
 public:
  explicit StockGuard(Gpu& gpu);
  ~StockGuard();

  StockGuard(const StockGuard&) = delete;
  StockGuard& operator=(const StockGuard&) = delete;

  // Restore now. The guard stays registered and restores again on destruction
  // if anything was written in between.
  Result<void> restore();
  [[nodiscard]] const std::optional<Settings>& stock_snapshot() const noexcept { return stock_; }
  [[nodiscard]] Gpu& gpu() noexcept { return *gpu_; }

 private:
  Gpu* gpu_;
  std::optional<Settings> stock_;
};

// Process-wide registry of GPUs under a StockGuard.
class StockRegistry {
 public:
  static StockRegistry& instance();
  void add(Gpu* gpu);
  void remove(Gpu* gpu);
  // Restore stock on every registered GPU. Safe to call from any thread and
  // more than once. Returns the number of GPUs that failed to restore.
  int restore_all(const char* reason) noexcept;
  [[nodiscard]] std::size_t size() const;

 private:
  mutable std::mutex mu_;
  std::vector<Gpu*> gpus_;
};

}  // namespace gpuoc
