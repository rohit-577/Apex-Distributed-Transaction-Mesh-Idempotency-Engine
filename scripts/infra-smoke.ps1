# Smoke test for the Docker development infrastructure.
#
# Usage:
#   powershell ./scripts/infra-smoke.ps1
#
# What it does:
#   1. Starts postgres + redis via docker compose (builds nothing).
#   2. Waits for both health checks to report healthy.
#   3. Proves reachability from the host: TCP on both ports plus a real
#      postgres ping and redis ping from inside the containers.
#   4. Leaves the stack running for development (use `docker compose down`
#      to stop it).
#
# Exit code is non-zero on any failure so CI can gate on this script.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Wait-Healthy($Service, $TimeoutSec = 120) {
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $status = docker inspect --format "{{.State.Health.Status}}" "apex-phase0-$Service-1" 2>$null
    if (-not $status) {
      # Fallback when the container name differs (custom COMPOSE_PROJECT_NAME).
      $status = docker compose ps --format json 2>$null | ConvertFrom-Json |
        Where-Object { $_.Service -eq $Service } |
        ForEach-Object { docker inspect --format "{{.State.Health.Status}}" $_.ID }
    }
    if ($status -eq "healthy") {
      Write-Host "$Service is healthy."
      return
    }
    Start-Sleep -Seconds 2
  }
  Write-Error "$Service did not become healthy within $TimeoutSec seconds."
}

& docker compose up -d
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Wait-Healthy "postgres"
Wait-Healthy "redis"

Write-Host "--- postgres ping ---"
& docker compose exec -T postgres pg_isready -U ${env:POSTGRES_USER:-apex} -d ${env:POSTGRES_DB:-apex}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "--- redis ping ---"
& docker compose exec -T redis redis-cli ping
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$TcpPg = Test-NetConnection -ComputerName "127.0.0.1" -Port 5432 -WarningAction SilentlyContinue
$TcpRedis = Test-NetConnection -ComputerName "127.0.0.1" -Port 6379 -WarningAction SilentlyContinue
if (-not $TcpPg.TcpTestSucceeded) { Write-Error "TCP 127.0.0.1:5432 unreachable from host." }
if (-not $TcpRedis.TcpTestSucceeded) { Write-Error "TCP 127.0.0.1:6379 unreachable from host." }
Write-Host "TCP 127.0.0.1:5432 reachable: $($TcpPg.TcpTestSucceeded)"
Write-Host "TCP 127.0.0.1:6379 reachable: $($TcpRedis.TcpTestSucceeded)"

Write-Host "Infra smoke test PASSED."
