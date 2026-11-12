# Build Prompt: Extreme GPU OC

A fully autonomous NVIDIA GPU auto-overclocker for Windows, written in C++. Paste everything below the line into a coding agent, or use it as the build spec.

---

You are a senior C++ systems engineer. Build **Extreme GPU OC** (`gpuoc`), a Windows program for NVIDIA GPUs that runs with no human in the loop once started. It discovers NVIDIA GPUs, benchmarks them, stress-tests them, auto-tunes clock, voltage, and power settings, applies and validates overclocks, and manages named profiles. Safety limits are mandatory, and configuration cannot disable them.

## 0. Scope

- **Platform:** Windows 10 and Windows 11, x64 only. The daemon runs as a Windows service with rights to write GPU settings.
- **Vendor:** NVIDIA only. Do not write AMD or Intel code. Keep the `IGpuBackend` interface so the mock backend can implement it, but ship no other vendor backends.
- **Feature set for v1:** everything in this spec, including voltage control. Voltage control is opt-in and gated (see section 8).
- **Autonomy:** the program runs unattended, including autonomous tuning on real hardware, once the gates in section 8 are satisfied.

## 1. Working rules

- Build in this order: (1) overclocking API and mock backend, (2) safety layer, (3) benchmark and stress, (4) tuner, (5) controller and service, (6) profiles and CLI, (7) NVIDIA hardware backends (NVML and NVAPI). Each step must build and pass its tests before the next one starts.
- The build environment may have no NVIDIA GPU and may not run Windows. Every algorithm must be tested against the mock backend. Hardware backends must compile and be unit-tested for parsing, error handling, and capability checks. Their behavior on real hardware must be listed as "unverified on hardware" in the README.
- Never write to a real GPU during development, testing, or CI. Use the mock backend or dry-run mode.
- Do not stop to ask questions. Where this spec is silent, choose the most conservative option and record it in `docs/DECISIONS.md`.

## 2. Language, build, and layout

- C++20. CMake 3.20 or newer. Build with MSVC (Visual Studio 2022 or later) or clang-cl.
- Warnings as errors: `/W4 /WX` on MSVC, `-Wall -Wextra -Wpedantic -Werror` on clang-cl.
- Use the Unicode Windows API (`UNICODE` and `_UNICODE` defined). Use `std::wstring` at Windows API boundaries and UTF-8 internally.
- Do not throw across the public API. Use `Result<T>` (a value, or an `Error{code, message}`). Convert `HRESULT`, NVML, and NVAPI status codes at the backend boundary.
- Dependencies, all pinned: a TOML parser, a JSON library, GoogleTest (via FetchContent), and the NVML and NVAPI headers from NVIDIA's official sources.
- Do not link NVML or NVAPI statically. Load `nvml.dll` and `nvapi64.dll` at runtime with `LoadLibraryW` and `GetProcAddress`. If either is missing, that backend reports itself unavailable and the program keeps running.
- Layout:
  ```
  include/gpuoc/     public API headers (the overclocking API)
  src/core/          device model, telemetry, safety limits, last-known-good (LKG) store
  src/backends/      nvidia (NVML + NVAPI), mock
  src/bench/         benchmark runners
  src/stress/        stress runner, stability verdicts, Windows Event Log checks
  src/tuner/         auto-tuning search
  src/controller/    autonomous state machine and runtime monitor
  src/profile/       profile store, validation, process-based auto-switching
  src/platform/      service host, named pipe, elevation check, paths
  src/cli/           gpuoc.exe
  src/service/       gpuocd.exe (Windows service)
  tests/             unit and integration tests
  docs/              ARCHITECTURE.md, API.md, SAFETY.md, WINDOWS.md, DECISIONS.md
  examples/          gpuoc.toml, profiles/*.toml
  ```

## 3. Overclocking API (our own)

Write a backend-agnostic C++ API in `namespace gpuoc`. The rest of the program talks to GPUs only through this API, never directly to NVML or NVAPI.

