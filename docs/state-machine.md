# Apex Idempotency Record State Machine

Status: **Implemented (Phase 3): durable core + lease ownership + fencing
epochs + in-flight multiplexing. Request roles below are runtime roles,
not durable states — the table still has exactly three states.**

## Durable states vs request roles

Durable (PostgreSQL): `PROCESSING`, `COMPLETED`, `FAILED` — unchanged.
No `WAITING` state exists and none is needed: waiting observes state, it
is not state.

Runtime roles per request: `OWNER` (holds a valid lease + current epoch and
executes), `WAITER` (same key+fingerprint, another generation owns — suspends
without executing, writing, or fencing), `REPLAYER` (terminal row observed),
`CONFLICT` (fingerprint mismatch). A waiter that observes a dead owner
promotes to owner through the standard Phase 2 recovery CAS — waiting and
recovery are different operations sharing one decision engine.

## Record lifecycle (one row per idempotency key, durable in PostgreSQL)

```text
lease won? (SET key token NX PX ttl — Redis, atomic)
    |
    +-- no (held) ------> 202, create nothing (crash window A: the holder
    |                      may not have inserted yet — still "in progress")
    |
    +-- no (Redis down) -> 503 redis_unavailable, create nothing (fail closed)
    |
    +-- yes ------------> atomic acquire (INSERT … ON CONFLICT DO NOTHING,
                          epoch 1, one transaction)
                              |
                              +-- inserted ---> PROCESSING(epoch=1)
                              |                     |
                              |        execute, then terminal write, fenced:
                              |        UPDATE … WHERE status='PROCESSING'
                              |          AND fp=:fp AND fencing_epoch=:epoch
                              |            |                    |
                              |            v                    v
                              |      COMPLETED(epoch=1)    FAILED(epoch=1)
                              |            |                    |
                              |            v                    v
                              |      replay stored         409 with original
                              |      response             failure attached
                              |
                              +-- conflicted --> row exists: run the
                                                 ownership check below
```

Ownership check on a found `PROCESSING` row (same fingerprint; anything
else is replay/conflict/failed-terminal without touching Redis):

```text
lease key present? --yes--> 202 (active owner; non-waiting)
    |
    no (hint only — INV-09, never proof of ownerlessness)
    |
    win the lease? --no--> 202 (someone just took it; they own recovery)
    |
    yes: CAS fencing_epoch observed+1 (row-locked transaction)
        |
        +-- won (epoch N) --> execute as generation N --> fenced terminal
        |                     write with epoch N --> 200/500, release lease
        +-- lost -----------> re-read durable row --> 202 / replay / 409
```

- Only `PROCESSING → COMPLETED` and `PROCESSING → FAILED` exist. Terminal
  states are final: the epoch-guarded UPDATE affects 0 rows on terminal or
  superseded rows, so replays, retries, recoveries, and late writes cannot
  move them (tested: TEST F, F4–F9, T1–T7).
- Fingerprints are immutable after creation: recovery advances the epoch
  but never adopts a foreign fingerprint (409 regardless of lease state).
- There is no `PENDING` state and no epoch column in Phase 1. The Phase 0
  draft named them against a Redis-lease design that is explicitly deferred;
  V002 will add ownership columns when the coordination phase lands.

## Transaction boundaries (each is one libpq transaction unless noted)

