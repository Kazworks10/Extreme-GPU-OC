#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "core/log.hpp"
#include "gpuoc/gpu.hpp"

namespace gpuoc {

namespace {
constexpr auto kLockTimeout = std::chrono::seconds(5);
}

Gpu::Gpu(std::shared_ptr<IGpuBackend> backend, GpuId id, std::shared_ptr<const SafetyPolicy> policy)
    : backend_(std::move(backend)), id_(std::move(id)), policy_(std::move(policy)) {
  if (!backend_ || !policy_) {
    std::fprintf(stderr, "gpuoc: fatal: Gpu requires a backend and a safety policy\n");
    std::abort();
  }
}

void Gpu::set_policy(std::shared_ptr<const SafetyPolicy> policy) {
  if (!policy) return;
  std::lock_guard lk(mu_);
  policy_ = std::move(policy);
}

Result<Capabilities> Gpu::capabilities() {
  std::lock_guard lk(mu_);
  if (caps_) return *caps_;
  GPUOC_TRY_ASSIGN(Capabilities caps, backend_->capabilities(id_));
  caps_ = caps;
  return caps;
}

void Gpu::invalidate_capabilities() {
  std::lock_guard lk(mu_);
  caps_.reset();
}

Result<EffectiveLimits> Gpu::limits() {
  GPUOC_TRY_ASSIGN(Capabilities caps, capabilities());
  std::lock_guard lk(mu_);
  return policy_->limits_for(caps);
}

Result<Telemetry> Gpu::telemetry() { return backend_->read_telemetry(id_); }

Result<Settings> Gpu::settings() { return backend_->read_settings(id_); }

Result<Settings> Gpu::stock_settings() {
  GPUOC_TRY_ASSIGN(Capabilities caps, capabilities());
  return stock_settings_for(caps);
}

std::optional<std::string> Gpu::mismatch(const Settings& want, const Settings& got, const Capabilities& caps) {
  auto cmp = [](const char* what, const std::optional<int>& w, const std::optional<int>& g, int tol,
                const char* unit) -> std::optional<std::string> {
    if (!w) return std::nullopt;
    if (!g) return std::format("{}: requested {}{} but the driver reports no value", what, *w, unit);
    if (std::abs(*w - *g) > tol) return std::format("{}: requested {}{} but read back {}{}", what, *w, unit, *g, unit);
    return std::nullopt;
  };
  if (auto m = cmp("core offset", want.core_offset_mhz, got.core_offset_mhz, 1, " MHz")) return m;
  if (auto m = cmp("memory offset", want.mem_offset_mhz, got.mem_offset_mhz, 1, " MHz")) return m;
  if (auto m = cmp("power limit", want.power_limit_w, got.power_limit_w, 1, " W")) return m;
  const int vstep = caps.voltage_step_mv > 0 ? caps.voltage_step_mv : HardLimits::kVoltageStepFallbackMv;
  if (auto m = cmp("voltage", want.voltage_mv, got.voltage_mv, std::max(1, vstep / 2), " mV")) return m;
  if (want.fan_curve) {
    if (!got.fan_curve) return std::string("fan curve: the driver reports no fan state");
    if (!(*want.fan_curve == *got.fan_curve)) {
      Settings a, b;
      a.fan_curve = want.fan_curve;
      b.fan_curve = got.fan_curve;
      return std::format("fan curve: requested {} but read back {}", a.to_string(), b.to_string());
    }
  }
  return std::nullopt;
}

Result<ApplyReport> Gpu::apply(const Settings& request, ClampMode mode) {
  std::unique_lock lk(mu_, std::defer_lock);
  if (!lk.try_lock_for(kLockTimeout)) return make_error(ErrorCode::Busy, "GPU is busy (lock timeout)");

  GPUOC_TRY_ASSIGN(Capabilities caps, capabilities());
  ApplyReport report;
  report.requested = request;
  GPUOC_TRY_ASSIGN(report.applied, policy_->check(caps, request, mode, &report.notes));
  for (const auto& n : report.notes) log::warn("[{}] {}", id_.uuid, n);
  if (report.applied.empty()) return report;

  // Snapshot for rollback: a partial apply is never left in place silently.
  auto previous = backend_->read_settings(id_);

  auto rollback = [&](const std::string& why) {
    log::error("[{}] apply failed ({}); rolling back", id_.uuid, why);
    if (report.applied.fan_curve && !report.applied.fan_curve->automatic()) {
      Settings fan_auto;
      fan_auto.fan_curve = FanCurve::driver_auto();
      if (auto r = backend_->apply(id_, fan_auto); !r) log::error("[{}] restoring automatic fan failed: {}", id_.uuid, r.error().message);
    }
    if (previous) {
      Settings restore;
      const Settings& p = previous.value();
      if (report.applied.core_offset_mhz) restore.core_offset_mhz = p.core_offset_mhz;
      if (report.applied.mem_offset_mhz) restore.mem_offset_mhz = p.mem_offset_mhz;
      if (report.applied.power_limit_w) restore.power_limit_w = p.power_limit_w;
      if (report.applied.voltage_mv) restore.voltage_mv = p.voltage_mv;
      if (report.applied.fan_curve && p.fan_curve && p.fan_curve->automatic()) restore.fan_curve = p.fan_curve;
      if (!restore.empty()) {
        if (auto r = backend_->apply(id_, restore); !r) {
          log::error("[{}] rollback failed: {}; resetting to stock", id_.uuid, r.error().message);
          (void)backend_->reset_to_stock(id_);
        }
      }
    } else {
      (void)backend_->reset_to_stock(id_);
    }
  };

  log::info("[{}] apply {}{}", id_.uuid, report.applied.to_string(),
            std::string_view(backend_->name()).starts_with("dry-run") ? " (dry-run)" : "");
  if (auto r = backend_->apply(id_, report.applied); !r) {
    rollback(r.error().message);
    return r.error();
  }
  auto readback = backend_->read_settings(id_);
  if (!readback) {
    rollback("read-back failed: " + readback.error().message);
    return make_error(ErrorCode::VerifyFailed, "read-back after apply failed: " + readback.error().message);
  }
  if (auto m = mismatch(report.applied, readback.value(), caps)) {
    // The read-back cannot be trusted, so neither can the "previous" values:
    // fall back to stock, which is always safe.
    log::error("[{}] read-back mismatch ({}); resetting to stock", id_.uuid, *m);
    if (auto r = backend_->reset_to_stock(id_); !r) log::error("[{}] reset failed: {}", id_.uuid, r.error().message);
    return make_error(ErrorCode::VerifyFailed, *m);
  }
  return report;
}

Result<void> Gpu::reset_to_stock() {
  std::unique_lock lk(mu_, std::defer_lock);
  if (!lk.try_lock_for(kLockTimeout)) {
    // A stuck holder must not prevent a safety revert.
    log::warn("[{}] lock timeout during reset; resetting without the lock", id_.uuid);
  }
  auto caps = caps_ ? Result<Capabilities>(*caps_) : backend_->capabilities(id_);
  if (auto r = backend_->reset_to_stock(id_); !r) {
    // Best effort: at least return fans to driver control.
    Settings fan_auto;
    fan_auto.fan_curve = FanCurve::driver_auto();
    if (caps && caps.value().fan_control) (void)backend_->apply(id_, fan_auto);
    return r.error();
  }
  if (!caps) return success();
  const Settings stock = stock_settings_for(caps.value());
  GPUOC_TRY_ASSIGN(Settings readback, backend_->read_settings(id_));
  if (auto m = mismatch(stock, readback, caps.value())) {
    return make_error(ErrorCode::VerifyFailed, "stock restore did not verify: " + *m);
  }
  return success();
}

Result<void> Gpu::maintain_fan() {
  std::unique_lock lk(mu_, std::defer_lock);
  if (!lk.try_lock_for(kLockTimeout)) return make_error(ErrorCode::Busy, "GPU is busy (lock timeout)");
  GPUOC_TRY_ASSIGN(Capabilities caps, capabilities());
  if (!caps.fan_control) return success();
  GPUOC_TRY_ASSIGN(Settings cur, backend_->read_settings(id_));
  if (!cur.fan_curve || cur.fan_curve->automatic()) return success();
  Settings s;
  s.fan_curve = cur.fan_curve;
  if (auto r = backend_->apply(id_, s); !r) {
    log::error("[{}] fan curve could not be applied ({}); restoring automatic fan control", id_.uuid,
               r.error().message);
    Settings fan_auto;
    fan_auto.fan_curve = FanCurve::driver_auto();
    (void)backend_->apply(id_, fan_auto);
    return r.error();
  }
  return success();
}

// ---- StockGuard ---------------------------------------------------------------

StockGuard::StockGuard(Gpu& gpu) : gpu_(&gpu) {
  if (auto s = gpu.stock_settings()) {
    stock_ = s.value();
  } else {
    log::warn("[{}] StockGuard: could not snapshot stock settings: {}", gpu.id().uuid, s.error().message);
  }
  StockRegistry::instance().add(gpu_);
}

StockGuard::~StockGuard() {
  if (auto r = restore(); !r) log::error("[{}] StockGuard: stock restore failed: {}", gpu_->id().uuid, r.error().message);
  StockRegistry::instance().remove(gpu_);
}

Result<void> StockGuard::restore() {
  auto r = gpu_->reset_to_stock();
  if (r) return r;
  // Fallback: write the snapshot explicitly.
  if (stock_) {
    log::warn("[{}] reset_to_stock failed ({}); writing stock snapshot", gpu_->id().uuid, r.error().message);
    if (auto a = gpu_->apply(*stock_, ClampMode::Clamp)) return success();
  }
  return r;
}

// ---- StockRegistry ------------------------------------------------------------

StockRegistry& StockRegistry::instance() {
  static StockRegistry r;
  return r;
}

void StockRegistry::add(Gpu* gpu) {
  std::lock_guard lk(mu_);
  gpus_.push_back(gpu);
}

void StockRegistry::remove(Gpu* gpu) {
  std::lock_guard lk(mu_);
  auto it = std::find(gpus_.begin(), gpus_.end(), gpu);
  if (it != gpus_.end()) gpus_.erase(it);
}

std::size_t StockRegistry::size() const {
  std::lock_guard lk(mu_);
  return gpus_.size();
}

int StockRegistry::restore_all(const char* reason) noexcept {
  std::vector<Gpu*> copy;
  {
    std::lock_guard lk(mu_);
    copy = gpus_;
  }
  // De-duplicate: several guards may cover one GPU.
  std::sort(copy.begin(), copy.end());
  copy.erase(std::unique(copy.begin(), copy.end()), copy.end());
  int failures = 0;
  for (Gpu* g : copy) {
    log::warn("[{}] restoring stock ({})", g->id().uuid, reason ? reason : "unspecified");
    if (auto r = g->reset_to_stock(); !r) {
      ++failures;
      log::error("[{}] stock restore failed: {}", g->id().uuid, r.error().message);
    }
  }
  return failures;
}

}  // namespace gpuoc
