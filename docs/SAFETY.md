# Safety

These rules are compiled into the program. Configuration can tighten the limits but never loosen them past the hard ceilings in `include/gpuoc/safety.hpp`. Nothing writes to a GPU except through `Gpu::apply` and `Gpu::reset_to_stock`, and `Gpu::apply` always calls `SafetyPolicy::check`.

## Gates

| Rule | Where | Tests |
|---|---|---|
| Autonomous tuning only with `autotune.enabled = true` **and** `safety.acknowledge_risk = true`; otherwise monitor-only | `SafetyPolicy::autotune_permitted`, `GpuController::step_discover` | `Gates.AutotuneRequiresBothFlags`, `Controller.MonitorOnlyWithoutBothGates` |
| Any other autonomous write (re-applying the LKG at boot, automatic profile switching) needs `acknowledge_risk`; a manual `gpuoc tune` needs it too | `SafetyPolicy::autonomous_writes_permitted`, `Engine::profile_apply`, CLI `tune` | `Gates.AutonomousWritesRequireAcknowledgement`, `Controller.ReappliesValidLkgOnlyWithAcknowledgement`, `Cli.LimitsAndGatesExitThree` |
| Voltage changes need driver voltage control **and** `safety.allow_voltage = true` | `SafetyPolicy::check`, `derive_capabilities`, `Tuner::stage_voltage` | `Gates.VoltageRefusedWithoutAllowVoltage`, `Gates.VoltageRefusedWithoutBackendSupportEvenIfAllowed`, `TunerSearch.VoltageStageSkippedWithoutGate`, `NvidiaCapabilities.MissingSetterMeansNoOffsetsOrVoltage`, `NvidiaApply.ThroughTheGpuFacadeWithSafety`, `Cli.LimitsAndGatesExitThree` |
| A power limit above stock TDP needs `safety.allow_power_above_stock = true` | `SafetyPolicy::check`, `limits_for` | `Gates.PowerAboveStockRefusedWithoutGate`, `Gates.PowerAboveStockAllowedWithGate`, `ProfileResolve.LimitsAndGatesApplyToProfiles`, `Cli.LimitsAndGatesExitThree` |

## Limits

| Rule | Where | Tests |
|---|---|---|
| Effective limit = `min(config, driver range, hard ceiling)`; config can tighten, never loosen | `SafetyPolicy::SafetyPolicy`, `limits_for` | `LimitPrecedence.*`, `Config.HardCeilingStillAppliesToParsedConfig`, `TunerSafety.NeverExceedsEffectiveLimits` |
| Requests outside the range are clamped or rejected, never passed through | `SafetyPolicy::check`, `Gpu::apply` | `GpuFacade.RejectsOutOfDriverRange`, `GpuFacade.ClampsToDriverRange`, `LimitPrecedence.CheckClampsAndRejects` |
| Hard ceilings: maximum temperature 90 °C; defaults: revert at 83 °C, target 75 °C | `HardLimits` | `LimitPrecedence.TemperatureConfigCannotExceedHardCeiling`, `LimitPrecedence.Defaults`, `Config.DefaultsMatchSpec` |
| Power limit defaults to stock TDP and is capped by the driver maximum | `limits_for` | `LimitPrecedence.Defaults`, `LimitPrecedence.PowerConfigBeyondDriverIsClamped` |
| Voltage ceiling = driver maximum − one step; config cannot raise it | `limits_for` | `LimitPrecedence.VoltageCeilingIsDriverMaxMinusOneStep`, `LimitPrecedence.VoltageStepFallbackWhenDriverReportsNone`, `TunerSearch.VoltageExtendsStableCore` |
| Fans never below 30% while the GPU is above 60 °C | `check_fan_curve`, `enforce_fan_floor`, `NvidiaBackend::set_fans` | `FanFloor.CurveBelowFloorWhenHotIsRefused`, `FanFloor.ConfigCanOnlyRaiseFloor`, `FanFloor.FacadeRefusesUnsafeCurve`, `NvidiaApply.SoftwareFanCurveAndFloor` |
| If a fan curve cannot be read or applied, driver-automatic fan control is restored | `Gpu::apply` (rollback), `Gpu::maintain_fan`, `NvidiaBackend::set_fans` | `GpuFacade.ApplyFailureRestoresAutomaticFan`, `NvidiaApply.SoftwareFanCurveAndFloor` |
| A read-back mismatch is an error and is never left in place | `Gpu::apply` | `GpuFacade.ReadbackMismatchIsAnErrorAndRollsBack` |

