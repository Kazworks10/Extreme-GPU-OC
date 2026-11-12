# Windows

## Service

| Property | Value |
|---|---|
| Name | `ExtremeGpuOc` (display name "Extreme GPU OC") |
| Binary | `gpuocd.exe` (next to `gpuoc.exe` and `gpuoc-worker.exe`) |
| Account | LocalSystem |
| Start type | Automatic (delayed start) |
| Failure actions | Restart after 10 s, 30 s, 60 s; counter reset after 1 day; also on non-crash failure exits (the watchdog exits with code 3) |
| Controls accepted | Stop, Preshutdown (60 s preshutdown timeout) |

Install and control it from an **elevated** prompt. Each line is a separate command:

```bat
gpuoc service install
gpuoc service start
gpuoc service status
gpuoc service stop
gpuoc service uninstall
```

`gpuoc service install --config D:\gpuoc\gpuoc.toml` stores a config path in the service command line.

What happens at each stage:

- **Start:** every GPU is reverted to stock first, whatever a previous (perhaps crashed) instance left. Then the validated profile is re-applied if one exists and `safety.acknowledge_risk = true`. Otherwise the service tunes (if both gates are set) or stays monitor-only.
- **Stop and preshutdown:** tuning is aborted, stock is restored on every GPU synchronously, and only then does the service report `SERVICE_STOPPED`.
- **Watchdog:** a controller thread or the main loop that stops reporting progress for `service.watchdog_timeout_s` (120 s by default) makes the watchdog restore stock and exit, and the Service Control Manager restarts the service.

To debug, run the same code without the SCM. Ctrl+C stops it the same way the SCM would:

```bat
gpuocd --console -v
```

Options: `--config <path>`, `--dry-run` (log every write, apply nothing), `--backend auto|nvidia|mock`, `--pipe <name>`.

## Privileges

| Operation | Needs |
|---|---|
| `list`, `status`, `doctor`, `profile list/show`, `tune status` | Any user |
| `set`, `reset`, `tune`, `tune abort`, `profile apply/activate` through the service | An **elevated** Administrator. The pipe server checks the caller's token; with UAC, a non-elevated administrator token does not count. |
| The same operations in local mode (service not running) | An elevated prompt. `--dry-run` and `--backend mock` need none. |
| `service install/uninstall/start/stop` | An elevated prompt |
| `profile create/edit/delete` | Write access to the profile folder (user scope by default; `--system` writes to `%ProgramData%`) |

## Control pipe

`\\.\pipe\gpuoc` carries JSON-RPC 2.0, one request and one response per connection (see [API.md](API.md)). The security descriptor is:

```
D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12018b;;;BU)
```

- SYSTEM and Administrators get full access.
- Users get `FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES` (`0x12018b`). That is enough to send a request and read the reply, but not `FILE_CREATE_PIPE_INSTANCE`, so users cannot create pipe instances.

Remote clients are rejected (`PIPE_REJECT_REMOTE_CLIENTS`). The first instance is created with `FILE_FLAG_FIRST_PIPE_INSTANCE`, and each new instance is created before the previous one is released, so the name is never free for another process to take. Clients connect with `SECURITY_IDENTIFICATION` so the server can check their group membership.

## File locations

| Path | Contents |
|---|---|
| `%ProgramData%\ExtremeGpuOc\gpuoc.toml` | Configuration (override with `--config`) |
| `%ProgramData%\ExtremeGpuOc\lkg.json` | Validated (last-known-good) profiles |
| `%ProgramData%\ExtremeGpuOc\state.json` | Violations, cooldowns, disabled GPUs, the active default profile |
| `%ProgramData%\ExtremeGpuOc\logs\gpuocd.log` | Service log |
| `%ProgramData%\ExtremeGpuOc\logs\tune-<uuid>.jsonl` | Append-only tuning log (`dryrun-tune-…` in dry-run) |
| `%ProgramData%\ExtremeGpuOc\profiles.d\*.toml` | System profiles |
| `%APPDATA%\ExtremeGpuOc\profiles.d\*.toml` | User profiles (override system profiles of the same name) |

`%ProgramData%` and `%APPDATA%` are resolved with `SHGetKnownFolderPath` (`FOLDERID_ProgramData`, `FOLDERID_RoamingAppData`), falling back to the environment variables. Tests override both roots with `GPUOC_PROGRAMDATA` and `GPUOC_APPDATA`. Note that the service runs as LocalSystem, so its `%APPDATA%` is the system profile's. User profiles are therefore read by the CLI, while the service reads the system profiles and its own.

In `--dry-run`, `lkg.json` and `state.json` are never written.

## Libraries

All three libraries are loaded at runtime from System32 only (`LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_SYSTEM32`), so a DLL planted in the working directory is never picked up:

- `nvml.dll` (falls back to `%ProgramW6432%\NVIDIA Corporation\NVSMI\nvml.dll` on older drivers)
- `nvapi64.dll`
- `vulkan-1.dll` (loaded by the worker)

`gpuoc doctor` shows which libraries loaded, their file versions, the driver version, which NVAPI and NVML functions are available, whether the Event Log can be read, and the Vulkan devices.

## Event Log checks (best effort)

During stress runs and while monitoring, the System log is queried with this XPath:

```
*[System[((Provider[@Name='nvlddmkm'] and (EventID=13 or EventID=14 or EventID=153)) or
          (Provider[@Name='Display'] and EventID=4101)) and TimeCreated[@SystemTime>='…']]]
```

`Display` 4101 is the TDR message "Display driver nvlddmkm stopped responding and has successfully recovered". The `nvlddmkm` IDs vary between driver branches. **Verify them on your system**: open Event Viewer → Windows Logs → System, filter by the source `nvlddmkm`, and compare the IDs that appear after a known driver reset with `DriverEventIds` in `src/stress/event_log.hpp`. If the log cannot be read, stress verdicts rely on the other signals (checksums, worker exit status, device loss, ECC), and `gpuoc doctor` reports it.

## Child processes

`gpuoc-worker.exe` and external benchmark commands start with `CreateProcessW`, suspended, inside a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`, and only then resume. Only the stdout pipe and `NUL` are inherited (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). If gpuoc exits for any reason, its children die with it. An exit code at or above `0xC0000000` (an NTSTATUS such as `0xC0000005`) counts as a crash.
