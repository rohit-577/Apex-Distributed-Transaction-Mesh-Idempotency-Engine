# Apex Idempotency Record State Machine

Status: **Implemented (Phase 1) for the durable core. Fencing epochs,
waiters, and lease recovery are later-phase work and are marked as such.**

## Record lifecycle (one row per idempotency key, durable in PostgreSQL)

```text
atomic acquire (INSERT … ON CONFLICT DO NOTHING, one transaction)
    |
    +-- inserted -----> +------------+
    |                   | PROCESSING | ---(owner executes operation)---+
    |                   +------------+                                |
    |                       |                                       |
    |          terminal write, guarded:                             |
    |          UPDATE … WHERE status='PROCESSING' AND fp=:fp        |
    |              |                          |                      |
    |              v                          v                      |
    |        +-----------+              +-----------+                |
    |        | COMPLETED |              |  FAILED   |<---------------+
    |        +-----------+              +-----------+
    |              |                           |
    |              v                           v
    |        replay stored                409 with original
    |        response                     failure attached
    |
    +-- conflicted ---> read winner's committed row, then:
                           same fingerprint + PROCESSING -> 202
                           same fingerprint + COMPLETED  -> replay
                           same fingerprint + FAILED     -> 409
                           different fingerprint       -> 409 conflict
```

- Only `PROCESSING → COMPLETED` and `PROCESSING → FAILED` exist. Terminal
  states are final: the guarded UPDATE affects 0 rows on a terminal record,
  so replays, retries, and late writes cannot move it (tested: TEST F).
- There is no `PENDING` state and no epoch column in Phase 1. The Phase 0
  draft named them against a Redis-lease design that is explicitly deferred;
  V002 will add ownership columns when the coordination phase lands.

## Transaction boundaries (each is one libpq transaction unless noted)

| Transition | Transaction | On rollback / crash |
|---|---|---|
| none → `PROCESSING` | `BEGIN` → `INSERT … ON CONFLICT DO NOTHING RETURNING` → `COMMIT` (or `ROLLBACK` when conflicted, then autocommit `SELECT`) | No row exists. A crash before `COMMIT` is indistinguishable from "never arrived": the client retries with the same key. |
| `PROCESSING → COMPLETED` | Single-statement autocommit `UPDATE … WHERE status='PROCESSING' AND fingerprint=…` | Atomic by construction: either the full result payload lands or nothing does. |
| `PROCESSING → FAILED` | Same shape as COMPLETED, storing `error_code`/`error_message`. | Same atomicity. |
| Concurrent losers | Block inside their `INSERT` on the winner's uncommitted key, then read the settled row. Observable states are only ever committed ones. | — |

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
