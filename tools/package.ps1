# Build a portable zip: the three executables, the docs, and the examples.
param([string]$Bin = "build/bin/Release", [string]$Out = "dist")
$ErrorActionPreference = "Stop"
$sha = (git rev-parse --short HEAD).Trim()
$name = "extreme-gpu-oc-$sha-win-x64"
$stage = Join-Path $Out $name
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($exe in "gpuoc.exe", "gpuocd.exe", "gpuoc-worker.exe") {
  Copy-Item (Join-Path $Bin $exe) $stage
}
Copy-Item README.md $stage
Copy-Item -Recurse docs (Join-Path $stage "docs")
Copy-Item -Recurse examples (Join-Path $stage "examples")
# Sanity: the executables must start on a machine without the VC++ redistributable.
foreach ($exe in "gpuoc.exe", "gpuocd.exe", "gpuoc-worker.exe") {
  $bytes = [System.IO.File]::ReadAllBytes((Join-Path $stage $exe))
  $text = [System.Text.Encoding]::ASCII.GetString($bytes)
  if ($text -match "(?i)VCRUNTIME140\.dll|MSVCP140\.dll") { throw "$exe depends on the VC++ runtime DLLs" }
}
& (Join-Path $stage "gpuoc.exe") --version
if ($LASTEXITCODE -ne 0) { throw "gpuoc.exe --version failed" }
$zip = Join-Path $Out "$name.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "packaged $zip"
