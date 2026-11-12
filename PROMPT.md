# Build Prompt: Extreme GPU OC

A fully autonomous GPU auto-overclocker written in C++. Paste everything below the line into a coding agent, or use it as the build spec.

---

You are a senior C++ systems engineer. Build **Extreme GPU OC** (`gpuoc`), a Linux program that runs with no human in the loop once started. It discovers GPUs, benchmarks them, stress-tests them, auto-tunes clock and power settings, applies and validates overclocks, and manages named profiles. Safety limits are mandatory and configuration cannot disable them.

## 0. Working rules

- Build in this order: (1) overclocking API and mock backend, (2) safety layer, (3) benchmark and stress, (4) tuner, (5) controller and daemon, (6) profiles and CLI, (7) real hardware backends. Each step must build and pass its tests before the next one starts.
- No GPU is available in the build environment. Every algorithm must be tested against the mock backend. Real hardware backends must compile and be unit-tested for parsing and error handling. Their behavior on real hardware must be marked "unverified on hardware" in the README.
- Never write to a real GPU during development, testing, or CI. Use the mock backend or dry-run mode.
- Do not stop to ask questions. Where this spec is silent, choose the most conservative option and record it in `docs/DECISIONS.md`.

## 1. Language, build, and layout

- C++20. CMake 3.20 or newer. Compile with `-Wall -Wextra -Wpedantic -Werror`.
- Do not throw across the public API. Use a `Result<T>` type (a value, or an `Error{code, message}`).
- Keep dependencies minimal and pin their versions. Allowed: a small TOML parser, a JSON library, and GoogleTest (via FetchContent).
- Layout:
  ```
  include/gpuoc/     public API headers (the overclocking API)
  src/core/          device model, telemetry, safety limits, last-known-good (LKG) store
  src/backends/      nvidia (NVML via dlopen), amdgpu (sysfs/hwmon), mock
  src/bench/         benchmark runners
  src/stress/        stress runner and stability verdicts
  src/tuner/         auto-tuning search
  src/controller/    autonomous state machine and runtime monitor
  src/profile/       profile store, validation, auto-switching
  src/cli/           gpuoc
  src/daemon/        gpuocd
  tests/             unit and integration tests
  docs/              ARCHITECTURE.md, API.md, SAFETY.md, DECISIONS.md
  examples/          gpuoc.toml, profiles/*.toml
  ```

## 2. Overclocking API (our own)

Write a backend-agnostic C++ API in `namespace gpuoc`. The rest of the program talks to GPUs only through this API, never directly to vendor libraries.

```cpp
struct GpuId { std::string uuid; std::string name; std::string vendor; int index; };

struct Capabilities {
  bool core_offset, mem_offset, power_limit, fan_control, voltage_control, locked_clocks;
  Range core_offset_mhz, mem_offset_mhz;   // min/max as reported by the driver
  Range power_limit_w;
};

struct Telemetry {
  double core_mhz, mem_mhz, temp_c, power_w, fan_pct, util_pct;
  std::optional<double> voltage_mv;
  std::uint64_t ecc_uncorrected;           // 0 if unsupported
  std::chrono::steady_clock::time_point ts;
};

struct Settings {                          // every field optional: nullopt = leave unchanged
  std::optional<int> core_offset_mhz;
  std::optional<int> mem_offset_mhz;
  std::optional<int> power_limit_w;
  std::optional<FanCurve> fan_curve;
  std::optional<int> voltage_mv;           // only when voltage_control is true
};

class IGpuBackend {
 public:
  virtual Result<std::vector<GpuId>> enumerate() = 0;
  virtual Result<Capabilities>       capabilities(const GpuId&) = 0;
  virtual Result<Telemetry>          read_telemetry(const GpuId&) = 0;
  virtual Result<Settings>           read_settings(const GpuId&) = 0;
  virtual Result<void>               apply(const GpuId&, const Settings&) = 0;
  virtual Result<void>               reset_to_stock(const GpuId&) = 0;
  virtual ~IGpuBackend() = default;
};
```

Rules for the API:
- A `Gpu` facade wraps a backend and enforces capability checks, clamping to driver-reported ranges, and rejection of unsupported fields. Never silently ignore a request.
- `apply()` reads settings back and returns an error if they differ from the request beyond tolerance.
- A `StockGuard` RAII object snapshots stock settings on construction and restores them on destruction, including on error paths.
- An optional local control socket (`/run/gpuoc/gpuoc.sock`, JSON-RPC 2.0) exposes the same operations to other tools. Read-only methods are open to non-root users. Write methods require root.
- Document the API in `docs/API.md` with a short example.

## 3. Hardware backends

