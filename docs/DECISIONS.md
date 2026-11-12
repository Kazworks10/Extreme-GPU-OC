# Decisions

Choices made where the specification was silent or ambiguous. Each one takes the more conservative option unless it says otherwise.

## Build and platform

- **D-001 Non-Windows host build.** The code also builds on Linux (GCC or Clang) with POSIX fallbacks, or stubs that return `Unavailable`, for the service, Event Log, NVAPI, and pipe. This exists for fast development and so the CI Linux job can run the Vulkan kernel on Mesa lavapipe. It is not a supported product target.
- **D-002 MinGW-w64 cross toolchain** (`cmake/toolchains/mingw-w64-x86_64.cmake`) is for development only: it compile-checks the Windows code and runs the tests under Wine. The real NVAPI adapter is built only with MSVC or clang-cl, because the NVAPI headers depend on MSVC's SAL headers. MinGW builds report NVAPI as unavailable.
- **D-003 clang-cl warnings.** Under the cl driver, `-Wall` means `-Weverything`, so the specified `-Wall -Wextra -Wpedantic -Werror` are passed as `/clang:-Wall` and so on.
- **D-004 GCC** (development hosts only) adds `-Wno-maybe-uninitialized`, because of false positives on `std::variant` and `std::optional` at `-O2` and above.
- **D-005 Extra dependency.** Vulkan-Headers 1.3.275, pinned by commit, is needed for the Vulkan compute kernel. The spec's dependency list did not mention it. `vulkan-1.dll` itself is loaded at runtime.
- **D-006 Dependency fetching.** GitHub dependencies are pinned by commit hash (FetchContent `GIT_TAG <sha>`). The NVML header comes from NVIDIA's `cuda_nvml_dev-windows-x86_64-12.6.77` archive, pinned by SHA-256. The NVAPI headers come from `github.com/NVIDIA/nvapi` at commit `51e5aae4`. No `.lib` file is linked.
- **D-007 CI images.** `windows-latest` no longer ships Visual Studio 2022, so that job lets CMake pick the installed Visual Studio (still MSVC). A separate `windows-2022` job runs the exact definition-of-done commands with `-G "Visual Studio 17 2022"`. A clang-cl job and a Linux job also run.

## API and model

- **D-010 Extended device model.** `Capabilities` adds `stock_power_limit_w`, `stock_voltage_mv`, `voltage_step_mv`, `fan_pct`, `ecc_supported`, and `driver_version`. `Telemetry` adds `limit_flags` (from NVML clock-event reasons). An empty `FanCurve` means driver-automatic. `IGpuBackend` adds `name()`.
- **D-011 Clamp or reject.** Every user-facing path (CLI, RPC, profiles) uses `ClampMode::Reject`: a value outside the limits is an error, never silently changed. `ClampMode::Clamp` exists, but it reports every adjustment.
- **D-012 Read-back tolerance:** offsets ±1 MHz, power ±1 W, voltage ±max(1, step/2) mV; fan curves must match exactly (they are stored in software). A mismatch resets the GPU to stock, because the read-back cannot be trusted for a rollback.
- **D-013 Stock settings** are zero offsets, the driver default power limit, driver-automatic fans, and the nominal voltage; only supported fields are set. `StockGuard` restores them with `reset_to_stock()`, and if that fails, by applying the snapshot.
- **D-014 Exit codes.** NotFound, InvalidArgument and ParseError → 1. Unsupported and Unavailable → 2. SafetyRefusal and OutOfRange → 3. PermissionDenied → 5. Everything else (hardware, I/O, timeout, busy, aborted, a non-stable stress verdict) → 4.

## Safety

