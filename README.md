# Apex — Distributed Transaction Mesh & Idempotency Engine

A production-quality **educational** distributed HTTP gateway in C++20 that
demonstrates correct idempotency handling, concurrent request deduplication,
distributed lease coordination, in-flight multiplexing, durable PostgreSQL
state, Redis coordination, failure recovery, and rigorous concurrency
testing.

> **Phase 0 status: foundation only.** The gateway builds, serves, and is
> tested — but the idempotency engine does not exist yet.
> `POST /v1/operations` returns `501 Not Implemented` by design. Do not
> proceed to engine work until the Phase 0 report is reviewed.

## Repository map

```text
src/
  main.cpp            entry: config -> validate -> serve -> graceful shutdown
  config/             env-based configuration + validation (only env reader)
  api/                pure request router (no I/O, fully unit-tested)
  core/               phase constants
  execution/          HttpServer (accept loop), Session (connection),
                      DependencyChecker (async /ready probes)
  observability/      tiny thread-safe stderr logger
  coordination/       PLANNED Phase 1 (Redis leases, fencing, notification)
  persistence/        PLANNED Phase 1 (PostgreSQL idempotency records)
  concurrency/        PLANNED Phase 1 (in-flight map, waiter multiplexing)
tests/
  unit/               config + router tests (hermetic, milliseconds)
  integration/        real-socket HTTP + dependency-probe tests
  concurrency/        parallel-client baseline (seed for Phase 1 race tests)
  failure/ benchmark/ PLANNED (see docs/testing-strategy.md)
docs/                 architecture, invariants, state-machine, failure-model,
                      testing-strategy, benchmarking (status-labeled)
scripts/              configure / build / test / infra-smoke automation
docker-compose.yml    postgres:16 + redis:7 dev stack (health-checked)
```

Deviations from the Phase 0 brief, with reasons: `docker-compose.yml` lives
at the repo root (so `docker compose up` just works) instead of `docker/`;
`migrations/`, `tests/failure/`, `tests/benchmark/` are omitted until they
hold real content — empty directories prove nothing. All three are tracked
work in the docs.

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
`nlohmann-json` (JSON bodies), `gtest` (tests). PostgreSQL/Redis client
libraries arrive with Phase 1 — Phase 0 probes dependencies with plain TCP.

## Quickstart

```powershell
# 1. One-time: vcpkg + environment
git clone https://github.com/microsoft/vcpkg $HOME/vcpkg
& $HOME/vcpkg/bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$HOME/vcpkg"

# 2. Configure / build / test
powershell ./scripts/configure.ps1
powershell ./scripts/build.ps1 -Config Debug
powershell ./scripts/test.ps1 -Config Debug

# 3. Infrastructure (separate terminal)
docker compose up -d
powershell ./scripts/infra-smoke.ps1

# 4. Run the gateway
.\build\src\Debug\apex.exe
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
| `APEX_POSTGRES_HOST` / `APEX_POSTGRES_PORT` | `127.0.0.1` / `5432` | Durable-state endpoint (TCP-checked by `/ready`) |
| `APEX_REDIS_HOST` / `APEX_REDIS_PORT` | `127.0.0.1` / `6379` | Coordination endpoint (TCP-checked by `/ready`) |
| `APEX_LOG_LEVEL` | `info` | `debug` \| `info` \| `warning` \| `error` |

Exit codes: `0` clean shutdown · `1` runtime failure (e.g. port in use) ·
`2` invalid configuration (nothing is started).

## Endpoints

| Endpoint | Status | Meaning |
|---|---|---|
| `GET /health` | `200` | Process liveness. Never touches dependencies. |
| `GET /ready` | `200` / `503` | Both dependencies TCP-reachable within 2 s, else not-ready with per-dependency detail. |
| `POST /v1/operations` | `501` | Declared contract, engine lands in Phase 1. |
| anything else / wrong method | `404` / `405` | JSON errors, `Allow` header on `405`. |

## Verification (Phase 0 report)

Build, test, run, `/health`, `/ready`, Docker connectivity, and `git`
inspection results are recorded in the Phase 0 final report (delivered with
this foundation), not as claims in this file.

## Further reading

- `docs/architecture.md` — topology, module boundaries, decisions
- `docs/invariants.md` — INV-01…INV-11 correctness contract
- `docs/state-machine.md` — declared Phase 1 record lifecycle
- `docs/failure-model.md` — what must be survived
- `docs/testing-strategy.md` — test layers and determinism rules
- `docs/benchmarking.md` — methodology; no numbers exist yet
- `AGENTS.md` — engineering contract for future automation sessions
