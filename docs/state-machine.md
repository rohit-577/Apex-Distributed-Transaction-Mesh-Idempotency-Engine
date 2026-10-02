# Apex Idempotency Record State Machine

Status: **Declared (design intent for Phase 1). Nothing here is implemented —
`POST /v1/operations` returns `501` and there is no database table yet.**
This document exists so Phase 1 implements against a reviewed contract
instead of inventing transitions under pressure.

## Record lifecycle (one row per idempotency key, durable in PostgreSQL)

```text
              create (INSERT … ON CONFLICT DO NOTHING + fencing epoch)
                |
                v
            +---------+
            | PENDING | ---(owner executes business op)---+
            +---------+                                   |
                |                                         |
   terminal write with fencing check                      |
    (UPDATE … WHERE epoch = :mine)                        |
        |                          |                      |
        v                          v                      |
   +-----------+             +-----------+                |
   | COMPLETED |             |  FAILED   |<---------------+
   +-----------+             +-----------+
        |                          |
        v                          v
   return stored              return stored
   response to                error to owner
   owner + waiters            + waiters
```

- Only `PENDING → COMPLETED` and `PENDING → FAILED` transitions exist.
  Terminal states are final: no transition out of `COMPLETED`/`FAILED`.
- A duplicate arriving while `PENDING` attaches as a **waiter** (no new
  execution, no new row).
- A duplicate arriving at a terminal state receives the **stored response
  byte-for-byte** without re-executing anything.
- Every write carries the owner's **fencing epoch**; the `UPDATE` predicate
  includes `epoch = :mine`, so a stale owner whose lease expired mid-flight
  (see `docs/failure-model.md`) updates zero rows and must re-read instead
  of overwriting the successor's state.

## Ownership / fencing epoch (declared)

- The lease grant (Redis, Phase 1) mints a monotonically increasing epoch
  per key and the owner stores it in the `PENDING` row it created or adopted.
- Epoch comparison is the *only* authority for "who may write"; wall-clock
  TTL expiry is a hint that an epoch *may* be superseded, never proof of
  ownership.

## Waiter behavior (declared)

1. Attach to the in-flight execution (in-process map; cross-process via
   Redis hint + durable poll — notification is best-effort).
2. On hint or poll tick, re-read the durable row.
3. Return only when the row is `COMPLETED`/`FAILED`; otherwise keep waiting
   up to the request deadline, then return `503`/`504` (still safe: the
   client retries with the same key and gets the terminal response later).

## Out of scope for the state machine

Key expiration/GC policy, request schema details, and response-storage size
limits are Phase 1 design tasks and will extend this document — not
contradict it.
