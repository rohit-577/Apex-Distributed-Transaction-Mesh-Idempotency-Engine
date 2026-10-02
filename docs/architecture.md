# Apex Architecture

Status legend used across all docs: **Implemented** · **Planned** · **Validated** (observed by a test or a run) · **Not validated**.

## Purpose

Apex is a production-quality *educational* distributed HTTP gateway in C++20
whose job is to demonstrate correct idempotency handling under concurrency
and failure. It is not a CRUD app and not a generic REST API: every design
choice serves the question "what happens when the same write arrives twice,
at the same time, while machines fail?"

## Phase 3 scope (this document describes intent + current status)

Phase 3 adds **in-flight request multiplexing**: duplicates of an active
owner suspend asynchronously (no thread held) and converge on the owner's
durable result via a process-local waiter registry, Redis pub/sub wake-up
(wake-only), and fallback durable re-checks. Full mechanics below and in
`docs/lease-and-fencing.md`.

Explicitly NOT in Phase 3 (later phases): Redlock/quorum, Streams, renewal,
background orphan reaping, metrics/tracing, TLS/auth. Multiplexing changes
duplicate-while-PROCESSING from "202 immediately" to "wait, then the final
result" — the 202 remains only for timeouts, shutdown, caps, and the no-row
window-A case. This phase provides **durable idempotency with lease-based
ownership, fencing, and single-execution fan-in** — not distributed
exactly-once execution (see INV-10 and §11 of
`docs/lease-and-fencing.md`).

## Topology today (Implemented, Validated)

```text
HTTP client(s)
      |
      v
C++ async gateway (Asio/Beast, I/O thread pool; NEVER blocks — INV-01)
      |  POST /v1/operations: validate -> fingerprint -> post to DB pool
      v
Database worker threads (blocking allowed HERE only: PG + Redis alike)
      |
      v
IdempotencyService -> IdempotencyRepository -> PostgreSQL 18 (libpq)
      |       |             (durable authority, INV-08/INV-17, INV-20)
      |       +----------> LeaseManager -> RedisClient -> Redis 7 (redis-plus-plus)
      |                        (liveness only: apex:lease:<key>, TTL, tokens)
      |       +----------> WaiterRegistry (process-local waiter slots)
      |       +----------> publish completion -> apex:w:<hash> (wake-only)
      v                    CompletionSubscriber (one thread, psubscribe
SimulatedOperation          apex:w:*, reconnect sweep) -> registry -> sessions
  (or injected test executor; counts executions deterministically)
```

Redis holds lease liveness plus wake-up fan-out (single `redis:7` dev
instance — stated plainly, not Redlock). Nothing replayable depends on it:
terminal answers come from PostgreSQL without touching Redis.

## Module boundaries (Implemented unless marked)

| Area | Path | Responsibility | Status |
|---|---|---|---|
| Entry | `src/main.cpp` | Config → pool/service/DB threads → schema ensure → serve → ordered shutdown. No business logic. | Implemented |
| Config | `src/config/` | Env-based config + validation + libpq conninfo builder. Only module that reads the environment. | Implemented |
| API routing | `src/api/` | Pure (method, target) → result mapping. No I/O, no state. Does NOT own `/ready` or `/v1/operations` (dispatched by Session first). | Implemented |
| Execution | `src/execution/` | `HttpServer` (accept loop), `Session` (connection + async dispatch to service/DB pool), `DependencyChecker` (async probes). No business logic. | Implemented |
| Idempotency | `src/idempotency/` | Key rules, fingerprinting, operation-executor seam, orchestration service, process-local waiter registry (optimization only). No sockets, no SQL. | Implemented |
| Persistence | `src/persistence/` | `PgConnection` (RAII libpq), `ConnectionPool` (bounded checkout), `IdempotencyRepository` (the only SQL), `Schema` (migration applier). No HTTP, no orchestration. | Implemented |
| Observability | `src/observability/` | Tiny thread-safe stderr logger + transition logging with key-truncation policy (below). | Implemented |
| Core | `src/core/` | Phase constants. | Implemented (minimal) |
| Coordination | `src/coordination/` | `RedisClient` (only Redis connections in the codebase), `LeaseManager` (`SET NX PX`, Lua compare-delete, owner tokens), `CompletionSubscriber` (one thread, pattern-subscribe wake-ups, reconnect sweep). No Streams, no Cluster. | Implemented |
| Concurrency | `src/concurrency/` | In-flight map, waiter multiplexing. | Planned (later phase) |

