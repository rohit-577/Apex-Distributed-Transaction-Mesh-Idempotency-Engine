# Apex Failure Model

Status: **Phase 0–2 realities are Implemented; waiter multiplexing,
notification, and distributed recovery beyond orphan-epoch advancement
remain Declared for later phases.** This document defines what the system
must survive and what it is allowed to sacrifice. The crash-window matrix
lives in `docs/state-machine.md`; `docs/lease-and-fencing.md` §8–10 covers
the Redis/PostgreSQL/crash semantics in depth.

## Failures in scope

1. **Duplicate delivery** — same key arrives twice (retry, double-click,
   proxy replay), concurrently or sequentially. *Phase 1: handled durably
   (replay / 202 / 409 per state).*
2. **Owner crash mid-execution** — a `PROCESSING` row outlives its worker.
   *Phase 2: the next same-key request recovers (epoch + 1) and completes;
   a merely-stalled owner is fenced on wake (T1–T7). Orphans still wait for
   traffic — no background reaping.*
3. **Lease expiry with a live owner** — *Phase 2: enforced.* The expired
   generation keeps running but its terminal write presents a superseded
   epoch and affects zero rows (INV-20, FENCING INVARIANT). TTL expiry is
   the hint; the epoch is the verdict.
4. **Missed waiter notification** — *later phase* (no notification exists
   yet; there is nothing to miss — duplicates re-read the row every time).
5. **Redis loss** — *Phase 2: ownership fails closed (`503
   redis_unavailable`, nothing created); replay/conflict/failed paths never
   touch Redis and keep working (INV-08 no longer vacuous — tested with
   Redis down). The single dev instance is stated plainly; this is not
   Redlock and never claims quorum behavior.*
6. **PostgreSQL unavailability** — new work cannot start safely; operations
   answer `503 storage_unavailable` without recording phantom successes;
   the gateway stays up. *Implemented, tested (unreachable-DB service + raw
   connect failure).*
7. **Process crash / SIGKILL of the gateway** — in-flight HTTP state dies;
   committed rows survive; clients retry with the same key and converge on
   the terminal response. *Implemented to the extent Phase 1 promises:
   durability of committed records, not waiter resurrection.*
8. **Slow network (gray failure)** — probes, waits, and connection attempts
   always carry deadlines (`/ready` 2 s probes, 30 s idle timeout, libpq
   `connect_timeout=5`); nothing waits forever. *Implemented, tested.*

## What Phase 2 actually survives (Implemented, Validated)

Everything from Phase 1, plus:

- Lease contention: exactly one holder per key; losers told so (R2/R3).
- Lease expiry + safe release: generations rotate; stale tokens delete
  nothing (R4/R5, Lua compare-delete).
- Stale-owner commit after epoch advance: zero rows, current result stands
  (T1–T7 race, F5–F9).
- Concurrent recovery: one winner per observed epoch (F8, 16-way stress).
- 100-way fan-in, multi-key parallelism, mixed fingerprints: one row per
  key, consistent verdicts (stress suite).
- Redis outage: fail-closed ownership, working replay (CASE 5, R7).
- PostgreSQL mid-transaction death: aborted, epoch untouched, fresh
  connection recovers (CASE 6).
- Backend-killed connections surface as controlled `503`s, never hangs.

## What Phase 2 does NOT survive (accepted, documented)

- An orphaned `PROCESSING` row resolves only when the next same-key
  request (or operator) triggers recovery — no background healing, no
  waiting/multiplexing yet (active-owner duplicates still get `202`).
- Renewal-less long operations: an owner overrunning its TTL keeps epoch
  authority unless superseded — by design (fencing, not leases, decides).
- Anything requiring cross-node agreement beyond one Redis + one database,
  pub/sub notification, or metrics/tracing.

## Recovery principles (binding on later phases)

- **Durability before acknowledgement**: a result is returned only after its
  terminal record is committed.
- **Fencing before writing**: stale owners fail their epoch predicate and
  their results are discarded; they never overwrite. *(Enforced since
  Phase 2.)*
- **Polling is the correctness path; notification is the fast path.**
  *(Notification arrives next phase; the re-read rule already governs.)*
- **Client retry with the same key is always safe** (terminal states replay
  stored responses; `PROCESSING` re-observes or recovers).
