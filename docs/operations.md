# Apex Operations Guide (Phase 5)

How to run, watch, and recover Apex. Nothing here is aspirational: every
behavior is implemented and tested; limits point at the tests that prove
them.

## Architecture in one picture

```text
Client
  |
  v
HTTP / Beast (Asio I/O threads — never block)
  |
  v
IdempotencyService ──> PostgreSQL (durable truth: state, fingerprint,
  |                     response, fencing_epoch)
  +──> Redis lease ──> Redis (ownership liveness: SET NX PX, Lua release)
  +──> Redis pub/sub ─> Redis (wake-only notifications + reconnect sweep)
  +──> WaiterRegistry (process-local waiter slots; optimization only)
  +──> OrphanReaper (background adoption via the same service path)
```

## Startup order

1. Load + validate config (exit 2 on invalid; warnings logged, never
   containing secrets).
2. Log one `apex node=… config: …` line (ports, pools, TTLs, timeouts —
   no passwords, no bodies).
3. Best-effort schema ensure (`Schema::ensure`, idempotent V001→V003).
4. Start subscriber, reaper, listener, I/O pool — in that order.

## Dependencies

- **PostgreSQL (durable truth).** Required for everything except process
  liveness. Unreachable ⇒ `503 storage_unavailable` on ownership paths;
  replays need the row too (same 503). No phantom successes ever: terminal
  writes are single atomic guarded statements.
- **Redis (liveness + wake-up).** Required for NEW ownership (fail closed:
  `503 redis_unavailable`, nothing created). NOT required for replay,
  conflict answers, or failed-terminal answers. Total loss costs wake-up
  latency and recovery-by-traffic only — never correctness.
- **Lease behavior.** `SET key token NX PX <APEX_LEASE_TTL_MS>` elects who
  may try; token-guarded Lua release frees exactly our own lease; TTL is
  the backstop. No renewal: overrunning owners keep epoch authority unless
  superseded (fencing decides, not the clock).
- **Fencing.** `fencing_epoch` per key, 1 at creation, previous+1 per
  recovery CAS. Terminal writes carry `AND fencing_epoch = $N`; stale
  generations affect zero rows and answer `409 stale_ownership_epoch`.
- **Waiter behavior.** Duplicates of an active owner suspend with no thread
  held (registry slot + one Asio timer). Wake paths: local notify, pub/sub,
  fallback timer, reconnect sweep. Deadline (`APEX_WAITER_TIMEOUT_MS`)
  answers `202` and mutates nothing. Cap (`APEX_MAX_WAITERS_PER_KEY`)
  answers `202` immediately past 1024 waiters/key.
- **Recovery.** Traffic-driven (duplicate finds orphan → CAS → execute) and
  background (`OrphanReaper` every `APEX_REAPER_INTERVAL_MS`, at most
  `APEX_REAPER_BATCH_SIZE` candidates idle past
  `APEX_REAPER_ELIGIBLE_AFTER_MS`, via the partial index). Both funnel
  through `IdempotencyService::handle` — one ownership mechanism, and the
  epoch CAS admits exactly one winner per generation.

## Shutdown

`SIGINT`/`SIGTERM` (or TestServer teardown) runs a fixed order: stop
acceptor → stop reaper (join bounded) → registry shutdown (active waits get
`202` while the loop runs) → subscriber stop (join bounded by socket read
timeout) → stop `io_context` → join I/O workers → join DB pool → close
pools. No stage hangs: timers abort, threads join, guards release.

## Failure handling

- **Redis dies?** Ownership paths 503 fail-closed (nothing created, no
  orphan); replays/conflicts keep working; waiters fall back to durable
  re-checks; subscriber reconnects with backoff + sweep; leases held by the
  dead period expire via TTL.
- **PostgreSQL dies?** 503s on every durable path; in-flight transactions
  abort (nothing partial commits); pool discards dead connections and
  reconnects; epochs untouched; recovery resumes after restart.
- **Owner dies?** Orphan row (visible: `completed_at IS NULL`) recovers by
  traffic or reaper; a merely-stalled owner is fenced on wake (409).
- **Pub/Sub messages lost?** Fallback timers + reconnect sweeps converge
  every waiter on the durable row. Loss costs latency, never correctness.
- **Waiter disconnects?** Session death releases only its registry slot
  (owner, lease, epoch, row untouched). Verified with 100 waiters / 50
  abrupt disconnects converging on one execution.
- **After restart?** Committed rows replay; orphans recover; stale epochs
  stay rejected; schema ensure is idempotent. See `scripts/restart-smoke.ps1`.

## Configuration

All settings (`APEX_*`, see README + `Config.hpp` header): name, default,
valid range, startup validation, and safe failure behavior (fallback +
warning, or exit 2 for structural errors). Secrets (`*_PASSWORD`) are never
logged — invalid values produce warnings without echo.

## Health/readiness

- `GET /health` → process liveness only. Never touches dependencies.
- `GET /ready` → TCP reachability of PostgreSQL AND Redis within 2 s.
  Redis is included deliberately: new ownership fail-closes without it, so
  a gateway that cannot coordinate must not report ready — even though
  completed replay would still work. (Audited Phase 5: semantics match the
  fail-closed serving policy.)
- `GET /metrics` → Prometheus-text process counters (fixed cardinality —
  no request data, keys, or secrets ever rendered).

## Logs

One line per meaningful event with `key=<safe-prefix>` (first 16 chars +
length), `fp=<full sha256 hex>` (safe: a hash), `cid=<correlation ID>`,
epoch transitions, outcomes, and `latency_ms`. Waiter lifecycle emits
`idempotency.wait.started/woken/timeout/completed`. Bodies, responses,
passwords, and tokens (beyond short diagnostic prefixes where noted) never
appear. Startup emits the node + effective-config line.

## Metrics

Fixed counter set (`MetricsSnapshot`, rendered at `/metrics`): requests,
validation failures, conflicts, replays, waiters started/timeouts/aborted,
lease acquired/held/unavailable/releases/rejected, recovery
attempts/wins/losses, stale rejections, executions completed/failed,
notification vs fallback wakeups, cancellations, PG/Redis failures,
subscriber reconnects, reaper passes, orphans found/recovered, recovery
conflicts. No per-key labels — cardinality is constant by construction
(tested: live keys absent from rendered output).

## Common failure scenarios

| Symptom | Likely cause | Check |
|---|---|---|
| `503 redis_unavailable` on new keys, replays fine | Redis down / pool exhausted | `docker compose ps`, `apex_redis_failures`, reconnect log |
| `503 storage_unavailable` everywhere | PostgreSQL down | `apex_pg_failures`, pg logs, `/ready` |
| Growing `PROCESSING` rows, replays fine | Owners crashing / TTL too short for op duration | `completed_at IS NULL` age, `orphans_found` vs `orphans_recovered` |
| `409 stale_ownership_epoch` spikes | Slow owners + aggressive recovery (short TTL) | `stale_rejections`, owner `latency_ms` vs TTL |
| Waiter `202`s under load | Owner slower than waiter deadline, or cap hit | `waiter_timeouts`, owner latency, `max_waiters_per_key` |
| Reaper never recovers | `eligible_after` too high / `request_body` NULL (pre-V003 rows) | `reaper_passes` vs `orphans_found`, row columns |
