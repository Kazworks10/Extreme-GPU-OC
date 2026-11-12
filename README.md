# Extreme GPU OC

`gpuoc` is an autonomous overclocker for NVIDIA GPUs on Windows 10 and 11 (x64). Once started, it finds your NVIDIA GPUs, benchmarks and stress-tests them, and tunes clock offsets, voltage, power limit and fan curve. It validates the result with a long stress run, stores it as a last-known-good (LKG) profile, and keeps watching the GPU while it runs. Safety limits are built in, and configuration cannot turn them off.

> **Safety warning.** Overclocking runs your GPU outside the settings its maker validated. It can crash or freeze the system, corrupt data silently (including files being written at the time), and shorten the life of the hardware. It may also void your warranty. `gpuoc` reduces these risks with hard limits, long validation runs, and automatic reverts, but it cannot remove them. Autonomous tuning stays off until you set **both** `autotune.enabled = true` and `safety.acknowledge_risk = true`. Back up your data first.

## What it does

- **Discover:** enumerates NVIDIA GPUs through NVML (`nvml.dll`). Clock offsets and voltage go through NVAPI (`nvapi64.dll`). Both libraries are loaded at runtime. If one is missing, its features are reported as unsupported.
- **Benchmark:** a deterministic integer Vulkan compute kernel that needs only the installed driver (no CUDA toolkit). Its output is checked against a known reference checksum. You can also use any external command and parse its score with a regex. Protocol: 30 s warm-up, then 3 measured runs; the result is the median, and it is marked invalid if the coefficient of variation (CV) is over 3% or the GPU hit a limit.
- **Stress:** the same kernel runs in its own process (`gpuoc-worker.exe`) inside a kill-on-close job object. Every run ends in one verdict: `Stable`, `Unstable(reason)`, or `Inconclusive(reason)`.
  - Unstable: checksum mismatch, crash, non-zero exit, GPU lost or hung, driver reset in the Windows Event Log, new uncorrected ECC errors, or temperature or power over the limit.
  - Inconclusive: telemetry was missing, or the run was interrupted.
- **Tune:** memory offset first, then core offset, each with an exponential ramp and then a binary search. Then voltage (only behind its gate), then power limit and fan. One step of margin is taken off, and a 30-minute validation run follows; if it fails, `gpuoc` backs off one more step and validates again. Every trial is written to an append-only JSON-lines log, so tuning resumes after a crash or reboot.
- **Run:** applies the validated profile and polls telemetry every second. If the GPU runs above the target temperature, the core offset steps down; once it has stayed cool for long enough, it steps back up. A violation (temperature or power over the maximum, a driver failure, ECC errors, or a lost GPU) reverts the GPU to stock immediately and starts a 30-minute cooldown. Three violations in 24 hours disable auto-overclock for that GPU until you re-enable it.
- **Profiles:** TOML files with an objective, a target (`stock`, `fixed`, or `auto` = validated), per-GPU overrides, and process match rules for automatic switching. The built-ins are `stock`, `quiet`, `balanced` and `max`.

## Install

Requirements: Windows 10 or 11 x64, an NVIDIA GPU with a current driver, and an elevated (Administrator) prompt for anything that writes settings or installs the service.

1. Build (below) or unpack a release. Keep `gpuoc.exe`, `gpuocd.exe` and `gpuoc-worker.exe` in the same folder.
2. Check the system:
   ```bat
   gpuoc doctor
   ```
3. Create the configuration at `%ProgramData%\ExtremeGpuOc\gpuoc.toml`. Start from [`examples/gpuoc.toml`](examples/gpuoc.toml).
4. Install and start the service (elevated):
   ```bat
   gpuoc service install
   ```
   ```bat
   gpuoc service start
   ```

See [docs/WINDOWS.md](docs/WINDOWS.md) for service details, privileges and file locations.

## Quick start

```bat
gpuoc list
gpuoc status
gpuoc bench --gpu 0
gpuoc stress --duration 300 --gpu 0
```

Each line is a separate command. To let the service tune autonomously, set both gates in `gpuoc.toml`, then restart the service:

```toml
[autotune]
enabled = true

[safety]
acknowledge_risk = true
```

You can also start tuning yourself and follow it:

```bat
gpuoc tune --objective max_perf_per_watt
```
```bat
gpuoc tune status
```

To change settings manually (limits are enforced; out-of-range values are rejected, never silently clamped):

```bat
gpuoc set --gpu 0 --core +60 --mem +200 --power 300
```
```bat
gpuoc reset --gpu 0
```

When the service is running, `set` and `profile apply` go to it over `\\.\pipe\gpuoc`, and the settings stay until they are changed or the service stops. Without the service, `gpuoc` holds the settings only while it keeps running (until you press Ctrl+C, or for `--for <seconds>`); every exit path restores stock.

Try everything without an NVIDIA GPU:

```bat
gpuoc --backend mock status
gpuoc --backend mock --dry-run set --gpu 0 --core +45 --for 5
```

### Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | usage error |
| 2 | unsupported (missing library, driver, or GPU feature) |
| 3 | safety refusal (gate not set, value outside the limits) |
| 4 | hardware error (including stress verdicts that are not `Stable`) |
| 5 | insufficient privileges |

Add `--json` to any command for machine-readable output.

## Build

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```
```bat
cmake --build build --config Release
```
```bat
ctest --test-dir build -C Release
```

The build needs CMake 3.20 or newer and MSVC (Visual Studio 2022 or later) or clang-cl (`-T ClangCL`). Dependencies are fetched at configure time and pinned: toml++ 3.4.0, nlohmann/json 3.11.3 and GoogleTest 1.15.2 by commit; the NVML header from NVIDIA's `cuda_nvml_dev` 12.6.77 redistributable by SHA-256; the NVAPI headers from `github.com/NVIDIA/nvapi` by commit; and Vulkan-Headers 1.3.275 by commit. No NVIDIA library is linked.

Hardware tests are skipped unless `GPUOC_HW_TESTS=1`. They only read from the GPU; every write goes through dry-run.

## Verified and unverified on hardware

Nothing in this repository has been run on a real NVIDIA GPU. The development environment had no NVIDIA GPU and no Windows machine.

**Verified (automated tests):**

- The whole pipeline (discovery, benchmark, stress verdicts, tuning search across 24 seeds, non-monotonic re-tests, voltage, power and fan stages, validation back-off, budget cap, crash-and-resume, the controller state machine, the runtime monitor, cooldown and disablement, service stop and preshutdown, the watchdog, profiles, auto-switching, and the CLI) against the mock backend with a simulated clock.
- The Vulkan compute kernel is bit-exact against its CPU reference on a software Vulkan driver (Mesa lavapipe), both in-process and through `gpuoc-worker`.
- Windows code paths, built with MinGW-w64 and run under Wine: `CreateProcessW` with job objects and handle-list inheritance, crash, hang and exit detection of the worker, named-pipe JSON-RPC, `%ProgramData%`/`%APPDATA%` resolution, death tests for crash recovery and the watchdog.
- MSVC (Visual Studio 2022 with the exact definition-of-done commands, and the Visual Studio shipped on `windows-latest`) and clang-cl builds, with the full test suite passing on GitHub Actions Windows runners. That includes the crash-recovery and watchdog death tests, NTSTATUS crash detection of the worker, named-pipe anti-squatting, and the CLI integration tests.
- The real service under the real Service Control Manager, on the simulated GPU in dry-run mode: install, start, `list` and `set` over `\\.\pipe\gpuoc` as an elevated client, refusal of an out-of-range value (exit 3), stop with stock restored before `SERVICE_STOPPED`, and uninstall (`tools/ci-service-smoke.ps1`).
- The Windows Event Log query runs and reads the System log on a real Windows machine (on CI, with no NVIDIA driver, so no NVIDIA events).
- NVML and NVAPI status-code parsing, capability derivation, unit conversion, the fan-curve floor, PCI-bus mapping between NVML and NVAPI, and stock reset, all against fake function tables.

**Unverified on hardware:**

- Every real NVML call (`nvml.dll`): telemetry, power limits, fan control (`nvmlDeviceSetFanSpeed_v2` is restricted on many consumer boards), ECC counters, and clock-event reasons.
- Every real NVAPI call. In particular, **`NvAPI_GPU_SetPstates20` is not in the public NVAPI SDK.** It is resolved by its interface ID `0x0F4DAE6B`, the ID widely used by third-party tools. If the driver does not resolve it, clock offsets and voltage are reported unsupported. The units of the memory offset (effective versus actual memory clock) have not been confirmed.
- Voltage control through the P-state 2.0 base-voltage delta. Most current GPUs do not expose it, and `gpuoc` then reports it unsupported.
- Matching Vulkan devices to NVML GPUs by UUID.
- The Windows Event Log query and the event IDs used for driver failures (`nvlddmkm` 13, 14, 153 and `Display` 4101). Verify them on your system ([docs/WINDOWS.md](docs/WINDOWS.md)).
- Preshutdown, failure actions (restart after a crash or watchdog exit), and delayed auto-start under the real Service Control Manager. CI does install, start, drive over the pipe, and stop the real service, but only on the simulated GPU in dry-run mode.
- Thermal behaviour, how long tuning really takes, and whether the default search steps suit any particular card.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): components, threads and data flow.
- [docs/API.md](docs/API.md): the C++ overclocking API, the JSON-RPC methods and file formats.
- [docs/SAFETY.md](docs/SAFETY.md): every safety rule and the tests that enforce it.
- [docs/WINDOWS.md](docs/WINDOWS.md): the service, privileges, the pipe, file locations and the Event Log.
- [docs/DECISIONS.md](docs/DECISIONS.md): every choice made where the specification was silent.
