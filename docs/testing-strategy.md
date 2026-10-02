# Apex Testing Strategy

Status: **unit / integration / concurrency Implemented and passing (71
tests), including the live-PostgreSQL matrix; failure-injection beyond the
database layer and benchmark harnesses Planned.**

## Layers

| Layer | Location | What it proves | Needs Docker? |
|---|---|---|---|
| Unit | `tests/unit/` | Pure logic: config, routing, key rules, canonical JSON, fingerprints (incl. FIPS vector), simulated op, status parsing. Hermetic. | No |
| Integration | `tests/integration/` | Real sockets: status codes, JSON, `/ready` 200/503, malformed survival, keep-alive. Real PostgreSQL: acquire/find/complete/fail, terminal guards, rollback, CHECK constraints, pool recycling, conn failure. Full 7-case operations semantics over HTTP. | Only the `PgFixture` tests (self-skip without `APEX_TEST_POSTGRES_CONN`) |
| Concurrency | `tests/concurrency/` | 32-client health baseline + TEST A (2-way race), TEST B (50-way fan-in), TEST C (fingerprint race). Barrier-coordinated (`std::latch`), never sleeps. | Races need live PG (skip otherwise) |
| Failure (DB scope) | `tests/integration/db_repository_test.cpp` | Rollback, duplicate-insert race (23505), malformed rows (23514), invalid transitions, unreachable DB. Each ends in a verified consistent state. | Same gating as above |
| Failure (distributed) | `tests/failure/` (Planned) | Owner crash, Redis loss, partition. | Yes for full-matrix runs |
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
4. **Hermetic by default, live on demand.** Plain `ctest` runs the hermetic
   subset (live-DB tests self-skip with a clear reason). The full matrix
   runs via `scripts/test.ps1 -WithPostgres`, which brings up PostgreSQL,
   waits for health, and sets `APEX_TEST_POSTGRES_CONN`. Gated tests use
   unique keys + per-test cleanup, so order never matters.
5. **Failure tests prove recovery**, not just failure: each case ends in a
   verified terminal/consistent state.
6. **Concurrency tests run repeated** (CI repeats the concurrency binary;
   Phase 1 adds stress + sanitizer configurations where the platform
   supports them).

## Running

```powershell
powershell ./scripts/test.ps1                        # hermetic subset (Debug)
powershell ./scripts/test.ps1 -Config Release
powershell ./scripts/test.ps1 -WithPostgres           # FULL suite incl. live PG
powershell ./scripts/test.ps1 -Config Release -WithPostgres
ctest --test-dir build -C Debug --output-on-failure   # same as first line
powershell ./scripts/infra-smoke.ps1                  # Docker stack check (separate)
```