## Stock restore

| Exit path | Mechanism | Tests |
|---|---|---|
| Normal completion | `~StockGuard` | `StockGuard.RestoresOnNormalCompletionAndRegisters` |
| Error | `~StockGuard` during unwinding of `Result` returns | `StockGuard.RestoresOnErrorPath` |
| Ctrl+C, Ctrl+Break, console close, logoff, shutdown | `SetConsoleCtrlHandler` → `StockRegistry::restore_all` (synchronous), then the abort flag | `StockRegistry.RestoreAllRevertsEveryGuardedGpu` (the handler itself is not driven by a test) |
| Service stop | `ServiceLifecycle`: StopPending → `Engine::shutdown` (synchronous) → Stopped | `ServiceControl.StopRestoresStockBeforeReportingStopped` |
| Service preshutdown | Same as stop | `ServiceControl.PreshutdownRestoresStockBeforeReportingStopped` |
| Watchdog timeout | `Watchdog`: `restore_all`, then `_Exit(3)`; the SCM restarts the service | `Watchdog.StallTriggersRevertThenExit`, `WatchdogDeathTest.RealStallRestoresStockAndExits` |
| Start-up failure | Stop hook still runs before Stopped | `ServiceControl.StartFailureStillRestoresAndReportsStopped` |

## Boot behaviour

| Rule | Tests |
|---|---|
| The restart path reverts to stock before anything else, including settings left by a killed process | `Controller.StartupRevertsToStockFirst`, `CrashRecoveryDeathTest.KilledDuringTrialRevertsAndResumes` |
| The LKG profile is re-applied only if it was validated and has not been revoked | `LkgStore.OnlyValidatedSettingsEnter`, `LkgStore.UnvalidatedEntriesInFileAreIgnored`, `LkgStore.RevokeHidesEntries`, `Controller.ReappliesValidLkgOnlyWithAcknowledgement` |
| Unvalidated settings never persist across a reboot: only validated entries are stored, and automatic profile application is limited to validated or conservative settings (no clock, power or voltage above stock) | `LkgStore.OnlyValidatedSettingsEnter`, `AutoSwitch.MatchingProcessAppliesProfileAndExitRevertsIt` |

## Runtime

| Rule | Tests |
|---|---|
| A violation (temperature over max, power over max, driver failure event, ECC error, GPU lost) reverts to stock within one poll interval | `RuntimeMonitor.ViolationRevertsWithinOnePollInterval`, `RuntimeMonitor.DriverEventAndEccAreViolations` |
| 30-minute cooldown; three violations in 24 h disable auto-overclock until it is manually re-enabled (persisted) | `RuntimeMonitor.CooldownThenRetryAndDisableAfterThree` |
| Hysteresis never raises the core offset above the validated value | `RuntimeMonitor.HysteresisStepsDownAndBackUpNeverAboveValidated` |
| A safety limit exceeded during tuning is a violation (SafeRevert), not just an unstable point | `Controller.SafetyViolationDuringTuningEntersSafeRevert` |
| Inconclusive never counts as stable | `Verdicts.MissingTelemetryIsInconclusive`, `Verdicts.CleanRunIsStable` |

## Never implemented

There is no VBIOS or firmware flashing, no fuse or lock writes, no BIOS changes, and no disabling of vendor or OS safety features. That includes the TDR registry keys (`TdrDelay`, `TdrLevel`); `gpuoc` writes no registry values at all. `NeverImplement.ForbiddenOperationsAbsentFromSource` scans `src/` and `include/` for registry-write APIs, TDR key names, and flashing entry points, and fails if any appear.