Rules: I/O threads never block (DB *and* Redis work hop to the pool and
post back); persistence never imports networking; coordination never imports
SQL or HTTP; the pools hold no state and make no decisions (INV-17). No
global mutable state anywhere.

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
| First request (lease won, epoch 1) | Executes now → `200` (or `500` when the op itself fails) |
| Duplicate, same fingerprint, active owner | Waits (no thread held) → owner's final result, byte-identical for all |
| Duplicate, same fingerprint, orphaned | Recovers to epoch N+1, executes → `200` (fellow waiters converge too) |
| Waiter exceeds deadline / shutdown / cap / no-row window | `202 {"status":"processing"}` — transient; durable state untouched |
| Duplicate, same fingerprint, `COMPLETED` | Stored `http_status` + body replayed byte-identically (no Redis touched) |
| Duplicate, same fingerprint, `FAILED` | `409 idempotency_already_failed` with the original failure |
| Superseded owner commits late | `409 stale_ownership_epoch`; current result stands |
| Same key, different fingerprint | `409 idempotency_key_in_use` — never executed as the original |
| Database unreachable | `503 storage_unavailable`; gateway stays up (`/health` unaffected) |
| Redis unreachable (ownership needed) | `503 redis_unavailable`; fail closed, nothing created |
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
       post blocking service->handle() to DB pool (INV-01)
       verdict terminal/conflict/stale/unavailable? respond now
       verdict WAIT? register in WaiterRegistry + arm ONE timer
         (min(recheck, remaining)), suspend with NO thread held
         wake (local notify / pub-sub / timer) -> re-run handle() on pool
         terminal? respond identical bytes : still owned? re-arm : 202 on deadline
  -> else Router    -> 200 / 404 / 405 JSON
  -> keep-alive?    next read : close
```

Shutdown order: stop acceptor → registry shutdown (waiters 202 while the
loop runs) → subscriber stop/join → stop `io_context` → join I/O workers →
join DB pool → close pools. Each stage outlives its users; timer aborts
settle-destroy sessions that outlive the loop.

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

## Decisions made in Phase 2

1. **redis-plus-plus (sync) for coordination.** Async-first clients
   (cpp-redis) and Asio-native ones (boost-redis) mismatch the
   block-on-workers model; raw hiredis would need hand-rolled pooling.
   redis-plus-plus gives pooled sync commands, `EVAL` for the release
   script, and a future `Subscriber` for pub/sub — used from worker
   threads only. (vcpkg `redis-plus-plus`, CMake `redis++::redis++`.)
2. **Lease-first, then row.** Redis `SET NX PX` precedes the PostgreSQL
   INSERT so a dead coordinator creates no orphan rows; losers defer with
   `202` without touching the database.
3. **Replay paths never touch Redis.** Terminal/conflict answers come from
   the row alone — Redis outage degrades ownership, never replay.
4. **Always-release leases (RAII).** No lease handoff exists in Phase 2, so
   every exit path releases; token-guarded no-throw release makes this
   unconditionally safe. TTL remains the backstop.
5. **No renewal, no version table.** Renewal buys nothing while epochs
   decide correctness; the two-file ordered migration list stays until the
   count justifies a runner.

## Decisions made in Phase 3

1. **Wait verdict + async session loop, not blocking waits.** `handle()`
   stays fast and non-waiting; the Session suspends on a registry slot +
   one Asio timer and re-invokes the SAME decision engine per cycle — so
   waiters that observe a dead owner transparently become recoverers, with
   no second code path to drift.
2. **Register-then-always-recheck (no completed flags).** An earlier design
   remembered notified slots; it failed review (a completion with zero
   waiters left no trace for late registrants). The mandatory immediate
   re-check after every registration closes the race unconditionally and
   deletes an entire class of state.
3. **One subscriber thread, pattern subscription, reconnect sweep.** A
   dedicated `sw::redis::Subscriber` connection (commands untouched);
   socket-read timeouts bound stop latency; every reconnect sweeps all
   waiters to re-check (missed-notification windows become immediate
   convergence instead of hangs).
4. **Operation-executor seam.** Counting/gated test executors prove
   exactly-once fan-in without touching production paths; the seam is
   independently justified (real operations will replace simulation).
5. **Timeout/recheck/cap configuration.** 30 s / 1 s / 1024 defaults;
   timeouts answer 202 and mutate nothing.

## Deliberately deferred

- Redis Streams, renewal, Redlock or
  any multi-instance Redis, background orphan reaping, key expiry/GC,
  metrics/tracing, TLS, auth, rate limiting, benchmarks.
