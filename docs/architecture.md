# Apex Architecture

Status legend used across all docs: **Implemented** · **Planned** · **Validated** (observed by a test or a run) · **Not validated**.

## Purpose

Apex is a production-quality *educational* distributed HTTP gateway in C++20
whose job is to demonstrate correct idempotency handling under concurrency
and failure. It is not a CRUD app and not a generic REST API: every design
choice serves the question "what happens when the same write arrives twice,
at the same time, while machines fail?"

## Phase 1 scope (this document describes intent + current status)

Phase 1 adds the **durable PostgreSQL-backed idempotency layer**: key
validation, request fingerprinting, atomic record acquisition, terminal
transitions, response replay, and conflict rejection — all exercised against
a real database, all independent of Redis.

Explicitly NOT in Phase 1 (later phases): Redis coordination/leases/pub-sub,
fencing epochs, waiter multiplexing, multi-node ownership, distributed
recovery. A duplicate arriving while its record is `PROCESSING` gets a
deliberate `202`, not a wait. This phase provides **durable idempotency
state and atomic database-level coordination** — not distributed
exactly-once execution (see INV-10).

## Topology today (Implemented, Validated)

```text
HTTP client(s)
      |
      v
C++ async gateway (Asio/Beast, I/O thread pool; NEVER blocks — INV-01)
      |  POST /v1/operations: validate -> fingerprint -> post to DB pool
      v
Database worker threads (blocking allowed HERE only)
      |
      v
IdempotencyService -> IdempotencyRepository -> PostgreSQL 18 (libpq)
      |                    (durable authority, INV-08/INV-17)
      v
SimulatedOperation (deterministic stand-in for real work)
```

Redis is still only TCP-probed for `/ready`. Nothing reads or writes Redis
data in Phase 1.

## Module boundaries (Implemented unless marked)

| Area | Path | Responsibility | Status |
|---|---|---|---|
| Entry | `src/main.cpp` | Config → pool/service/DB threads → schema ensure → serve → ordered shutdown. No business logic. | Implemented |
| Config | `src/config/` | Env-based config + validation + libpq conninfo builder. Only module that reads the environment. | Implemented |
| API routing | `src/api/` | Pure (method, target) → result mapping. No I/O, no state. Does NOT own `/ready` or `/v1/operations` (dispatched by Session first). | Implemented |
| Execution | `src/execution/` | `HttpServer` (accept loop), `Session` (connection + async dispatch to service/DB pool), `DependencyChecker` (async probes). No business logic. | Implemented |
| Idempotency | `src/idempotency/` | Key rules, fingerprinting, simulated op, orchestration service. No sockets, no SQL. | Implemented |
| Persistence | `src/persistence/` | `PgConnection` (RAII libpq), `ConnectionPool` (bounded checkout), `IdempotencyRepository` (the only SQL), `Schema` (migration applier). No HTTP, no orchestration. | Implemented |
| Observability | `src/observability/` | Tiny thread-safe stderr logger + transition logging with key-truncation policy (below). | Implemented |
| Core | `src/core/` | Phase constants. | Implemented (minimal) |
| Coordination | `src/coordination/` | Redis leases, fencing, notification. | Planned (later phase) |
| Concurrency | `src/concurrency/` | In-flight map, waiter multiplexing. | Planned (later phase) |

Rules: I/O threads never block (DB work hops to the pool and posts back);
persistence never imports networking; the pool holds no state and makes no
decisions (INV-17). No global mutable state anywhere.

## Request contract: POST /v1/operations (Implemented, Validated)

- Required header `Idempotency-Key`. Rules: ≤ 255 chars, charset
  `[A-Za-z0-9-_.:]`, stored **exactly as received** (no truncation, no
  normalization — either could merge distinct client keys). Missing → `400
  missing_idempotency_key`; empty/bad-characters/too-long → `400
  invalid_idempotency_key` with a `reason`.
- Body must be JSON (else `400 invalid_json_body`). Fingerprint = hex
  SHA-256 over `method + "\n" + route + "\n" + canonical_body`, where the
  route is the normalized path (`/v1/operations`, query stripped) and the
  canonical body is the JSON re-serialized with object keys sorted
  byte-wise, no insignificant whitespace. Raw bytes are NOT hashed: `{"b":2,
  "a":1}` and `{"a":1,"b":2}` are the same logical request.
