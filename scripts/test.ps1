# Runs the automated test suite.
#
# Usage:
#   powershell ./scripts/test.ps1                        # hermetic subset (Debug)
#   powershell ./scripts/test.ps1 -Config Release
#   powershell ./scripts/test.ps1 -WithPostgres           # FULL suite incl. live PG
#   powershell ./scripts/test.ps1 -Config Release -WithPostgres
#
# Without -WithPostgres only the hermetic tests run: PostgreSQL-gated tests
# self-skip when APEX_TEST_POSTGRES_CONN is unset, so unit feedback stays
# fast and needs no Docker. With -WithPostgres the script starts the compose
# stack, waits for PostgreSQL to be healthy, points the gated tests at it,
# and runs everything (unit + integration + concurrency + failure paths).
# Docker reachability alone is covered by scripts/infra-smoke.ps1.

param(
  [ValidateSet("Debug", "Release")][string]$Config = "Debug",
  [switch]$WithPostgres
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

$CmakeBin = "C:\Program Files\CMake\bin"
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
  $env:PATH = "$CmakeBin;$env:PATH"
}

if ($WithPostgres) {
  Set-Location $Root
  Write-Host "Starting PostgreSQL via docker compose..."
  & docker compose up -d postgres
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  $deadline = (Get-Date).AddSeconds(120)
  for (;;) {
    $status = docker inspect --format "{{.State.Health.Status}}" "apex-phase0-postgres-1" 2>$null
    if ($status -eq "healthy") { break }
    if ((Get-Date) -ge $deadline) {
      Write-Error "postgres did not become healthy within 120 seconds."
    }
    Start-Sleep -Seconds 2
  }
  Write-Host "postgres is healthy."

  # Dev-stack credentials (docker-compose.yml defaults). These are
  # development-only values, already public in the repo — never use them
  # beyond local testing.
  $env:APEX_TEST_POSTGRES_CONN = "host=127.0.0.1 port=5432 dbname=apex user=apex " +
    "password=apex-dev-only connect_timeout=5 application_name=apex-tests"
  Write-Host "APEX_TEST_POSTGRES_CONN set for gated tests."
} else {
  Write-Host ("PostgreSQL-gated tests will SKIP (no APEX_TEST_POSTGRES_CONN). " +
    "Use -WithPostgres for the full suite.")
}

& ctest --test-dir (Join-Path $Root "build") -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "All tests ($Config) passed."
