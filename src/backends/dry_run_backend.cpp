#include "backends/dry_run_backend.hpp"

#include <format>

#include "core/log.hpp"

namespace gpuoc {

DryRunBackend::DryRunBackend(std::shared_ptr<IGpuBackend> inner)
    : inner_(std::move(inner)), name_(std::string("dry-run(") + inner_->name() + ")") {}

Result<Settings> DryRunBackend::read_settings(const GpuId& id) {
  GPUOC_TRY_ASSIGN(Settings real, inner_->read_settings(id));
  std::lock_guard lk(mu_);
  auto it = shadow_.find(id.uuid);
  return it == shadow_.end() ? real : real.merged(it->second);
}

Result<void> DryRunBackend::apply(const GpuId& id, const Settings& s) {
  const std::string line = std::format("[dry-run] {} apply {}", id.uuid, s.to_string());
  log::info("{}", line);
  std::lock_guard lk(mu_);
  journal_.push_back(line);
  shadow_[id.uuid] = shadow_[id.uuid].merged(s);
  return success();
}

Result<void> DryRunBackend::reset_to_stock(const GpuId& id) {
  const std::string line = std::format("[dry-run] {} reset_to_stock", id.uuid);
  log::info("{}", line);
  auto caps = inner_->capabilities(id);
  std::lock_guard lk(mu_);
  journal_.push_back(line);
  if (caps) {
    shadow_[id.uuid] = stock_settings_for(caps.value());
  } else {
    shadow_.erase(id.uuid);
  }
  return success();
}

std::vector<std::string> DryRunBackend::journal() const {
  std::lock_guard lk(mu_);
  return journal_;
}

}  // namespace gpuoc