- **NVIDIA:** load `libnvidia-ml.so` with `dlopen` at runtime. If it is missing, the backend reports itself unavailable and the program keeps running. Use NVML for enumeration, telemetry, power limits, clock offsets where the driver exposes them, and fan control where supported. Report locked-clock support separately. Do not emulate offsets with locked clocks.
- **AMD:** use amdgpu sysfs and hwmon (`pp_od_clk_voltage`, `power1_cap`, `pwm1*`, `pp_power_profile_mode`). Confirm overdrive is enabled before writing. Parse the OD table defensively.
- **Mock:** a simulated GPU with a hidden stability model. It has true maximum stable core and memory offsets, an error rate that rises past those thresholds, and thermal and power models. It is deterministic for a given seed. All tests and dry-run use it.
- **Intel and Windows:** out of scope for v1. The interface must not block adding them later.
- **Dry-run** (`--dry-run`): every write is logged with the value it would have set, and nothing is applied.

## 4. Benchmarking

- `IBenchmark` returns a score, its unit, and frame or time statistics.
- Built-in workload: a deterministic compute kernel (Vulkan compute preferred, OpenCL acceptable) whose output matches a known reference checksum. Its throughput or wall time is the score.
- External workload: any command, with the score parsed from stdout by a regex set in the config.
- Protocol: 30 s warm-up, then 3 measured runs. Report the median and coefficient of variation (CV). Mark the result invalid if CV exceeds 3%, or if the GPU hit a limit during the run.

## 5. Stress testing and stability verdicts

- Run the deterministic workload for a set duration: short (120 s default) for each search point, long (1800 s default) for final validation.
- Each run returns one verdict: `Stable`, `Unstable(reason)`, or `Inconclusive(reason)`. Verdicts are:
  - **Unstable** if the output checksum differs from the reference (silent corruption), the process crashes or exits non-zero, the GPU is lost or hangs past a timeout, the driver resets, uncorrected ECC errors increase (where supported), the kernel log shows GPU error messages (NVIDIA Xid, amdgpu reset; best effort via journal or dmesg), or temperature or power exceeds a configured limit.
  - **Inconclusive** if telemetry is missing or the program itself interrupted the run.
  - Inconclusive never counts as stable.

## 6. Auto-tuning

Tune each GPU per profile objective. Objectives: `max_performance`, `max_perf_per_watt` (default), and `balanced`.

Search in stages:
1. **Baseline:** measure stock score, power, and temperature.
2. **Memory offset:** exponential ramp (step × 2^n) to the first unstable point, then binary search for the last stable point. Default step: 50 MHz.
3. **Core offset:** same method, run with the validated memory offset. Default step: 15 MHz.
4. **Power limit and fan:** raise the power limit in steps only if the objective benefits and temperature stays under target. Change the fan curve only if fan control is available.
5. **Margin:** back the final core and memory offsets off by one step (configurable).
6. **Validation:** long stress run. If it fails, back off one more step and validate again, up to a retry limit.

Additional rules:
- Write each trial to an append-only JSON-lines log before starting the next one, so tuning can resume after a crash or reboot.
- If a result is non-monotonic (stable above an unstable point), mark that region suspect and re-test it before accepting anything.
- Cap total tuning time (4 h default). When the cap is reached, keep the best validated result so far.
- Re-tune when the GPU UUID changes, the driver version changes, the last validation is older than 14 days (configurable), or the runtime monitor records repeated instability.

## 7. Autonomous controller and runtime monitor

State machine: `Discover → Baseline → Tune → Validate → Commit → Run`. Any state can enter `SafeRevert` on a safety violation. `SafeRevert` moves to `Idle` and retries after a cooldown.

- **Commit** writes the validated profile to the LKG store. Only validated settings may enter the LKG store.
- **Run** applies the LKG profile and polls telemetry every second (configurable). Use hysteresis: if temperature is above target, step the core offset down one step. When temperature has stayed more than 5 °C below target for 60 s, step back up. Never go above the validated value.
- A runtime violation (temperature over max, power over max, GPU error in the kernel log, or ECC error) reverts to stock immediately, logs the event, and disables auto-overclock for that GPU for a cooldown (30 min default). After three violations in 24 h, disable auto-overclock for that GPU until it is manually re-enabled.
- `gpuocd` runs as a systemd service with `WatchdogSec` and a heartbeat. If the heartbeat stops, systemd restarts the daemon, and the restart path reverts to stock.

## 8. Safety (mandatory)

