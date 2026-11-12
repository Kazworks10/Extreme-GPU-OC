// --dry-run: reads go to the wrapped backend; every write is logged with the
// value it would have set and nothing is applied. read_settings() reports the
// shadow ("would-be") settings so read-back verification still exercises the
// full code path.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpuoc/backend.hpp"

namespace gpuoc {

class DryRunBackend final : public IGpuBackend {
 public:
  explicit DryRunBackend(std::shared_ptr<IGpuBackend> inner);

  Result<std::vector<GpuId>> enumerate() override { return inner_->enumerate(); }
  Result<Capabilities> capabilities(const GpuId& id) override { return inner_->capabilities(id); }
  Result<Telemetry> read_telemetry(const GpuId& id) override { return inner_->read_telemetry(id); }
  Result<Settings> read_settings(const GpuId& id) override;
  Result<void> apply(const GpuId& id, const Settings& s) override;
  Result<void> reset_to_stock(const GpuId& id) override;
  [[nodiscard]] const char* name() const noexcept override { return name_.c_str(); }

  // Every write that would have happened, in order (for tests and reports).
  [[nodiscard]] std::vector<std::string> journal() const;
  [[nodiscard]] IGpuBackend& inner() { return *inner_; }

 private:
  std::shared_ptr<IGpuBackend> inner_;
  std::string name_;
  mutable std::mutex mu_;
  std::map<std::string, Settings> shadow_;
  std::vector<std::string> journal_;
};

}  // namespace gpuoc
