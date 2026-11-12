// Simulated NVIDIA GPU with a hidden stability model and simple thermal,
// power, and voltage physics. Deterministic for a given seed and call
// sequence. Used by tests, --backend mock, and dry-run without hardware.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "bench/workload.hpp"
#include "gpuoc/backend.hpp"
#include "gpuoc/clock.hpp"
#include "stress/event_log.hpp"

namespace gpuoc::mock {

struct MockGpuSpec {
  std::string name = "NVIDIA GeForce RTX 4080 (mock)";
  Range core_offset_mhz{-500, 1000};
  Range mem_offset_mhz{-1000, 3000};
  Range power_limit_w{150, 400};
  int stock_power_w = 320;
  bool voltage_control = true;
  Range voltage_mv{700, 1100};
  int stock_voltage_mv = 1000;
  int voltage_step_mv = 10;
  bool fan_control = true;
  Range fan_pct{30, 100};
  bool ecc_supported = false;
  bool locked_clocks = true;

  // Hidden stability model. Negative = derive from the seed.
  int true_core_max_mhz = -1;
  int true_mem_max_mhz = -1;
  double volt_gain_mhz_per_mv = 0.8;  // extra stable core MHz per mV above stock
  int volt_gain_cap_mhz = 60;

  // Physics.
  double base_core_mhz = 2600;
  double base_mem_mhz = 11200;
  double idle_power_w = 25;
  double load_power_w = 260;  // at stock clocks and voltage, 100% utilization
  double ambient_c = 25;
  double thermal_tau_s = 25;
  double r_fan0 = 0.20;       // C/W at 0% fan
  double r_fan100 = 0.10;     // C/W at 100% fan
  double throttle_temp_c = 87;
  double score_noise_pct = 0.2;
};

struct MockFaults {
  bool readback_mismatch = false;   // read_settings reports a different core offset
  bool fail_apply = false;          // apply() returns a hardware error
  bool fail_fan_apply = false;      // manual fan writes fail
  bool telemetry_missing = false;   // read_telemetry fails
  std::optional<Range> unstable_core_hole;  // always unstable inside this band
  std::vector<int> transient_core_failures; // fail once at these core offsets
  bool device_lost_next = false;    // next iteration: device lost + TDR event
  bool crash_next = false;          // next iteration: worker crash
  bool hang_next = false;           // next iteration: hang
  bool exit_nonzero_next = false;   // next iteration: worker exits non-zero
  bool driver_event_next = false;   // next iteration: nvlddmkm event only
  int ecc_errors_next = 0;          // next iteration: uncorrected ECC errors
  double temp_offset_c = 0;         // added to the simulated temperature
  double extra_power_w = 0;         // added to the simulated power
  // Long-run instability: at core >= min_core, errors start after the GPU has
  // been loaded continuously for after_s seconds (short stress passes,
  // long validation fails).
  struct SlowFailure {
    int min_core = 0;
    double after_s = 0;
  };
  std::optional<SlowFailure> slow_failure;
};

struct MockOptions {
  int gpu_count = 1;
  std::uint64_t seed = 1;
  std::shared_ptr<IClock> clock;   // default: SystemClock
  std::vector<MockGpuSpec> specs;  // per GPU; missing entries use defaults
  std::string state_file;          // persist "hardware" registers here (crash tests)
  std::string driver_version = "566.36 (mock)";
};

class MockBackend final : public IGpuBackend, public std::enable_shared_from_this<MockBackend> {
 public:
  explicit MockBackend(MockOptions opts);

  Result<std::vector<GpuId>> enumerate() override;
  Result<Capabilities> capabilities(const GpuId&) override;
  Result<Telemetry> read_telemetry(const GpuId&) override;
  Result<Settings> read_settings(const GpuId&) override;
  Result<void> apply(const GpuId&, const Settings&) override;
  Result<void> reset_to_stock(const GpuId&) override;
  [[nodiscard]] const char* name() const noexcept override { return "mock"; }

  // Test and simulation hooks.
  MockFaults& faults(int index);
  [[nodiscard]] int hidden_core_max(int index) const;
  [[nodiscard]] int hidden_mem_max(int index) const;
  [[nodiscard]] int effective_core_max(int index) const;  // includes voltage gain
  [[nodiscard]] int apply_count(int index) const;
  [[nodiscard]] int reset_count(int index) const;
  [[nodiscard]] Settings raw_settings(int index) const;  // what the "hardware" holds
  [[nodiscard]] bool load_active(int index) const;
  void set_driver_version(std::string v);
  [[nodiscard]] IClock& clock() const { return *clock_; }
  [[nodiscard]] std::uint64_t seed() const { return opts_.seed; }

  // Workload and event log bound to this mock.
  WorkloadFactory workload_factory();
  std::shared_ptr<IEventLogSource> event_log();

  // Used by MockWorkload.
  void set_load(int index, bool on);
  IterationResult simulate_iteration(int index, Ms duration);
  void push_event(int event_id, std::string provider, std::string message);
  Result<std::vector<DriverEvent>> events_since(WallTime since);

 private:
  struct Gpu {
    MockGpuSpec spec;
    GpuId id;
    int core = 0, mem = 0, power = 0, voltage = 0;
    FanCurve fan;
    double temp = 0;
    double fan_pct = 0;
    double last_power = 0;
    double perf = 1.0;
    std::uint32_t flags = 0;
    bool load = false;
    SteadyTime load_since{};
    std::uint64_t ecc = 0;
    SteadyTime last{};
    std::mt19937_64 rng;
    MockFaults faults;
    int applies = 0, resets = 0;
  };

  Result<int> find(const GpuId& id) const;
  void advance(Gpu& g);
  void step_physics(Gpu& g, double dt);
  double stable_core_max(const Gpu& g) const;
  void save_state() const;
  void load_state();

  MockOptions opts_;
  std::shared_ptr<IClock> clock_;
  mutable std::mutex mu_;
  std::vector<Gpu> gpus_;
  std::vector<DriverEvent> events_;
};

// Event-log source that reports nothing (no Windows Event Log available).
class NullEventLog final : public IEventLogSource {
 public:
  [[nodiscard]] bool available() const override { return false; }
  Result<std::vector<DriverEvent>> driver_events_since(WallTime) override {
    return std::vector<DriverEvent>{};
  }
};

}  // namespace gpuoc::mock
