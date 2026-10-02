# Runs the full automated test suite.
#
# Usage:
#   powershell ./scripts/test.ps1                 # Debug (default)
#   powershell ./scripts/test.ps1 -Config Release
#
# The suite needs no Docker infrastructure: dependency tests use local
# sockets owned by the tests themselves. Docker reachability is covered by
# scripts/infra-smoke.ps1 instead, so unit feedback stays fast and hermetic.

param([ValidateSet("Debug", "Release")][string]$Config = "Debug")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

$CmakeBin = "C:\Program Files\CMake\bin"
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
  $env:PATH = "$CmakeBin;$env:PATH"
}

& ctest --test-dir (Join-Path $Root "build") -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "All tests ($Config) passed."