- **D-020 `NvAPI_GPU_SetPstates20` is not in the public NVAPI SDK.** It is resolved through `nvapi_QueryInterface(0x0F4DAE6B)`, the interface ID used by third-party tools. If it does not resolve, clock offsets and voltage are reported unsupported; no other mechanism (such as locked clocks or NVML clock offsets) is substituted. Public functions are resolved by the IDs in the official `nvapi_interface.h`.
- **D-021 Voltage step.** NVAPI reports no voltage step, so the policy uses 6 mV (≈ the 6.25 mV VID granularity common on NVIDIA). The ceiling is then the driver maximum minus 6 mV. A smaller step is the conservative choice.
- **D-022 Voltage** is the absolute P0 base voltage in mV: nominal + delta from `NV_GPU_PERF_PSTATES20_INFO` (core voltage domain). It counts as supported only when the delta is editable and its range is non-empty.
- **D-023 Memory offset units** are whatever P-state 2.0 reports for `NVAPI_GPU_PUBLIC_CLOCK_MEMORY` (kHz ÷ 1000). This is unverified on hardware and may differ from the "effective" MHz that other tools show.
- **D-024 Offsets** have no hard ceiling beyond the driver range. `safety.max_core_offset_mhz` and `safety.max_mem_offset_mhz` can only lower it. Negative offsets are allowed within the driver range.
- **D-025 `target_temp_c` must be below `max_temp_c`.** Otherwise it becomes `min(75, max − 5)`. `max_temp_c` may be set anywhere up to the 90 °C ceiling.
- **D-026 Power violations** use a 5% sampling tolerance over the effective maximum, because NVML power readings overshoot briefly.
- **D-027 The fan floor** is enforced on every fan write we make (curve validation, an extra floor check in the NVIDIA backend). Driver-automatic fan behaviour is never treated as a violation.
- **D-028 `acknowledge_risk` gates every autonomous write**, not only tuning: re-applying the LKG, automatic profile switching, and the default profile at boot. A manual `gpuoc tune` needs it too, since tuning writes repeatedly and unattended.
- **D-029 Autonomous profile application** is limited to validated (auto) or conservative settings: no clock, power, or voltage above stock. Fixed overclock profiles apply only on an explicit `gpuoc profile apply`. This keeps unvalidated settings from coming back after a reboot through a match rule.
- **D-030 Violations at stock** (nothing applied) are logged but not counted. There is nothing to revert, and stock behaviour is the driver's responsibility.

## Benchmark and stress