- Autonomous tuning runs only when both `autotune.enabled = true` and `safety.acknowledge_risk = true` are set in config. Without both, the program stays in monitor-only mode.
- Config can tighten limits but never loosen them past hard limits in code. The effective limit is always `min(config, driver_range, hard_ceiling)`.
- Hard ceilings: max temperature 90 °C; default revert threshold 83 °C; target 75 °C; power limit defaults to stock TDP. A higher power limit needs `safety.allow_power_above_stock = true` and is still capped by the driver maximum.
- Voltage: never change it unless the backend exposes voltage control AND `safety.allow_voltage = true`.
- Fans: never set below 30% while GPU temperature is above 60 °C. If a fan curve cannot be read or applied, restore driver-automatic fan control.
- Every write path ends in a `StockGuard` that restores stock on any exit: normal completion, error, signal (SIGINT and SIGTERM set an atomic flag that the loop checks), or watchdog timeout.
- Boot behavior: after a reboot the GPU is at stock. An optional systemd oneshot unit may re-apply the LKG profile, but only if it was validated and has not been revoked. Unvalidated settings never persist across a reboot.
- Never implement: VBIOS or firmware flashing, fuse or lock writes, BIOS changes, or disabling vendor safety features.

## 9. Profiles

- A profile is a TOML file with: name, objective, target settings (core and memory offsets, power limit, fan curve, temperature target), tuning options, and match rules.
- Locations: `/etc/gpuoc/profiles.d/` (system) and `~/.config/gpuoc/profiles.d/` (user). Per-GPU overrides are keyed by UUID.
- Built-in profiles: `stock` (always present, cannot be deleted), `quiet`, `balanced`, and `max`. Built-ins are validated against the same limits as user profiles.
- Auto-switching: a process watcher reads `/proc`. When a process matches a profile's rule (executable name glob or path), apply that profile. When the process exits, return to the default profile. Rules are checked in order, and the first match wins.
- Applying a profile reads telemetry back and verifies the settings. A mismatch is an error, never a silent partial apply.

## 10. CLI and daemon

`gpuoc` subcommands:
- `list`, `status [--json]`, `doctor` (checks root, driver libraries, sysfs paths, overdrive enablement, kernel log access)
- `set --gpu <uuid|index> [--core +N] [--mem +N] [--power W]` (limits enforced)
- `reset [--gpu ...]`
- `bench [--gpu ...]`, `stress --duration <s>`
- `tune [--objective ...] [--budget <s>]`, `tune status`, `tune abort`
- `profile list|show|create|edit|delete|apply|activate`

`gpuocd` daemon: `--config <path>`, `--dry-run`.

Exit codes: 0 success, 1 usage error, 2 unsupported, 3 safety refusal, 4 hardware error. Output is human-readable by default. `--json` gives machine-readable output.

## 11. Testing and acceptance

Required tests (GoogleTest):
- **Clamping:** requests outside the driver range are clamped or rejected, never passed through.
- **Limit precedence:** config values beyond the driver range or hard ceilings are clamped.
- **Search:** against the mock, the tuner finds the true max stable offset to within one step, across at least 20 random seeds.
- **Non-monotonic results** are detected and re-tested.
- **Verdicts:** checksum mismatch, crash, reset, ECC increase, and temperature violation each map to `Unstable`. Missing telemetry maps to `Inconclusive`.
- **Runtime violation** reverts to stock within one poll interval.
- **Crash recovery:** kill the process during a trial. On restart it reverts to stock and resumes from the tuning log.
- **Profile parsing:** invalid TOML and out-of-range values are rejected with clear errors.
- **Auto-switch:** a matching process applies its profile, and its exit reverts it.
- **End-to-end:** a full autonomous run on the mock, with a simulated clock, completes, commits an LKG profile, and passes validation.
- **Hardware tests** are gated behind `GPUOC_HW_TESTS=1` and skipped otherwise.

CI: a GitHub Actions workflow on Ubuntu that builds and runs all tests without a GPU.

Docs: a `README.md` covering install, quick start, a safety warning, and a list of what is verified on hardware and what is not. Plus the four files in `docs/`.

## 12. Defaults (use these; do not ask)

- Linux only in v1.
- Config format: TOML, at `/etc/gpuoc/gpuoc.toml`, overridable with `--config`.
- Core step 15 MHz, memory step 50 MHz, warm-up 30 s, 3 runs, short stress 120 s, long stress 1800 s.
- Max temperature 83 °C, target 75 °C, tuning budget 4 h, re-validate every 14 days.

## 13. Definition of done

- `cmake -S . -B build && cmake --build build && ctest --test-dir build` passes on a clean checkout with no GPU.
- Every rule in section 8 is enforced by at least one test.
- `docs/DECISIONS.md` lists every choice made where this spec was silent.
- The final report lists what was built, what was tested, what is unverified on real hardware, and any spec items not completed, with reasons.
