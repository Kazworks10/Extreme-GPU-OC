# Architecture

## Components

```
                         gpuoc.exe (CLI)                         gpuocd.exe (service)
                              |   \                                   |
            JSON-RPC 2.0 over \\.\pipe\gpuoc  \  local mode           |  ServiceLifecycle (SCM / --console)
                              |                 \                     |  Watchdog, PipeServer
                              v                  v                    v
                        +-----------------------------------------------------+
                        | Engine: backend, Gpu objects, SafetyPolicy, stores, |
                        | workloads, event log, controllers, auto-switch, RPC |
                        +-----------------------------------------------------+
                           |              |               |              |
              GpuController (one per GPU) |        ProfileStore     AutoSwitcher
              state machine + RuntimeMonitor            (TOML)       (process list)
                           |
                        Tuner ---- ITrialRunner (StressRunner + IBenchmark) ---- IWorkload
                           |                                                       |
                       TuningLog (JSONL)                          ProcessWorkload -> gpuoc-worker.exe
                           |                                                       (Vulkan compute)
                        LkgStore (lkg.json), StateStore (state.json)
                           |
                         Gpu facade  (capabilities, SafetyPolicy::check, read-back verify, StockGuard)
                           |
                      IGpuBackend
            +--------------+---------------+---------------+
       NvidiaBackend    MockBackend    DryRunBackend (decorator)
     NVML + NVAPI loaded at runtime
```

| Directory | Contents |
|---|---|
| `include/gpuoc/` | Public API: `Result<T>`, the device model, `IGpuBackend`, `Gpu`, `StockGuard`, `SafetyPolicy`, `IClock` |
| `src/core/` | Device model, safety policy, config (TOML), LKG store, JSON codec, logging, heartbeat |
| `src/backends/` | `nvidia/` (NVML, NVAPI, capability derivation), `mock/` (simulated GPU), dry-run decorator |
| `src/bench/` | Kernel (GLSL source, embedded SPIR-V, CPU reference), Vulkan runner, worker process, benchmarks |
| `src/stress/` | Stress runner, verdicts, Windows Event Log reader |
| `src/tuner/` | Search, stages, tuning log, re-tune policy |
| `src/controller/` | Per-GPU state machine, runtime monitor, state store, watchdog, engine |
| `src/profile/` | Profile format, store, auto-switching, process listing |
| `src/platform/` | Paths, files, processes and job objects, the pipe, the service, dynamic libraries |
| `src/rpc/` | The JSON-RPC 2.0 dispatcher and client |
| `src/cli/`, `src/service/` | `gpuoc.exe` and `gpuocd.exe` |

## The write path

Every write goes `Gpu::apply` → `SafetyPolicy::check` → `IGpuBackend::apply` → `IGpuBackend::read_settings` → compare.

1. **Capabilities.** A field the GPU does not support is an `Unsupported` error, never ignored.
2. **Safety.** Gates (voltage, power above stock, the fan floor) refuse the request. Limits are `min(config, driver range, hard ceiling)`. A value outside them is rejected (`ClampMode::Reject`, used for every user-facing path) or clamped with a note (`ClampMode::Clamp`).
3. **Write and read back.** If the read-back differs beyond tolerance, the GPU is reset to stock and the call returns `VerifyFailed`; a partial apply is never left in place. If a write fails, the previous values are restored, and a failed fan curve returns the fans to driver control.

`StockGuard` captures the stock settings when it is created and restores them when it is destroyed. Guards register in `StockRegistry`, so the console Ctrl+C handler, service stop and preshutdown, and the watchdog can restore every guarded GPU synchronously from any thread.

## Controller state machine

```
Discover -> Baseline -> Tune -> Validate -> Commit -> Run
   |                                                   |
   +-> MonitorOnly (gates not set, tuning failed)      +-> (violation) SafeRevert -> Idle --cooldown--> Discover
   +-> Disabled (3 violations in 24 h, until re-enabled)
```

- **Discover:** check the persisted state (disabled, cooldown), any manual target, and whether the LKG entry for the objective is current (`retune_reason`: new UUID, new driver, older than 14 days, repeated instability). A current LKG is applied, and needs `acknowledge_risk`. Otherwise, if both gates are set, tuning starts.
- **Baseline, Tune, Validate:** a `Tuner` runs in the controller's thread. Commands from the pipe get `Busy` while it runs, except `tune abort`.
- **Commit:** writes the validated entry to `lkg.json`. `LkgStore::commit` refuses anything not validated.
- **Run:** applies the target and calls `RuntimeMonitor::tick` every poll: hysteresis, software fan-curve upkeep, and violation checks.
- **SafeRevert:** stock is restored at the moment the violation is seen. The state then records the violation, starts the cooldown, or disables the GPU.

## Threads

| Thread | Work |
|---|---|
| main (service) | `ServiceLifecycle::run` → `Engine::run`: the auto-switch poll every 2 s and a heartbeat |
| one per GPU | `GpuController::step` in a loop (tuning blocks this thread only) |
| pipe server | One JSON-RPC request per connection, handled in turn |
| watchdog | Fires if any registered thread misses its heartbeat for `service.watchdog_timeout_s` |
| per child process | A stdout reader (worker or external benchmark) |

Each controller thread and the main loop register a heartbeat slot. The watchdog looks at the stalest slot, so a busy thread cannot hide a hung one.

## Tuning in detail

The `Tuner::search` loop over one dimension (memory offset, then core offset):

1. If a stable point lies above an unstable one, the region is suspect. Its points are re-tested at twice the duration, up to `max_retest_rounds` rounds. If the conflict remains, everything above the lowest unstable point is treated as unstable.
2. `L` is the highest stable point below the lowest unstable point `U`.
3. With no `U` yet, the ramp tests `lo + step`, then `lo + 2(L − lo)` (`step × 2^n`), capped at the limit.
4. While `U − L > step`, test the midpoint on the step grid.
5. Boundary look-ahead: test `U + step` once. If it passes, step 1 re-tests both.
6. Stop when the budget would not leave room for validation plus one benchmark.

An inconclusive trial is re-run once. A second inconclusive result stops tuning, because guessing either way would bias the search. A trial that exceeds a temperature or power limit is a safety violation: tuning stops and the controller enters SafeRevert.

Resume works by replay. A session is keyed by the GPU, the driver version, and a hash of the tuning parameters. Trial *n* from the log is reused when the algorithm requests the same stage, settings and duration at sequence *n*. A `trial_start` without a `trial_result` (the process died) is replayed as `Unstable`. From the first mismatch on, trials run live.

## The mock GPU

`MockBackend` simulates hidden maximum stable core and memory offsets, derived from the seed. The error rate is zero at or below them and rises steeply above (more than 75 MHz past the core limit ends in a device loss and a TDR event). Voltage adds stable core headroom. The simulation also models power (∝ clocks × V², capped by the power limit), thermals (first-order lag, fan-dependent thermal resistance), and throttling. Fault injection covers read-back mismatch, failing writes, missing telemetry, transient and persistent instability bands, failures that appear only under long stress, crashes, hangs, non-zero exits, device loss, driver events, ECC errors, and temperature or power spikes. It is deterministic for a given seed and call sequence, and it uses its own random-number transforms so results match across compilers. With a `state_file`, the "hardware registers" outlive the process, which is how the crash-recovery test checks that a restart reverts settings left behind by a killed process.
