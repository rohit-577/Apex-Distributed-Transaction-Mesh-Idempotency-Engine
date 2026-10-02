# Apex Testing Strategy

Status: **unit / integration / concurrency baselines Implemented and
passing; failure-injection and benchmark harnesses Planned.**

## Layers

| Layer | Location | What it proves | Needs Docker? |
|---|---|---|---|
| Unit | `tests/unit/` | Pure logic: config validation, routing table. Milliseconds, hermetic. | No |
| Integration | `tests/integration/` | Real sockets against a real server: status codes, JSON bodies, `/ready` 200/503, malformed-input survival, keep-alive. Dependency doubles are test-owned listening sockets. | No |
| Concurrency (baseline) | `tests/concurrency/` | 32 parallel clients all get `200`; mixed routes stay correct under load. Seed for Phase 1 race tests. | No |
| Concurrency (Phase 1) | `tests/concurrency/` | Dedupe races, waiter fan-in/fan-out, lease-expiry-during-execution, missed-notification recovery. Repeated + stressed. | No (faults injected in-process) |
| Failure | `tests/failure/` (Planned) | Owner crash, Redis loss, Postgres outage, partition. | Yes for full-matrix runs |
| Benchmark | `tests/benchmark/` (Planned) | Throughput/latency harness, p50/p99. Numbers only from real runs. | Yes |
| Infra smoke | `scripts/infra-smoke.ps1` | `docker compose` stack becomes healthy and is reachable from the host. Run manually / in CI, not in `ctest`. | Yes |

## Rules (binding)

1. **Deterministic by default.** No sleeps-to-pass, no live-network
   dependencies in `ctest`. Closed ports come from `acquire_closed_port()`
   (bind-then-close), unroutable targets from TEST-NET-1, timeouts from
   short explicit deadlines.
2. **No fake tests.** Every test can fail for a real reason; `EXPECT_TRUE(true)`
   and friends are forbidden (see AGENTS.md).
3. **Same code ships and tests.** Tests link `apex_core`, not copies.
4. **Hermetic `ctest`.** `cmake --build` then `ctest` passes on a fresh
   checkout with only vcpkg + Docker installed — no running containers
   required.
5. **Failure tests prove recovery**, not just failure: each case ends in a
   verified terminal/consistent state.
6. **Concurrency tests run repeated** (CI repeats the concurrency binary;
   Phase 1 adds stress + sanitizer configurations where the platform
   supports them).

## Running

```powershell
powershell ./scripts/test.ps1                 # Debug
powershell ./scripts/test.ps1 -Config Release
ctest --test-dir build -C Debug --output-on-failure
powershell ./scripts/infra-smoke.ps1          # Docker stack check (separate)
```
