# Apex Idempotency Record State Machine

Status: **Implemented (Phase 2): durable core + lease ownership + fencing
epochs. Waiter multiplexing is the next phase and is marked as such.**

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

## Crash windows (honest, not solved)

- **Crash between acquire-commit and terminal write:** a `PROCESSING` row is
  orphaned. It answers `202` to duplicates until the later lease-recovery
  phase reaps or adopts it. Operators can spot these rows (`completed_at IS
  NULL`, old `created_at`). Distributed lease recovery is explicitly out of
  scope for Phase 1.
- **Crash mid-`UPDATE`:** impossible to observe half-written — single
  statement, single row.
- **Process death with in-flight HTTP sessions:** in-memory waiters die;
  committed rows survive; clients retry with the same key and converge on
  the terminal response.

## Out of scope

Key expiration/GC policy and response-size caps beyond the 1 MiB request
limit are later-phase work and will extend this document — not contradict it.
