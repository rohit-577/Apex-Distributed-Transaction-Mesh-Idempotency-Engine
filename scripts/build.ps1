# Builds the Apex binaries.
#
# Usage:
#   powershell ./scripts/build.ps1            # Debug (default)
#   powershell ./scripts/build.ps1 -Config Release

param([ValidateSet("Debug", "Release")][string]$Config = "Debug")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

$CmakeBin = "C:\Program Files\CMake\bin"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  $env:PATH = "$CmakeBin;$env:PATH"
}

& cmake --build (Join-Path $Root "build") --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Build ($Config) succeeded."
