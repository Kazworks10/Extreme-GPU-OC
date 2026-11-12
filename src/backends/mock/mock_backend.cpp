#include "backends/mock/mock_backend.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>

#include "core/log.hpp"
#include "platform/fs.hpp"

namespace gpuoc::mock {
namespace {

// Portable random helpers: std::*_distribution output is implementation-
// defined, so the mock derives its numbers directly from mt19937_64 to stay
// identical across MSVC, clang, and GCC.
double uniform01(std::mt19937_64& rng) { return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0); }

double normal01(std::mt19937_64& rng) {
  double u1 = uniform01(rng);
  const double u2 = uniform01(rng);
  if (u1 < 1e-300) u1 = 1e-300;
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

class MockWorkload final : public IWorkload {
 public:
  MockWorkload(std::shared_ptr<MockBackend> backend, int index) : b_(std::move(backend)), idx_(index) {}
  ~MockWorkload() override { stop(); }
  [[nodiscard]] std::string name() const override { return "mock-kernel"; }
  [[nodiscard]] std::string unit() const override { return "Gop/s"; }
  Result<void> start() override {
    b_->set_load(idx_, true);
    running_ = true;
    return success();
  }
  IterationResult run_iteration() override {
    if (!running_) return IterationResult{IterationStatus::StartFailed, 0, 0, "not started"};
    b_->clock().sleep_for(Ms(1000));
    return b_->simulate_iteration(idx_, Ms(1000));
  }
  void stop() override {
    if (running_) b_->set_load(idx_, false);
    running_ = false;
  }

 private:
  std::shared_ptr<MockBackend> b_;
  int idx_;
  bool running_ = false;
};

class MockEventLog final : public IEventLogSource {
 public:
  explicit MockEventLog(std::weak_ptr<MockBackend> b) : b_(std::move(b)) {}
  [[nodiscard]] bool available() const override { return true; }
  Result<std::vector<DriverEvent>> driver_events_since(WallTime since) override {
    auto b = b_.lock();
    if (!b) return make_error(ErrorCode::Unavailable, "mock backend gone");
    return b->events_since(since);
  }

 private:
  std::weak_ptr<MockBackend> b_;
};

}  // namespace

MockBackend::MockBackend(MockOptions opts) : opts_(std::move(opts)) {
  clock_ = opts_.clock ? opts_.clock : std::shared_ptr<IClock>(&SystemClock::instance(), [](IClock*) {});
  const int n = std::max(0, opts_.gpu_count);
  gpus_.resize(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    Gpu& g = gpus_[static_cast<std::size_t>(i)];
    g.spec = static_cast<std::size_t>(i) < opts_.specs.size() ? opts_.specs[static_cast<std::size_t>(i)] : MockGpuSpec{};
    g.rng.seed(opts_.seed * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(i) + 1);
    if (g.spec.true_core_max_mhz < 0) g.spec.true_core_max_mhz = 45 + static_cast<int>(g.rng() % 210);
    if (g.spec.true_mem_max_mhz < 0) g.spec.true_mem_max_mhz = 300 + static_cast<int>(g.rng() % 1200);
    g.id.index = i;
    g.id.name = g.spec.name;
    g.id.uuid = std::format("GPU-00000000-0000-4000-8000-{:012x}",
                            (opts_.seed * 1000003ULL + static_cast<std::uint64_t>(i)) & 0xffffffffffffULL);
    g.power = g.spec.stock_power_w;
    g.voltage = g.spec.stock_voltage_mv;
    g.temp = g.spec.ambient_c + 8;
    g.fan_pct = 30;
    g.last = clock_->now();
  }
  load_state();
}

Result<int> MockBackend::find(const GpuId& id) const {
  for (std::size_t i = 0; i < gpus_.size(); ++i) {
    if (gpus_[i].id.uuid == id.uuid) return static_cast<int>(i);
  }
  return make_error(ErrorCode::NotFound, std::format("no mock GPU with UUID {}", id.uuid));
}

double MockBackend::stable_core_max(const Gpu& g) const {
  double max = g.spec.true_core_max_mhz;
  if (g.spec.voltage_control && g.voltage > g.spec.stock_voltage_mv) {
    max += std::min<double>(g.spec.volt_gain_cap_mhz, (g.voltage - g.spec.stock_voltage_mv) * g.spec.volt_gain_mhz_per_mv);
  }
  return max;
}

void MockBackend::step_physics(Gpu& g, double dt) {
  const auto& s = g.spec;
  const double util = g.load ? 1.0 : 0.02;
  const double v = s.voltage_control ? static_cast<double>(g.voltage) : static_cast<double>(s.stock_voltage_mv);
  const double vf = (v / s.stock_voltage_mv) * (v / s.stock_voltage_mv);
  const double core_clock = s.base_core_mhz + g.core;
  const double mem_clock = s.base_mem_mhz + g.mem;
  const double clock_factor = 0.8 * core_clock / s.base_core_mhz + 0.2 * mem_clock / s.base_mem_mhz;
  double perf = 0.75 * core_clock / s.base_core_mhz + 0.25 * mem_clock / s.base_mem_mhz;
  double power = s.idle_power_w + util * s.load_power_w * clock_factor * vf;
  std::uint32_t flags = kLimitNone;
  if (power > g.power) {
    const double scale = (g.power - s.idle_power_w) / std::max(1.0, power - s.idle_power_w);
    perf *= std::clamp(scale, 0.1, 1.0);
    power = g.power;
    if (g.load) flags |= kLimitPower;
  }
  power += g.faults.extra_power_w;

  double fan = 0;
  if (g.fan.automatic()) {
    fan = std::clamp(30.0 + (g.temp - 50.0) * 2.5, 30.0, 100.0);
  } else {
    fan = std::clamp<double>(g.fan.evaluate(g.temp), s.fan_pct.min, s.fan_pct.max);
  }
  const double r = s.r_fan0 - (s.r_fan0 - s.r_fan100) * fan / 100.0;
  const double t_ss = s.ambient_c + power * r;
  g.temp += (t_ss - g.temp) * (1.0 - std::exp(-dt / s.thermal_tau_s));
  if (g.temp > s.throttle_temp_c) {
    flags |= kLimitThermal;
    perf *= 0.9;
  }
  g.fan_pct = fan;
  g.last_power = power;
  g.perf = perf;
  g.flags = flags;
}

void MockBackend::advance(Gpu& g) {
  const SteadyTime now = clock_->now();
  double dt = seconds_between(g.last, now);
  g.last = now;
  if (dt <= 0) return;
  dt = std::min(dt, 12.0 * g.spec.thermal_tau_s);  // steady state reached anyway
  while (dt > 0) {
    const double h = std::min(dt, 1.0);
    step_physics(g, h);
    dt -= h;
  }
}

Result<std::vector<GpuId>> MockBackend::enumerate() {
  std::lock_guard lk(mu_);
  std::vector<GpuId> out;
  for (const auto& g : gpus_) out.push_back(g.id);
  return out;
}

Result<Capabilities> MockBackend::capabilities(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(int i, find(id));
  const auto& s = gpus_[static_cast<std::size_t>(i)].spec;
  Capabilities c;
  c.core_offset = true;
  c.mem_offset = true;
  c.power_limit = true;
  c.fan_control = s.fan_control;
  c.voltage_control = s.voltage_control;
  c.locked_clocks = s.locked_clocks;
  c.core_offset_mhz = s.core_offset_mhz;
  c.mem_offset_mhz = s.mem_offset_mhz;
  c.power_limit_w = s.power_limit_w;
  c.voltage_mv = s.voltage_control ? s.voltage_mv : Range{};
  c.stock_power_limit_w = s.stock_power_w;
  c.stock_voltage_mv = s.voltage_control ? s.stock_voltage_mv : 0;
  c.voltage_step_mv = s.voltage_control ? s.voltage_step_mv : 0;
  c.fan_pct = s.fan_pct;
  c.ecc_supported = s.ecc_supported;
  c.driver_version = opts_.driver_version;
  return c;
}

Result<Telemetry> MockBackend::read_telemetry(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(int i, find(id));
  Gpu& g = gpus_[static_cast<std::size_t>(i)];
  if (g.faults.telemetry_missing) return make_error(ErrorCode::Unavailable, "mock: telemetry unavailable");
  advance(g);
  Telemetry t;
  const double clock_scale = g.perf / (0.75 * (g.spec.base_core_mhz + g.core) / g.spec.base_core_mhz +
                                       0.25 * (g.spec.base_mem_mhz + g.mem) / g.spec.base_mem_mhz);
  t.core_mhz = (g.spec.base_core_mhz + g.core) * (g.load ? clock_scale : 0.08);
  t.mem_mhz = g.load ? g.spec.base_mem_mhz + g.mem : 405.0;
  t.temp_c = g.temp + g.faults.temp_offset_c;
  t.power_w = g.last_power;
  t.fan_pct = g.fan_pct;
  t.util_pct = g.load ? 100.0 : 2.0;
  if (g.spec.voltage_control) t.voltage_mv = static_cast<double>(g.voltage);
  t.ecc_uncorrected = g.spec.ecc_supported ? g.ecc : 0;
  t.limit_flags = g.flags;
  t.ts = clock_->now();
  return t;
}

Result<Settings> MockBackend::read_settings(const GpuId& id) {
  std::lock_guard lk(mu_);
  GPUOC_TRY_ASSIGN(int i, find(id));
  const Gpu& g = gpus_[static_cast<std::size_t>(i)];
  Settings s;
  s.core_offset_mhz = g.core + (g.faults.readback_mismatch ? 50 : 0);
  s.mem_offset_mhz = g.mem;
  s.power_limit_w = g.power;
  if (g.spec.fan_control) s.fan_curve = g.fan;
  if (g.spec.voltage_control) s.voltage_mv = g.voltage;
  return s;
}

Result<void> MockBackend::apply(const GpuId& id, const Settings& s) {
  {
    std::lock_guard lk(mu_);
    GPUOC_TRY_ASSIGN(int i, find(id));
    Gpu& g = gpus_[static_cast<std::size_t>(i)];
    advance(g);
    if (g.faults.fail_apply) return make_error(ErrorCode::HardwareError, "mock: apply failed (injected)");
    // Behave like a driver: reject values outside the reported range.
    auto check = [](const char* what, const std::optional<int>& v, const Range& r) -> Result<void> {
      if (v && !r.contains(*v))
        return make_error(ErrorCode::InvalidArgument, std::format("mock driver: {} {} outside [{}, {}]", what, *v, r.min, r.max));
      return success();
    };
    GPUOC_TRY(check("core offset", s.core_offset_mhz, g.spec.core_offset_mhz));
    GPUOC_TRY(check("memory offset", s.mem_offset_mhz, g.spec.mem_offset_mhz));
    GPUOC_TRY(check("power limit", s.power_limit_w, g.spec.power_limit_w));
    if (s.voltage_mv) {
      if (!g.spec.voltage_control) return make_error(ErrorCode::Unsupported, "mock driver: voltage control not exposed");
      GPUOC_TRY(check("voltage", s.voltage_mv, g.spec.voltage_mv));
    }
    if (s.fan_curve) {
      if (!g.spec.fan_control) return make_error(ErrorCode::Unsupported, "mock driver: fan control not exposed");
      if (g.faults.fail_fan_apply && !s.fan_curve->automatic())
        return make_error(ErrorCode::HardwareError, "mock: fan write failed (injected)");
    }
    if (s.core_offset_mhz) g.core = *s.core_offset_mhz;
    if (s.mem_offset_mhz) g.mem = *s.mem_offset_mhz;
    if (s.power_limit_w) g.power = *s.power_limit_w;
    if (s.voltage_mv) g.voltage = *s.voltage_mv;
    if (s.fan_curve) g.fan = *s.fan_curve;
    ++g.applies;
  }
  save_state();
  return success();
}

Result<void> MockBackend::reset_to_stock(const GpuId& id) {
  {
    std::lock_guard lk(mu_);
    GPUOC_TRY_ASSIGN(int i, find(id));
    Gpu& g = gpus_[static_cast<std::size_t>(i)];
    advance(g);
    g.core = 0;
    g.mem = 0;
    g.power = g.spec.stock_power_w;
    g.voltage = g.spec.stock_voltage_mv;
    g.fan = FanCurve::driver_auto();
    ++g.resets;
  }
  save_state();
  return success();
}

MockFaults& MockBackend::faults(int index) {
  std::lock_guard lk(mu_);
  return gpus_.at(static_cast<std::size_t>(index)).faults;
}
int MockBackend::hidden_core_max(int index) const {
  std::lock_guard lk(mu_);
  return gpus_.at(static_cast<std::size_t>(index)).spec.true_core_max_mhz;
}
int MockBackend::hidden_mem_max(int index) const {
  std::lock_guard lk(mu_);
  return gpus_.at(static_cast<std::size_t>(index)).spec.true_mem_max_mhz;
}
int MockBackend::effective_core_max(int index) const {
  std::lock_guard lk(mu_);
  return static_cast<int>(std::floor(stable_core_max(gpus_.at(static_cast<std::size_t>(index)))));
}
int MockBackend::apply_count(int index) const {
  std::lock_guard lk(mu_);
  return gpus_.at(static_cast<std::size_t>(index)).applies;
}
int MockBackend::reset_count(int index) const {
  std::lock_guard lk(mu_);
  return gpus_.at(static_cast<std::size_t>(index)).resets;
}
Settings MockBackend::raw_settings(int index) const {
  std::lock_guard lk(mu_);
  const Gpu& g = gpus_.at(static_cast<std::size_t>(index));
  Settings s;
  s.core_offset_mhz = g.core;
  s.mem_offset_mhz = g.mem;
  s.power_limit_w = g.power;
  s.voltage_mv = g.voltage;
  s.fan_curve = g.fan;
  return s;
}
bool MockBackend::load_active(int index) const {
  std::lock_guard lk(mu_);
  return gpus_.at(static_cast<std::size_t>(index)).load;
}
void MockBackend::set_driver_version(std::string v) {
  std::lock_guard lk(mu_);
  opts_.driver_version = std::move(v);
}

void MockBackend::set_load(int index, bool on) {
  std::lock_guard lk(mu_);
  Gpu& g = gpus_.at(static_cast<std::size_t>(index));
  advance(g);
  if (on && !g.load) g.load_since = clock_->now();
  g.load = on;
}

void MockBackend::push_event(int event_id, std::string provider, std::string message) {
  // Caller holds mu_ or not; events_ guarded by mu_ in callers below.
  events_.push_back(DriverEvent{clock_->wall_now(), std::move(provider), event_id, std::move(message)});
}

Result<std::vector<DriverEvent>> MockBackend::events_since(WallTime since) {
  std::lock_guard lk(mu_);
  std::vector<DriverEvent> out;
  for (const auto& e : events_) {
    if (e.time >= since) out.push_back(e);
  }
  return out;
}

IterationResult MockBackend::simulate_iteration(int index, Ms duration) {
  std::lock_guard lk(mu_);
  Gpu& g = gpus_.at(static_cast<std::size_t>(index));
  advance(g);
  const double dt = std::chrono::duration<double>(duration).count();
  IterationResult r;
  r.seconds = dt;
  auto& f = g.faults;
  if (f.crash_next) {
    f.crash_next = false;
    r.status = IterationStatus::Crashed;
    r.detail = "mock: worker crashed (injected)";
    return r;
  }
  if (f.hang_next) {
    f.hang_next = false;
    r.status = IterationStatus::Hang;
    r.detail = "mock: worker hung (injected)";
    return r;
  }
  if (f.exit_nonzero_next) {
    f.exit_nonzero_next = false;
    r.status = IterationStatus::ExitedNonZero;
    r.detail = "mock: worker exited with code 3 (injected)";
    return r;
  }
  if (f.device_lost_next) {
    f.device_lost_next = false;
    push_event(DriverEventIds::kDisplayTdrId, "Display",
               "Display driver nvlddmkm stopped responding and has successfully recovered. (mock)");
    r.status = IterationStatus::DeviceLost;
    r.detail = "mock: device lost (injected)";
    return r;
  }
  if (f.driver_event_next) {
    f.driver_event_next = false;
    push_event(13, "nvlddmkm", "Graphics Exception (mock)");
  }
  if (f.ecc_errors_next > 0) {
    g.ecc += static_cast<std::uint64_t>(f.ecc_errors_next);
    f.ecc_errors_next = 0;
  }

  // Hidden stability model.
  const double core_excess = g.core - stable_core_max(g);
  const double mem_excess = g.mem - g.spec.true_mem_max_mhz;
  double lambda = 0;
  if (core_excess > 0) lambda += 0.1 + 0.02 * core_excess;
  if (mem_excess > 0) lambda += 0.1 + 0.005 * mem_excess;
  if (f.unstable_core_hole && f.unstable_core_hole->contains(g.core)) lambda += 0.5;
  if (f.slow_failure && g.core >= f.slow_failure->min_core &&
      seconds_between(g.load_since, clock_->now()) > f.slow_failure->after_s)
    lambda += 0.05;
  const double p_fail = 1.0 - std::exp(-lambda * dt);
  const double u = uniform01(g.rng);
  const double noise = normal01(g.rng) * g.spec.score_noise_pct / 100.0;

  auto it = std::find(f.transient_core_failures.begin(), f.transient_core_failures.end(), g.core);
  if (it != f.transient_core_failures.end()) {
    f.transient_core_failures.erase(it);
    r.status = IterationStatus::ChecksumMismatch;
    r.detail = "mock: transient checksum mismatch (injected)";
    return r;
  }
  if (u < p_fail) {
    if (core_excess > 75) {
      push_event(DriverEventIds::kDisplayTdrId, "Display",
                 "Display driver nvlddmkm stopped responding and has successfully recovered. (mock)");
      r.status = IterationStatus::DeviceLost;
      r.detail = std::format("mock: TDR at core {:+} MHz", g.core);
      return r;
    }
    if (mem_excess > 0 && g.spec.ecc_supported) ++g.ecc;
    r.status = IterationStatus::ChecksumMismatch;
    r.detail = std::format("mock: checksum mismatch at core {:+} mem {:+}", g.core, g.mem);
    return r;
  }
  r.work = 1000.0 * g.perf * dt * (1.0 + noise);
  return r;
}

WorkloadFactory MockBackend::workload_factory() {
  std::weak_ptr<MockBackend> weak = weak_from_this();
  return [weak](const GpuId& id) -> Result<std::unique_ptr<IWorkload>> {
    auto self = weak.lock();
    if (!self) return make_error(ErrorCode::Unavailable, "mock backend gone");
    int index = -1;
    {
      std::lock_guard lk(self->mu_);
      GPUOC_TRY_ASSIGN(index, self->find(id));
    }
    return std::unique_ptr<IWorkload>(std::make_unique<MockWorkload>(self, index));
  };
}

std::shared_ptr<IEventLogSource> MockBackend::event_log() { return std::make_shared<MockEventLog>(weak_from_this()); }

void MockBackend::save_state() const {
  if (opts_.state_file.empty()) return;
  nlohmann::json j;
  j["gpus"] = nlohmann::json::array();
  {
    std::lock_guard lk(mu_);
    for (const auto& g : gpus_) {
      nlohmann::json fan = nlohmann::json::array();
      for (const auto& p : g.fan.points) fan.push_back({p.temp_c, p.pct});
      j["gpus"].push_back({{"uuid", g.id.uuid}, {"core", g.core}, {"mem", g.mem}, {"power", g.power},
                           {"voltage", g.voltage}, {"fan", fan}});
    }
  }
  if (auto r = platform::write_file_atomic(platform::u8path(opts_.state_file), j.dump()); !r)
    log::error("mock: cannot save state: {}", r.error().message);
}

void MockBackend::load_state() {
  if (opts_.state_file.empty()) return;
  auto text = platform::read_file(platform::u8path(opts_.state_file));
  if (!text) return;
  auto j = nlohmann::json::parse(text.value(), nullptr, false);
  if (j.is_discarded() || !j.contains("gpus") || !j["gpus"].is_array()) return;
  for (const auto& e : j["gpus"]) {
    for (auto& g : gpus_) {
      if (e.value("uuid", std::string()) != g.id.uuid) continue;
      g.core = e.value("core", 0);
      g.mem = e.value("mem", 0);
      g.power = e.value("power", g.spec.stock_power_w);
      g.voltage = e.value("voltage", g.spec.stock_voltage_mv);
      g.fan.points.clear();
      if (e.contains("fan") && e["fan"].is_array()) {
        for (const auto& p : e["fan"]) {
          if (p.is_array() && p.size() == 2) g.fan.points.push_back(FanPoint{p[0].get<int>(), p[1].get<int>()});
        }
      }
    }
  }
}

}  // namespace gpuoc::mock
