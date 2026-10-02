# Apex Architecture

Status legend used across all docs: **Implemented** · **Planned** · **Validated** (observed by a test or a run) · **Not validated**.

## Purpose

Apex is a production-quality *educational* distributed HTTP gateway in C++20
whose job is to demonstrate correct idempotency handling under concurrency
and failure. It is not a CRUD app and not a generic REST API: every design
choice serves the question "what happens when the same write arrives twice,
at the same time, while machines fail?"

## Phase 0 scope (this document describes intent + current status)

Phase 0 builds only the foundation: a working async HTTP gateway, strict
configuration, Dockerized infrastructure, and a tested build. **The
idempotency engine is not implemented yet** — `POST /v1/operations` answers
`501 Not Implemented` and says so in the response body.

## Target topology (Planned — Phase 1+)

```text
HTTP client(s)
      |
      v
C++ async HTTP gateway (Boost.Asio/Boost.Beast, I/O thread pool)
      |
      v
Idempotency engine  (dedupe concurrent duplicates, waiter multiplexing,
                     fencing/ownership checks)
      |                       |
      v                       v
PostgreSQL               Redis
durable system           coordination / cache /
of record                waiter notification (best-effort only)
      |
      v
Business operation (simulated write)
```

Duplicate concurrent requests must eventually wait on the single in-flight
execution and receive the same final response without re-executing the
operation. Waiters recover by re-reading durable state — never by trusting a
notification alone (see `docs/invariants.md`, `docs/failure-model.md`).

## Module boundaries (Implemented unless marked)

| Area | Path | Responsibility | Status |
|---|---|---|---|
| Entry | `src/main.cpp` | Config load → validate → serve → graceful shutdown. No business logic. | Implemented |
| Config | `src/config/` | Env-based config + validation. Only module that reads the environment. | Implemented |
| API routing | `src/api/` | Pure (method, target) → result mapping. No I/O, no state. | Implemented |
| Execution | `src/execution/` | `HttpServer` (accept loop), `Session` (connection), `DependencyChecker` (async probes). No business logic. | Implemented |
| Observability | `src/observability/` | Tiny thread-safe stderr logger. Metrics/tracing are later phases. | Implemented |
| Core | `src/core/` | Phase constants. Shared domain types arrive with Phase 1. | Implemented (minimal) |
| Coordination | `src/coordination/` | Redis leases, fencing tokens, waiter notification. | Planned (Phase 1+) |
| Persistence | `src/persistence/` | PostgreSQL idempotency-record repository. Durable source of truth. | Planned (Phase 1) |
| Concurrency | `src/concurrency/` | In-flight map, waiter multiplexing primitives. | Planned (Phase 1) |
| Business op | `src/execution/` or new `src/domain/` | Simulated write behind the idempotency record. | Planned (Phase 1) |

Rules: networking never blocks on coordination or persistence; persistence
never imports networking; coordination decisions are re-checkable against
durable state. No global mutable state anywhere.

## Request lifecycle today (Implemented, Validated by tests)

```text
accept -> Session::do_read (30 s idle timeout, 1 MiB body cap)
  -> parse error?   400 JSON, close
  -> body too big?  413 JSON, close
  -> GET /ready?    async TCP probes (2 s deadline each) -> 200/503 JSON
  -> else Router    -> 200 / 404 / 405 / 501 JSON
  -> keep-alive?    next read : close
```

SIGINT/SIGTERM stops the acceptor, drains in-flight sessions, stops the
`io_context`, and joins all I/O threads before any destructor runs.

## API contract (Implemented)

- `GET /health` → `200 {"status":"ok","service":"apex","version":"…","phase":"phase-0"}`.
  Process liveness only; never touches dependencies.
- `GET /ready` → `200 {"status":"ready",…}` when PostgreSQL **and** Redis
  TCP-connect within the deadline, else `503 {"status":"not_ready",…}` with
  per-dependency booleans. Phase 0 checks reachability only — no SQL, no
  RESP session (Planned: real readiness probes in Phase 1).
- `POST /v1/operations` (JSON body, future `Idempotency-Key` header) →
  `501 {"error":"not_implemented",…}`. Request validation, dedup, and
  execution are Phase 1.
- Unknown route → `404`; wrong method on a known route → `405` + `Allow`.

## Decisions made in Phase 0

1. **Boost.Asio/Beast** for async networking (no hand-written HTTP parser).
2. **vcpkg manifest mode** (`vcpkg.json`) for reproducible third-party deps:
   `boost-asio`, `boost-beast`, `gtest`, `nlohmann-json`. Rationale: native
   MSVC support, binary caching, one file declares everything.
3. **Visual Studio CMake generator** as the supported default on Windows —
   needs no Developer Prompt and covers Debug + Release in one configure.
4. **Header `Idempotency-Key` contract declared now, enforced later**, so the
   future API shape is stable before the engine exists.
5. **`/ready` does TCP checks only** and says so in this doc — a fake-deep
   health check would be worse than an honest shallow one.

## Deliberately deferred

- PostgreSQL schema and repository, Redis client and lease protocol, fencing
  token design, waiter multiplexing, exactly-once scope definition, metrics,
  structured logging, TLS, authentication, rate limiting, benchmarks.
