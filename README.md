# Apex — Distributed Transaction Mesh & Idempotency Engine

A production-quality **educational** distributed HTTP gateway in C++20 that
demonstrates correct idempotency handling, concurrent request deduplication,
distributed lease coordination, in-flight multiplexing, durable PostgreSQL
state, Redis coordination, failure recovery, and rigorous concurrency
testing.

> **Phase 1 status: durable idempotency core implemented.** `POST
> /v1/operations` is backed by PostgreSQL (key validation, fingerprinting,
> atomic acquire, replay, conflicts — all tested live). Still ahead:
> Redis coordination/leases, waiter multiplexing (`202` for now instead of
> waiting), distributed recovery. Do not begin Phase 2 until the Phase 1
> report is reviewed.

## Repository map

```text
src/
  main.cpp            entry: config -> validate -> serve -> graceful shutdown
  config/             env-based configuration + validation (only env reader)
  api/                pure request router (no I/O, fully unit-tested)
  core/               phase constants
  execution/          HttpServer (accept loop), Session (connection,
                      dependency probes, async operations dispatch)
  idempotency/        key rules, fingerprinting, simulated op, service
                      (no sockets, no SQL)
  persistence/        RAII libpq connections, bounded pool, idempotency
                      repository (the only SQL), schema applier
  observability/      tiny thread-safe stderr logger + transition logging
  coordination/       PLANNED later phase (Redis leases, fencing, notification)
  concurrency/        PLANNED later phase (in-flight map, waiter multiplexing)
tests/
  unit/               config, router, request-contract tests (hermetic)
  integration/        real-socket HTTP + dependency probes + live-PostgreSQL
                      repository/operations tests (gated, see below)
  concurrency/        parallel-client baseline + idempotency race tests
                      (2-way, 50-way fan-in, fingerprint race; gated)
  failure/ benchmark/ PLANNED (see docs/testing-strategy.md)
migrations/           V001__idempotency_records.sql (single source of truth;
                      applied by server at boot + by test fixtures)
docs/                 architecture, invariants, state-machine, failure-model,
                      testing-strategy, benchmarking (status-labeled)
scripts/              configure / build / test / infra-smoke automation
docker-compose.yml    postgres:16 + redis:7 dev stack (health-checked)
```

Deviations from the original brief, with reasons: `docker-compose.yml` lives
at the repo root (so `docker compose up` just works) instead of `docker/`;
`tests/failure/`, `tests/benchmark/` are omitted until they hold real
content — empty directories prove nothing. Both are tracked work in the docs.

## Prerequisites

| Tool | Version used | Notes |
|---|---|---|
| Windows 11 x64 | 10.0.26200 | Primary dev OS |
| Visual Studio Build Tools (C++ workload) | 18.x (MSVC 14.51) | Any recent VS works; `configure.ps1` auto-detects |
| CMake | ≥ 3.28 (4.4.3 used) | `winget install Kitware.CMake` |
| vcpkg (bootstrapped) | rolling | `VCPKG_ROOT` must be set; deps install automatically at configure |
| Docker Desktop | 28.x + Compose v2 | Only for the infra stack, never for `ctest` |
| Git | 2.x | |

The `.venv/` directory in the workspace root is **unrelated** to this C++
project (no Python code exists here) and is left untouched.

Third-party C++ dependencies (all via `vcpkg.json`, rationale in
`docs/architecture.md`): `boost-asio` + `boost-beast` (async HTTP),
`nlohmann-json` (JSON bodies), `gtest` (tests), `libpq` 18.x (durable
state), `openssl` 3.x (SHA-256 fingerprints). Note: `configure.ps1`
redirects the vcpkg installed tree to `%LOCALAPPDATA%\apex` because
meson-based ports cannot build under paths with spaces/`&` — see
`docs/architecture.md` decision 6.

## Quickstart

