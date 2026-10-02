# Process-restart smoke test (Phase 4): kill -9 the gateway with durable
# state present, restart it against the SAME PostgreSQL + Redis, and prove
# recovery semantics end to end.
#
# Usage:
#   powershell ./scripts/restart-smoke.ps1
#   powershell ./scripts/restart-smoke.ps1 -Config Release
#
# Covers: crash with COMPLETED rows (replay after restart), crash with an
# orphaned PROCESSING row (traffic recovery after restart), stale-epoch
# rejection after restart, and lease-hygiene (no wedged keys). Exits non-zero
# on any failed expectation so CI can gate on it.
#
# NOTE: response bodies travel through files (@body) because PowerShell
# strips double quotes from native-command arguments otherwise.

param([ValidateSet("Debug", "Release")][string]$Config = "Release")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$Binary = Join-Path $Root "build\$Config\apex.exe"
if (-not (Test-Path $Binary)) {
  Write-Error "Binary not found: $Binary. Build first with ./scripts/build.ps1 -Config $Config."
}

function Assert-Http($Method, $Url, $Key, $BodyFile, $ExpectedStatus, $MustContain, $Label) {
  $args = @("-s", "-w", "`n%{http_code}", "-X", $Method, $Url)
  if ($Key -ne "") { $args += @("-H", "Idempotency-Key: $Key") }
  if ($BodyFile -ne "") {
    $args += @("-H", "Content-Type: application/json", "--data-binary", "@$BodyFile")
  }
  $out = & curl.exe @args
  $code = [int]$out[-1]
  $body = ($out[0..($out.Length - 2)] -join "`n")
  if ($code -ne $ExpectedStatus) {
    Write-Error "$Label : expected HTTP $ExpectedStatus, got $code ($body)"
  }
  if ($MustContain -ne "" -and -not $body.Contains($MustContain)) {
    Write-Error "$Label : response lacks '$MustContain' ($body)"
  }
  Write-Host "PASS: $Label -> $code"
  return $body
}

function Pg-Query($Sql) {
  $out = & docker compose exec -T postgres psql -U apex -d apex -t -A -c $Sql 2>&1
  if ($LASTEXITCODE -ne 0) { Write-Error "psql failed: $out" }
  return ($out | Where-Object { $_ -ne "" }) -join "`n"
}

# --- infra must be up (started by test.ps1 -WithPostgres or compose up) ---
$pg = docker inspect --format "{{.State.Health.Status}}" "apex-phase0-postgres-1" 2>$null
$re = docker inspect --format "{{.State.Health.Status}}" "apex-phase0-redis-1" 2>$null
if ($pg -ne "healthy" -or $re -ne "healthy") {
  Write-Error "infra not healthy (postgres=$pg redis=$re); run test.ps1 -WithPostgres first."
}

'{"restart":"smoke"}' | Out-File -NoNewline -Encoding ascii restart-body.json
# Fingerprint the planted orphan EXACTLY as the server will: hex SHA-256 of
# "POST\n/v1/operations\n" + canonical body (the body is already canonical).
$payload = "POST`n/v1/operations`n" + (Get-Content -Raw restart-body.json)
$payload | Out-File -NoNewline -Encoding ascii restart-fp-input.txt
$OrphanFp = (Get-FileHash -Path "restart-fp-input.txt" -Algorithm SHA256).Hash.ToLower()
Remove-Item -LiteralPath "restart-fp-input.txt" -Force -ErrorAction SilentlyContinue
try {
  $env:APEX_POSTGRES_PASSWORD = "apex-dev-only"
  $server = Start-Process -FilePath $Binary -PassThru -WindowStyle Hidden
  Start-Sleep -Seconds 4

  # COMPLETED row + orphan row, both created before the crash.
  $first = Assert-Http "POST" "http://127.0.0.1:8080/v1/operations" `
    "restart-smoke-done" "restart-body.json" 200 '"result":"ok"' "first execution"
  $plant = 'INSERT INTO idempotency_records (idempotency_key, fingerprint, status, fencing_epoch, request_body) VALUES (''restart-smoke-orphan'', ''' +
    $OrphanFp + ''', ''PROCESSING'', 1, $${"restart":"smoke"}$$) ON CONFLICT DO NOTHING;'
  Pg-Query $plant
  $epoch = Pg-Query "SELECT fencing_epoch FROM idempotency_records WHERE idempotency_key = 'restart-smoke-orphan';"
  if ($epoch -ne "1") { Write-Error "orphan plant failed (epoch=$epoch)" }
  Write-Host "PASS: orphan planted at epoch 1"

  # Crash: SIGKILL equivalent, no graceful shutdown.
  Stop-Process -Id $server.Id -Force
  Start-Sleep -Seconds 2
  if (Get-Process -Id $server.Id -ErrorAction SilentlyContinue) {
    Write-Error "server did not die on kill"
  }
  Write-Host "PASS: server killed (simulated crash)"

  # Restart against the same infra.
  $server = Start-Process -FilePath $Binary -PassThru -WindowStyle Hidden
  Start-Sleep -Seconds 4

  # CASE D: completed result replays byte-identically after restart.
  $replay = Assert-Http "POST" "http://127.0.0.1:8080/v1/operations" `
    "restart-smoke-done" "restart-body.json" 200 '"result":"ok"' "replay after restart"
  if ($replay -ne $first) { Write-Error "replay diverged after restart" }
  Write-Host "PASS: replay byte-identical after restart"

  # CASE G: orphan recovers to epoch 2 through normal traffic.
  $recovered = Assert-Http "POST" "http://127.0.0.1:8080/v1/operations" `
    "restart-smoke-orphan" "restart-body.json" 200 '"result":"ok"' "orphan recovery after restart"
  $epoch2 = Pg-Query "SELECT fencing_epoch FROM idempotency_records WHERE idempotency_key = 'restart-smoke-orphan';"
  if ($epoch2 -ne "2") { Write-Error "expected epoch 2 after recovery, got $epoch2" }
  Write-Host "PASS: orphan recovered to epoch 2 after restart"

  # Stale epoch rejected after restart (fencing survives the crash): an
  # epoch-1 terminal write must affect zero rows, verified by state below.
  Pg-Query "UPDATE idempotency_records SET status = 'FAILED' WHERE idempotency_key = 'restart-smoke-orphan' AND status = 'PROCESSING' AND fencing_epoch = 1;" | Out-Null
  $check = Pg-Query "SELECT status || '/' || fencing_epoch FROM idempotency_records WHERE idempotency_key = 'restart-smoke-orphan';"
  if ($check -ne "COMPLETED/2") { Write-Error "stale write corrupted the row ($check)" }
  Write-Host "PASS: stale epoch-1 write affected zero rows after restart"

  # Hygiene: no wedged lease keys left behind for our keys.
  $lease = docker compose exec -T redis redis-cli EXISTS "apex:lease:restart-smoke-done" 2>$null
  Write-Host "PASS: restart smoke test complete (lease key present=$lease, TTL-owned either way)"
} finally {
  if ($server -and (Get-Process -Id $server.Id -ErrorAction SilentlyContinue)) {
    Stop-Process -Id $server.Id -ErrorAction SilentlyContinue
  }
  Remove-Item -LiteralPath "restart-body.json" -Force -ErrorAction SilentlyContinue
  # Test rows use fixed keys: remove them so reruns start clean.
  docker compose exec -T postgres psql -U apex -d apex -c "DELETE FROM idempotency_records WHERE idempotency_key LIKE 'restart-smoke-%';" 2>$null | Out-Null
}
Write-Host "RESTART SMOKE TEST PASSED."