- Canonicalization limits (documented, not hidden): `1` vs `1.0` differ,
  duplicate object keys resolve parser-last-wins, non-JSON bodies rejected.

## HTTP semantics (Implemented, Validated end-to-end + by race tests)

| Situation | Response |
|---|---|
| Missing / invalid key, invalid JSON | `400` with a machine-readable `error` |
| First request (acquire wins) | Executes now → `200` (or `500` when the op itself fails) |
| Duplicate, same fingerprint, `PROCESSING` | `202 {"status":"processing"}` — deliberate, no waiting |
| Duplicate, same fingerprint, `COMPLETED` | Stored `http_status` + body replayed byte-identically |
| Duplicate, same fingerprint, `FAILED` | `409 idempotency_already_failed` with the original failure |
| Same key, different fingerprint | `409 idempotency_key_in_use` — never executed as the original |
| Database unreachable | `503 storage_unavailable`; gateway stays up (`/health` unaffected) |
| Wrong method on the route | `405` + `Allow: POST` |

## Request lifecycle today (Implemented, Validated by tests)

```text
accept -> Session::do_read (30 s idle timeout, 1 MiB body cap)
  -> parse error?   400 JSON, close
  -> body too big?  413 JSON, close
  -> GET /ready?    async TCP probes (2 s deadline each) -> 200/503 JSON
  -> POST /v1/operations?
       validate key/fingerprint inline (pure CPU)
       service missing? 503
       post blocking service->handle() to DB pool, suspend (INV-01)
       resume on session executor -> respond 200/202/400/409/500/503 JSON
  -> else Router    -> 200 / 404 / 405 JSON
  -> keep-alive?    next read : close
```

Shutdown order: stop acceptor → stop `io_context` → join I/O workers →
join DB pool → close connection pool. Each stage outlives its users.

## Logging policy (Implemented)

Transition logs carry: truncated key (first 16 chars + length), full
fingerprint hex (a hash is safe to log), previous → new state, outcome, and
latency in ms. Bodies are never logged; conninfo/passwords are never
logged; invalid passwords are never echoed even as warnings. Rationale:
keys are opaque client tokens that may embed account data — a prefix is
enough to correlate, the fingerprint is enough to identify the request.

## Decisions made in Phase 1

1. **Raw `libpq`, not an ORM or heavier wrapper.** Explicit connections,
   explicit transactions, parameterized statements — the durability layer
   must show its mechanics, not hide them. (vcpkg `libpq`, CMake
   `PostgreSQL::PostgreSQL`.)
2. **OpenSSL SHA-256 for fingerprints** (`OpenSSL::Crypto`). Never hand-roll
   hashing where a collision corrupts dedup.
3. **Key as PRIMARY KEY, no surrogate id.** The uniqueness invariant IS the
   primary key; every access is by key, so no secondary index exists.
4. **Status predicate (`WHERE status='PROCESSING'`) instead of a version
   column** for terminal guards. One writer per acquisition exists in Phase
   1, so the predicate is sufficient; epochs/versioning arrive with the
   coordination phase (V002).
5. **Lazy pool + best-effort schema ensure at boot.** A dead database means
   `503`s on the operations route, not a refusal to boot — `/health` and
   `/ready` keep Phase 0 behavior. Migrations stay a single idempotent
   `.sql` file applied by both server and tests (no version table until V002).
6. **VCPKG_INSTALLED_DIR relocated outside the repo** (`%LOCALAPPDATA%`).
   Meson/pkg-config ports (libpq) emit backslash-escaped include paths that
   MSVC cannot parse when the tree lives under a path with spaces/`&` (this
   repo's directory name). The manifest stays the single source of truth;
   only the local artifact directory moves. See `scripts/configure.ps1`.

## Deliberately deferred

- Redis client/leases/pub-sub/streams, fencing tokens/epochs, waiter
  registry/multiplexing, multi-node coordination, distributed recovery,
  key expiry/GC, metrics/tracing, TLS, auth, rate limiting, benchmarks.
