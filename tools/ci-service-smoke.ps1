# CI only: install the real Windows service on the simulated GPU in dry-run
# mode, drive it over \\.\pipe\gpuoc, and stop it through the SCM.
param([string]$Bin = "build/bin/Release")
$ErrorActionPreference = "Stop"
$gpuoc = Join-Path $Bin "gpuoc.exe"

function Run([string[]]$argv) {
  Write-Host "> gpuoc $($argv -join ' ')"
  & $gpuoc @argv
  if ($LASTEXITCODE -ne 0) { throw "gpuoc $($argv -join ' ') exited with $LASTEXITCODE" }
}

try {
  Run @("service", "install", "--backend", "mock", "--dry-run")
  Run @("service", "start")
  Run @("service", "status")
  Run @("list")                                  # over the pipe
  Run @("set", "--gpu", "0", "--core", "+30")    # write method: elevated caller
  $status = & $gpuoc --json status | ConvertFrom-Json
  if ($status.gpus[0].controller.state -ne "Run") { throw "expected the controller in Run, got $($status.gpus[0].controller.state)" }
  if ($status.gpus[0].settings.core_offset_mhz -ne 30) { throw "core offset not applied" }
  & $gpuoc set --gpu 0 --core +9000
  if ($LASTEXITCODE -ne 3) { throw "out-of-range set should exit 3, got $LASTEXITCODE" }
  Run @("service", "stop")
  $log = Get-Content "$env:ProgramData\ExtremeGpuOc\logs\gpuocd.log"
  $log | Select-Object -Last 25 | Write-Host
  if (-not ($log -match "all GPUs restored to stock \(service stop\)")) { throw "no stock restore logged on stop" }
} finally {
  & $gpuoc service uninstall | Out-Host
}
exit 0