```cpp
struct GpuId { std::string uuid; std::string name; int index; };

struct Capabilities {
  bool core_offset, mem_offset, power_limit, fan_control, voltage_control, locked_clocks;
  Range core_offset_mhz, mem_offset_mhz;   // min/max as reported by the driver
  Range power_limit_w;
  Range voltage_mv;                        // only meaningful if voltage_control
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
- Local control uses a named pipe, `\\.\pipe\gpuoc`, carrying JSON-RPC 2.0. Pipe ACL: Users may read status; only Administrators may send write methods.
- Document the API in `docs/API.md` with a short example.

## 4. NVIDIA backends

- **NVML (`nvml.dll`):** enumeration (UUID and index), telemetry (core and memory clocks, temperature, power, fan, utilization, ECC counts), power limits, and fan control where supported. Use the driver-reported min and max for each setting. Report unsupported features as unsupported. Do not guess.
- **NVAPI (`nvapi64.dll`):** core and memory clock offsets through the P-state 2.0 interface, and voltage control where the GPU and driver expose it. Check each function's availability at runtime. A missing function means that capability is unsupported. Do not assume a call exists.
- NVML is the source of truth for telemetry and limits. NVAPI is used for offsets and voltage.
- Report locked-clock support separately. Do not emulate offsets with locked clocks.
- **Mock:** a simulated GPU with a hidden stability model. It has true maximum stable core and memory offsets, an error rate that rises past those thresholds, and thermal, power, and voltage models. It is deterministic for a given seed. Tests and dry-run use it.
- **Dry-run** (`--dry-run`): log every write with the value it would have set. Apply nothing.

## 5. Benchmarking

- `IBenchmark` returns a score, its unit, and frame or time statistics.
- Built-in workload: a deterministic compute kernel using Vulkan compute, which needs only the installed NVIDIA driver and no CUDA toolkit. Its output must match a known reference checksum. Its throughput or wall time is the score.
- External workload: any command, with the score parsed from stdout by a regex set in the config. Run it through `CreateProcessW` inside a job object, so the child is killed if the program exits.
- Protocol: 30 s warm-up, then 3 measured runs. Report the median and coefficient of variation (CV). Mark the result invalid if CV exceeds 3%, or if the GPU hit a limit during the run.

## 6. Stress testing and stability verdicts

- Run the deterministic workload for a set duration: short (120 s default) for each search point, long (1800 s default) for final validation.
- Each run returns one verdict: `Stable`, `Unstable(reason)`, or `Inconclusive(reason)`.
  - **Unstable** if the output checksum differs from the reference (silent corruption); the process crashes or exits non-zero; the GPU is lost or hangs past a timeout; the display driver resets; uncorrected ECC errors increase (where supported); the Windows Event Log shows NVIDIA driver failures (TDR and `nvlddmkm` events, best effort; verify the event IDs on the target system); or temperature or power exceeds a configured limit.
  - **Inconclusive** if telemetry is missing or the program itself interrupted the run.
  - Inconclusive never counts as stable.

## 7. Auto-tuning

Tune each GPU per profile objective. Objectives: `max_performance`, `max_perf_per_watt` (default), and `balanced`.

Search in stages:
1. **Baseline:** measure stock score, power, and temperature.
2. **Memory offset:** exponential ramp (step × 2^n) to the first unstable point, then binary search for the last stable point. Default step: 50 MHz.
3. **Core offset:** same method, run with the validated memory offset. Default step: 15 MHz.
4. **Voltage** (only when allowed by section 8): at the core offset that fails, raise voltage one driver-reported step at a time to test whether the extra voltage extends the stable core offset. Stop when the voltage ceiling is reached, the power limit is reached, or the gain is less than one core step.
5. **Power limit and fan:** raise the power limit in steps only if the objective benefits and temperature stays under target. Change the fan curve only if fan control is available.
6. **Margin:** back the final core and memory offsets off by one step (configurable).
7. **Validation:** long stress run. If it fails, back off one more step and validate again, up to a retry limit.

Additional rules:
- Write each trial to an append-only JSON-lines log before starting the next one, so tuning can resume after a crash or reboot.
- If a result is non-monotonic (stable above an unstable point), mark that region suspect and re-test it before accepting anything.
- Cap total tuning time (4 h default). When the cap is reached, keep the best validated result so far.
- Re-tune when the GPU UUID changes, the NVIDIA driver version changes, the last validation is older than 14 days (configurable), or the runtime monitor records repeated instability.

## 8. Autonomous controller and runtime monitor

State machine: `Discover → Baseline → Tune → Validate → Commit → Run`. Any state can enter `SafeRevert` on a safety violation. `SafeRevert` moves to `Idle` and retries after a cooldown.

- **Commit** writes the validated profile to the LKG store (`lkg.json`). Only validated settings may enter it.
- **Run** applies the LKG profile and polls telemetry every second (configurable). Use hysteresis: if temperature is above target, step the core offset down one step. When temperature has stayed more than 5 °C below target for 60 s, step back up. Never go above the validated value.
- A runtime violation (temperature over max, power over max, a driver failure event, or an ECC error) reverts to stock immediately, logs the event, and disables auto-overclock for that GPU for a cooldown (30 min default). After three violations in 24 h, disable auto-overclock for that GPU until it is manually re-enabled.
- `gpuocd.exe` runs as a Windows service with a watchdog thread. If the watchdog stops being fed, the service reverts to stock and exits, and the Service Control Manager restarts it under its failure-action policy. The restart path reverts to stock before any tuning resumes.

## 9. Safety (mandatory)

Gates:
- Autonomous tuning runs only when both `autotune.enabled = true` and `safety.acknowledge_risk = true` are set in config. Without both, the program stays in monitor-only mode.
- Voltage changes require the backend to expose voltage control AND `safety.allow_voltage = true`.
- A power limit above stock TDP requires `safety.allow_power_above_stock = true`.

Limits:
- Config can tighten limits but never loosen them past hard limits in code. The effective limit is always `min(config, driver_range, hard_ceiling)`.
- Hard ceilings: max temperature 90 °C. Default revert threshold 83 °C. Default target 75 °C. Power limit defaults to stock TDP and is capped by the driver maximum.
- Voltage: the ceiling is the driver-reported maximum minus one step. Config cannot raise it.
- Fans: never set below 30% while GPU temperature is above 60 °C. If a fan curve cannot be read or applied, restore driver-automatic fan control.

Stock restore:
- Every write path ends in a `StockGuard` that restores stock on any exit: normal completion, error, Ctrl+C or Ctrl+Break (`SetConsoleCtrlHandler`), service stop, service preshutdown (`SERVICE_CONTROL_PRESHUTDOWN`), or watchdog timeout.
- Shutdown and stop handlers run synchronously and must finish before the service reports stopped.

Boot behavior:
- After a reboot the GPU is at stock. The service may re-apply the LKG profile on start, but only if it was validated and has not been revoked.
- Unvalidated settings never persist across a reboot.

Never implement:
- VBIOS or firmware flashing, fuse or lock writes, BIOS changes.
- Disabling vendor or OS safety features. This includes editing TDR registry keys (`TdrDelay`, `TdrLevel`).

## 10. Profiles

- A profile is a TOML file with: name, objective, target settings (core and memory offsets, power limit, fan curve, temperature target, voltage if allowed), tuning options, and match rules.
- Locations:
  - System profiles: `%ProgramData%\ExtremeGpuOc\profiles.d\`
  - User profiles: `%APPDATA%\ExtremeGpuOc\profiles.d\`
  - Per-GPU overrides are keyed by GPU UUID.
- Built-in profiles: `stock` (always present, cannot be deleted), `quiet`, `balanced`, and `max`. Built-ins are validated against the same limits as user profiles.
- Auto-switching: poll the process list every 2 s with `CreateToolhelp32Snapshot`, and get executable paths with `QueryFullProcessImageNameW`. When a process matches a profile's rule (executable name glob or full path), apply that profile. When the process exits, return to the default profile. Rules are checked in order, and the first match wins.
- Applying a profile reads telemetry back and verifies the settings. A mismatch is an error, never a silent partial apply.

## 11. CLI and service

`gpuoc.exe` subcommands:
- `list`, `status [--json]`, `doctor`
- `set --gpu <uuid|index> [--core +N] [--mem +N] [--power W] [--voltage mV]` (limits enforced)
- `reset [--gpu ...]`
- `bench [--gpu ...]`, `stress --duration <s>`
- `tune [--objective ...] [--budget <s>]`, `tune status`, `tune abort`
- `profile list|show|create|edit|delete|apply|activate`
- `service install|uninstall|start|stop|status`

`doctor` checks: elevation, presence and version of `nvml.dll` and `nvapi64.dll`, NVIDIA driver version, which NVAPI functions are available, and whether the Windows Event Log can be read.

`gpuocd.exe`:
- Windows service named `ExtremeGpuOc`, start type automatic (delayed), running as LocalSystem.
- Failure actions configured with the SCM to restart the service.
- `--console` runs the same code without the SCM, for debugging.
- Options: `--config <path>`, `--dry-run`.

Exit codes: 0 success, 1 usage error, 2 unsupported, 3 safety refusal, 4 hardware error, 5 insufficient privileges. Output is human-readable by default. `--json` gives machine-readable output.

## 12. Testing and acceptance

Required tests (GoogleTest):
- **Clamping:** requests outside the driver range are clamped or rejected, never passed through.
- **Limit precedence:** config values beyond the driver range or hard ceilings are clamped. This includes voltage.
- **Gates:** voltage and autotune changes are refused unless their gates are set.
- **Search:** against the mock, the tuner finds the true max stable offset to within one step, across at least 20 random seeds.
- **Non-monotonic results** are detected and re-tested.
- **Verdicts:** checksum mismatch, crash, driver reset, ECC increase, and temperature violation each map to `Unstable`. Missing telemetry maps to `Inconclusive`.
- **Runtime violation** reverts to stock within one poll interval.
- **Crash recovery:** kill the process during a trial. On restart it reverts to stock and resumes from the tuning log.
- **Service control:** stop and preshutdown both trigger stock restore before reporting stopped.
- **Watchdog:** a stalled watchdog triggers revert and exit.
- **Profile parsing:** invalid TOML and out-of-range values are rejected with clear errors.
- **Auto-switch:** a matching process applies its profile, and its exit reverts it.
- **Paths:** `%ProgramData%` and `%APPDATA%` are resolved correctly, with environment overrides in tests.
- **End-to-end:** a full autonomous run on the mock, with a simulated clock, completes, commits an LKG profile, and passes validation.
- **Hardware tests** are gated behind `GPUOC_HW_TESTS=1` and skipped otherwise.

CI: a GitHub Actions workflow on `windows-latest` that builds with MSVC and runs all tests that do not need hardware.

Docs:
- `README.md`: install, quick start, a safety warning, and a list of what is verified on hardware and what is not.
- `docs/WINDOWS.md`: service install, elevation, and file locations.
- The other files listed in section 2.

## 13. Defaults (use these; do not ask)

- Windows 10 and 11, x64, NVIDIA only.
- Config format: TOML, at `%ProgramData%\ExtremeGpuOc\gpuoc.toml`, overridable with `--config`.
- LKG store: `%ProgramData%\ExtremeGpuOc\lkg.json`. Logs: `%ProgramData%\ExtremeGpuOc\logs\`.
- Core step 15 MHz, memory step 50 MHz, voltage step as reported by the driver, warm-up 30 s, 3 runs, short stress 120 s, long stress 1800 s.
- Max temperature 83 °C, target 75 °C, tuning budget 4 h, re-validate every 14 days.

## 14. Definition of done

- On a clean `windows-latest` checkout with no GPU: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` and `cmake --build build --config Release` succeed, and `ctest --test-dir build -C Release` passes.
- Every rule in section 9 is enforced by at least one test.
- `docs/DECISIONS.md` lists every choice made where this spec was silent.
- The final report lists what was built, what was tested, what is unverified on real hardware, and any spec items not completed, with reasons.