```powershell
# 1. One-time: vcpkg + environment
git clone https://github.com/microsoft/vcpkg $HOME/vcpkg
& $HOME/vcpkg/bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$HOME/vcpkg"

# 2. Configure / build / test
powershell ./scripts/configure.ps1
powershell ./scripts/build.ps1 -Config Debug
powershell ./scripts/test.ps1 -Config Debug                    # hermetic subset
powershell ./scripts/test.ps1 -Config Debug -WithPostgres     # FULL suite

# 3. Infrastructure (separate terminal)
docker compose up -d
powershell ./scripts/infra-smoke.ps1

# 4. Run the gateway (needs the DB password for the operations route;
#    /health + /ready work without it)
$env:APEX_POSTGRES_PASSWORD = "apex-dev-only"   # dev-stack default only
.\build\Debug\apex.exe
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/ready
```

Manual equivalents (any generator that supports your toolchain works; the
Visual Studio generator needs no Developer Prompt):

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug    # or Release
ctest --test-dir build -C Debug --output-on-failure
```

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `APEX_PORT` | `8080` | Listen port (`0` = OS-assigned, for tests) |
| `APEX_THREADS` | HW concurrency (min 2) | I/O thread-pool size |
| `APEX_POSTGRES_HOST` / `APEX_POSTGRES_PORT` | `127.0.0.1` / `5432` | Durable-state endpoint |
| `APEX_POSTGRES_USER` / `APEX_POSTGRES_DB` | `apex` / `apex` | Durable-state credentials/database (non-empty) |
| `APEX_POSTGRES_PASSWORD` | _(empty)_ | Durable-state password; empty = send none (trust auth). Never logged. |
| `APEX_DB_POOL_SIZE` | `8` | Blocking PG connection-pool size (`1`–`64`) |
| `APEX_MIGRATIONS_DIR` | `migrations` | Directory holding `V001__*.sql` (server working dir) |
| `APEX_REDIS_HOST` / `APEX_REDIS_PORT` | `127.0.0.1` / `6379` | Coordination endpoint (TCP-checked by `/ready` only) |
| `APEX_LOG_LEVEL` | `info` | `debug` \| `info` \| `warning` \| `error` |
| `APEX_TEST_POSTGRES_CONN` | _(unset)_ | libpq conninfo for gated tests; unset = those tests skip. Set automatically by `test.ps1 -WithPostgres`. |

Exit codes: `0` clean shutdown · `1` runtime failure (e.g. port in use) ·
`2` invalid configuration (nothing is started).

## Endpoints

| Endpoint | Status | Meaning |
|---|---|---|
| `GET /health` | `200` | Process liveness. Never touches dependencies. |
| `GET /ready` | `200` / `503` | Both dependencies TCP-reachable within 2 s, else not-ready with per-dependency detail. |
| `POST /v1/operations` + `Idempotency-Key` | `200` | First execution finished; `500` when the operation itself fails. |
| duplicate, same fingerprint, `PROCESSING` | `202` | In progress — retry later with the same key (no waiting yet). |
| duplicate, same fingerprint, `COMPLETED` | stored status | Stored response replayed byte-identically. |
| duplicate after `FAILED` | `409` | Terminal; original failure attached; use a new key to retry. |
| same key, different request | `409` | Fingerprint conflict; never executed as the original. |
| missing/invalid key or body | `400` | Machine-readable `error` (`missing_idempotency_key`, `invalid_idempotency_key`, `invalid_json_body`). |
| DB unreachable | `503` | `storage_unavailable`; gateway stays up. |
| anything else / wrong method | `404` / `405` | JSON errors, `Allow` header on `405`. |

## Verification (Phase 1 report)

Build (Debug + Release), 71/71 tests in both configs incl. the live-DB
matrix, clean-database migration, live `/health` + `/ready` + operations
flows, Docker smoke, and `git` inspection results are recorded in the
Phase 1 final report (delivered with this phase), not as claims in this file.

## Further reading

- `docs/architecture.md` — topology, module boundaries, request contract, decisions
- `docs/invariants.md` — INV-01…INV-17 correctness contract
- `docs/state-machine.md` — implemented record lifecycle + transaction table
- `docs/failure-model.md` — what must be survived
- `docs/testing-strategy.md` — test layers, gating rules, determinism rules
- `docs/benchmarking.md` — methodology; no numbers exist yet
- `AGENTS.md` — engineering contract for future automation sessions