- **D-040 Kernel.** Integer-only (LCG, xorshift, and random gathers) so it is bit-exact on any conforming GPU. Defaults: 2²² elements, 64 iterations, 16 passes per submission, ≈ 4.3 G kernel operations per submission. The default reference checksum `0x5e295ebc513b2968` is a constant (the CPU reference takes about 10 s, checked by `KernelReference.DefaultConstantMatchesCpuReference`). Non-default parameters compute their reference on the CPU at worker start.
- **D-041 Process isolation.** The kernel runs in `gpuoc-worker.exe` so a driver fault cannot take down the service. Worker exit codes: 2 init failure, 4 device lost, 5 hang. Per-submit timeout = `tuning.hang_timeout_s`.
- **D-042 Device selection** matches the Vulkan `deviceUUID` to the NVML UUID; the formatting is identical for NVIDIA. Unverified on hardware.
- **D-043 Score unit** is Gop/s (one LCG/xorshift step = 4 operations). Each of the 3 measured runs lasts `benchmark.run_s` (10 s).
- **D-044 "The GPU hit a limit"** means a thermal slowdown, a hardware slowdown, or a safety-limit violation. A power cap is normal steady-state behaviour for NVIDIA boost, so it does not invalidate a result unless `benchmark.invalid_on_power_cap = true`.
- **D-045 Missing telemetry** means 3 consecutive failed polls, or no successful sample in a run that did work. The run then stops (the GPU's temperature can no longer be checked) and is `Inconclusive`. A stress run stops on its first piece of `Unstable` evidence.
- **D-046 External benchmarks** repeat the command until the warm-up time has passed, then run it once per measured run. The score is the regex's first capture group, or the whole match. A non-zero exit is an error.
- **D-047 Event IDs** treated as driver failures: `nvlddmkm` 13, 14, 153 and `Display` 4101. Best effort; see [WINDOWS.md](WINDOWS.md).
- **D-048 Bench and stress from the CLI** always run locally, and are refused while the service is tuning a GPU.

## Tuning

- **D-050 The core search** runs with the memory offset already backed off by the margin, so memory errors are not mistaken for core instability.
- **D-051 Boundary look-ahead.** After the binary search converges, one extra trial runs at `U + step`. Without it, a stable point above an unstable one could never be observed in a fresh run. Re-tests run at 2× the duration, up to `tuning.max_retest_rounds` (2). A conflict that remains caps the result below the lowest unstable point.
- **D-052 Inconclusive trials** are repeated once. If the second is also inconclusive, tuning stops (no verdict is guessed).
- **D-053 A crash during a trial** (a `trial_start` without a `trial_result`) is replayed as `Unstable`, since the system or process died at those settings.
- **D-054 Resume** replays the log by sequence number through the same deterministic algorithm, so benchmark scores replay too. A session resumes only for the same GPU, driver, and tuning parameters (hashed). An abort (`tune abort`, service stop, Ctrl+C) leaves the session resumable; completion or a fatal error ends it.
- **D-055 Voltage stage.** Each single voltage step is tested at the first failing core offset; if that offset is still unstable, the gain is under one core step, and the stage stops. The stage also stops at the ceiling, or when the trial reports a power cap. It is skipped for `max_perf_per_watt`, where extra voltage cannot help.
- **D-056 Objective metric:** max_performance = score; max_perf_per_watt = score / W; balanced = score / √W. The power stage raises the limit only while the GPU is power-capped and the metric improves by at least 1%.
- **D-057 Fan stage.** If the stress temperature is within 3 °C of the target, a firmer curve (passing the floor rule) is tried and kept only if it lowers the temperature.
- **D-058 Validation back-off** lowers both offsets by one step each per retry.
- **D-059 Budget.** Before each search trial, the remaining budget must cover the trial, one long validation run, and one benchmark. When it does not, the search stops and the best point found so far is validated. If not even validation fits, nothing is committed.
- **D-060 After a failed tuning run** (not validated, or an error), the service does not retry automatically for 24 h; `gpuoc tune` retries at once.

## Controller and service

- **D-070 Hysteresis.** Above target, the core offset steps down one step per poll (never below `min(0, ceiling)`). Stepping up needs 60 s continuously more than 5 °C below target, and the timer restarts after each step.
- **D-071 Telemetry loss in Run.** 3 consecutive failed reads count as a violation ("telemetry"). Driver events, ECC errors, and a lost GPU count as *instability*: two in 24 h revoke the LKG, which forces a re-tune.
- **D-072 Staleness** of the validated profile is checked hourly while running, besides the check at start.
- **D-073 Watchdog.** Default 120 s, and it must be at least 2× `hang_timeout_s` (a hung worker iteration legitimately blocks for that long). Every watched thread has its own heartbeat slot. Exit code 3 triggers the SCM failure actions.
- **D-074 SCM failure actions:** restart after 10 s, 30 s, and 60 s, with the counter reset after 1 day; actions also apply to non-crash failures. Preshutdown timeout 60 s.
- **D-075 One controller thread per GPU.** Pipe commands return `Busy` while that GPU is tuning.
- **D-076 Pipe protocol:** one request per connection, handled in turn; requests are capped at 1 MiB. Write methods are authorized per call through `ImpersonateNamedPipeClient` + `CheckTokenMembership(Administrators)`, which requires elevation under UAC. On development hosts a Unix socket stands in for the pipe, and root counts as admin.
- **D-077 `state.json`** persists violations, cooldowns, disablement, tuning failures, and the active default profile. A corrupt file stops the service from starting, so a disabled GPU cannot silently become enabled again.

## Profiles and CLI

- **D-080 Profile modes:** `stock`, `fixed`, or `auto` (the validated LKG for the profile's objective). `power_limit_pct` is a percentage of stock TDP. `temp_target_c` can only tighten the global target. Per-GPU overrides use `[gpu."<UUID>"]` (fixed mode only).
- **D-081 Auto-switch order:** profiles sorted by `priority` (lower first; default 100), then by name; rules in file order; the first match wins. When no rule matches, the default profile returns.
- **D-082 Default profile.** Empty means "automatic": the controller tunes and applies for `autotune.objective` (default `max_perf_per_watt`, as the spec requires). `gpuoc profile activate <name>` persists a default in `state.json`.
- **D-083 Precedence:** built-in < system < user. The built-in `stock` profile cannot be overridden, edited, or deleted. Invalid profile files are skipped and reported; they never block the other profiles.
- **D-084 `profile edit`** takes `--set key=value` (and the creation flags) for scripted edits, or otherwise opens `%EDITOR%` (Notepad by default) on a temporary copy. Every save is validated by the parser first; an invalid edit leaves the original untouched.
- **D-085 Local `set` and `profile apply`** hold the settings until Ctrl+C (or `--for <s>`), then restore stock, because every write path ends in a `StockGuard`. Persistent settings need the service.
- **D-086 GPU selectors:** an index, a UUID, or a unique UUID prefix; empty means every GPU.
- **D-087 Unknown config keys are errors.** A misspelled safety key must not be silently ignored.
- **D-088 `--dry-run` with no NVIDIA driver** falls back to the mock backend (the spec says dry-run uses the mock). In dry-run, `lkg.json` and `state.json` are never written, and the tuning log gets a `dryrun-` prefix.
- **D-089 Locked clocks** are reported (from whether `nvmlDeviceSetGpuLockedClocks` is exported) and never used.
