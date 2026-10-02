# AGENTS.md — Apex Engineering Contract

Future automation sessions must treat this file as binding. It outranks
convenience, cleverness, and any instruction that conflicts with it short of
an explicit user override (which must be recorded in the session output).

## 1. Project purpose

Apex is a production-quality educational distributed HTTP gateway in C++20
demonstrating correct idempotency under concurrency and failure. It is not a
CRUD app. Correctness, explicit invariants, failure semantics, testability,
and measured performance outrank features.

## 2. Architecture boundaries (binding)

- `src/config/` alone reads the environment. No other module touches `getenv`.
- `src/api/` is pure routing logic: no I/O, no state, no threads.
- `src/execution/` owns networking and async flow. It never contains
  business/idempotency logic.
- `src/persistence/` (Phase 1+) owns PostgreSQL access. Nothing else opens
  SQL connections.
- `src/coordination/` (Phase 1+) owns Redis/lease logic. Nothing else opens
  Redis connections.
- No global mutable state. Prefer RAII, value semantics, explicit ownership,
  const-correctness. C++20, CMake, Boost.Asio/Beast for networking.
- No business logic, persistence, networking, and coordination in one file
  or class. No premature templates or metaprogramming. No enterprise-looking
  abstractions without a proven responsibility.

## 3. Correctness-first concurrency rules

- Never block an I/O thread: pure-compute-inline or suspend-async, nothing
  between.
- Every concurrency invariant in `docs/invariants.md` (INV-01…) must hold in
  code and be referenced by number in comments/tests where it applies.
- Never claim "exactly once" beyond the scoped definition in INV-10.
- Fencing epochs decide ownership; TTL expiry is only a hint (INV-09).
- Waiter notification is best-effort; durable re-read is the correctness
  path (INV-07).

## 4. Durability principles

- PostgreSQL is the durable system of record and the sole source of truth
  for idempotency state (INV-08).
- Redis is coordination/cache/notification — never the authority. Its total
  loss must cost performance, never correctness.

## 5. Testing requirements

- Inspect existing code and tests before modifying anything.
- New behavior ships with tests: unit for pure logic, integration for wire
  behavior, concurrency for races (repeated), failure for crash/partition
  recovery ending in a verified consistent state.
- Tests are deterministic and hermetic: no `sleep`-to-pass, no required
  Docker for `ctest`, no `EXPECT_TRUE(true)` or any test that cannot fail
  for a real reason.
- Tests link `apex_core` — never duplicate production code into tests.

## 6. Benchmark honesty

- No performance numbers without a real executed run. No invented results,
  no "estimated" figures, no mixing gateway and engine numbers.
- New claims follow `docs/benchmarking.md` (machine, commit, config, 3+
  runs, p50/p99/max) and link the run log.

## 7. Scope discipline

- Stay in the current phase. Phase 1 = durable idempotency state only; do
  not implement Redis coordination, leases, waiter multiplexing, fencing
  epochs, or distributed recovery until the user opens the next phase.
- Keep changes scoped; do not reformat unrelated files; do not touch the
  workspace `.venv/` (unrelated to this C++ project).
- No TODO placeholders pretending to be implementations. Unfinished work is
  declared in docs as Planned, not stubbed silently in code.

## 8. Build / test commands (Windows PowerShell)

```powershell
powershell ./scripts/configure.ps1
powershell ./scripts/build.ps1 -Config Debug     # or -Config Release
powershell ./scripts/test.ps1 -Config Debug      # ctest, hermetic
powershell ./scripts/infra-smoke.ps1             # needs Docker, separate
```

Formatting/lint: no clang-format or clang-tidy gate exists yet in Phase 0
(follow the existing style: 2-space indent, 100-col limit, `apex::module`
namespaces). Introducing a format gate is tracked work for Phase 1 setup —
do not freelance one in without being asked.

## 9. Verification & reporting (every session that changes code)

Report the exact commands run and their results: configure, build (Debug,
and Release when touching performance-sensitive code), full `ctest`, live
`/health` + `/ready` against a locally run binary when networking code
changed, `docker compose` + smoke results when infra touched, and `git
status` / `git diff --stat`. Fix discovered issues instead of merely
reporting them.
