# Runs the Apex benchmark suite (Phase 6) against the compose stack and
# records environment + results alongside the run log.
#
# Usage:
#   powershell ./scripts/bench.ps1                        # Release, all scenarios
#   powershell ./scripts/bench.ps1 -Scenario B            # single scenario
#   powershell ./scripts/bench.ps1 -SoakSeconds 120       # append a soak run
#
# Requires the Release build (benchmarks on Debug are meaningless) and a
# healthy compose stack. Every scenario verifies logical execution counts;
# any correctness failure stops the run with a non-zero exit code.
# Results are appended to docs/benchmark-runs.md (committed evidence)
# and the raw log is kept next to it.

param(
  [string]$Scenario = "all",
  [int]$SoakSeconds = 0,
  [ValidateSet("Release")][string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$Binary = Join-Path $Root "build\tests\$Config\apex_bench.exe"
if (-not (Test-Path $Binary)) {
  Write-Error "Benchmark binary not found: $Binary. Build with ./scripts/build.ps1 -Config $Config."
}

& docker compose up -d postgres redis
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$deadline = (Get-Date).AddSeconds(150)
for (;;) {
  $pg = docker inspect --format "{{.State.Health.Status}}" "apex-phase0-postgres-1" 2>$null
  $re = docker inspect --format "{{.State.Health.Status}}" "apex-phase0-redis-1" 2>$null
  if ($pg -eq "healthy" -and $re -eq "healthy") { break }
  if ((Get-Date) -ge $deadline) { Write-Error "infra not healthy (pg=$pg redis=$re)." }
  Start-Sleep -Seconds 2
}

$env:APEX_TEST_POSTGRES_CONN = "host=127.0.0.1 port=5432 dbname=apex user=apex password=apex-dev-only connect_timeout=5 application_name=apex-bench"
$env:APEX_TEST_REDIS_HOST = "127.0.0.1"
$env:APEX_TEST_REDIS_PORT = "6379"

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$LogFile = Join-Path $Root "docs\benchmark-run-$Stamp.log"
$Args = @("--scenario=$Scenario")
if ($SoakSeconds -gt 0) { $Args += "--soak-seconds=$SoakSeconds" }

Write-Host "Running apex_bench $($Args -join ' ') (log: $LogFile)"
& $Binary @Args 2>&1 | Tee-Object -FilePath $LogFile
if ($LASTEXITCODE -ne 0) { Write-Error "Benchmark correctness gate FAILED (see $LogFile)." }

$Cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$RamGB = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$Commit = (git -C $Root rev-parse --short HEAD) 2>$null
$Summary = @"
## Run $Stamp (commit $Commit)
- machine: $Cpu / ${RamGB}GiB RAM / Windows / MSVC $(& "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe" 2>&1 | Select-Object -First 1) / $Config
- postgres: $(docker compose exec -T postgres postgres --version 2>$null) / redis: $(docker compose exec -T redis redis-server --version 2>$null)
- command: apex_bench $($Args -join ' ')
- log: docs/benchmark-run-$Stamp.log
- result: PASS (all scenarios met their correctness gates)
"@
Add-Content -Path (Join-Path $Root "docs\benchmark-runs.md") -Value $Summary
Write-Host "Recorded in docs/benchmark-runs.md"