| Transition | Transaction | On rollback / crash |
|---|---|---|
| none → `PROCESSING` (epoch 1) | Lease `SET NX PX` won, then `BEGIN` → `INSERT … ON CONFLICT DO NOTHING RETURNING` → `COMMIT` (or `ROLLBACK` when conflicted) | No row exists. A crash before `COMMIT` is indistinguishable from "never arrived": the client retries with the same key. A crash between lease-win and `INSERT` leaves a lease with no row (window A): duplicates defer with 202 until the TTL frees it. |
| `PROCESSING → PROCESSING` (epoch N → N+1) | `BEGIN` → `SELECT … FOR UPDATE` → eligibility check → `UPDATE … SET fencing_epoch = fencing_epoch + 1 … AND fencing_epoch = $observed` → `COMMIT`. Exactly one winner per observed epoch (INV-19). | Aborted recovery commits nothing; the epoch is untouched and another generation may try. |
| `PROCESSING → COMPLETED` | Single-statement autocommit `UPDATE … WHERE status='PROCESSING' AND fingerprint=… AND fencing_epoch=…` | Atomic by construction: either the full result payload lands or nothing does. A superseded epoch affects zero rows (FENCING INVARIANT). |
| `PROCESSING → FAILED` | Same shape as COMPLETED, storing `error_code`/`error_message`. | Same atomicity. |
| Concurrent losers | Block inside their `INSERT` on the winner's uncommitted key, then read the settled row. Observable states are only ever committed ones. | — |

## Crash windows (analyzed per system pair; tested in cross_system_test.cpp)

| Window | Durable PG state | Redis state | Expected behavior |
|---|---|---|---|
| Lease acquired, crash before PG persist (A) | No row | Key held until TTL | Duplicates defer 202; after expiry a new owner proceeds to 200. Retry safe. |
| Epoch persisted, crash before execution (B) | `PROCESSING` at new epoch | Lease may be held or expired | Recoverable again (generations keep advancing); stale epochs rejected. |
| Lease expires mid-execution (C) | `PROCESSING` at owner's epoch | Key free | Next duplicate recovers (epoch + 1) and completes; late owner fenced (T1–T7). |
| Stale owner commits after new epoch (T7) | Unchanged (0 rows) | Unchanged | Stale result discarded; current result authoritative. |
| Redis down during ownership attempt | No row created | Unreachable | 503 `redis_unavailable`, fail closed, no orphan. |
| PostgreSQL down during ownership persist | Nothing committed (aborted txn) | Lease released on exit | 503 `storage_unavailable`; epoch untouched; fresh connection recovers. |
| Crash mid-`UPDATE` | Impossible to observe half-written (single statement) | — | — |
| Owner crashes before durable completion | `PROCESSING` | lease may remain/expire | Waiters suspend; recovery via Phase 2 takes ownership; waiters converge. |
| PG commit succeeds, publish fails | Terminal | no notification | Correctness intact: waiters converge via fallback re-check. |
| Publish succeeds, waiter misses it | Terminal | notification sent | Fallback re-check finds the row; reconnect sweep prods deaf waiters. |
| Waiter starts after completion | Terminal | Irrelevant | Immediate replay, never registers. |
| Redis unavailable, terminal exists | Terminal | Unreachable | Replay works (INV-MUX-10). |
| Redis unavailable during `PROCESSING` | `PROCESSING` | Unreachable | 503 fail-closed (documented fallback semantics). |
| Stale owner resumes | Terminal/newer epoch | Old lease invalid | Fenced write rejected (INV-MUX-07). |
| Waiter exceeds deadline | `PROCESSING` (unchanged) | Any | Waiter exits 202; durable state, lease, epoch untouched (INV-MUX-06). |
| Process shutdown with waiters | Any | Subscriber closes | Registry shutdown wakes all (202); timers abort; clean exit (P3-18). |
| Process restart | Committed rows survive | Subscriptions re-established | Waiter state rebuilds from PostgreSQL (INV-MUX-11). |

## Residual honest limits

- An orphaned `PROCESSING` row still waits for the next same-key request
  (or operator action) — no background healing. It is *visible*
  (`completed_at IS NULL`) and *honest* (duplicates wait or recover, never
  see a fabricated result).
- A waiter can only converge as fast as its wake-ups: pub/sub normally,
  fallback interval at worst, deadline at latest.

## Out of scope

Key expiration/GC policy and response-size caps beyond the 1 MiB request
limit are later-phase work and will extend this document — not contradict it.
