# Apex Failure Model

Status: **Phase 0 realities are Implemented; everything about leases,
owners, and recovery is Declared for Phase 1.** This document defines what
the system must survive and what it is allowed to sacrifice.

## Failures in scope

1. **Duplicate delivery** — same key arrives twice (retry, double-click,
   proxy replay), concurrently or sequentially.
2. **Owner crash mid-execution** — a `PENDING` row outlives its worker.
3. **Lease expiry with a live owner** — the owner is slow (GC, scheduling,
   network stall), the lease lapses, a successor takes over. The original
   owner is now *stale* and must be fenced (INV-09).
4. **Missed waiter notification** — Redis pub/sub drops a message, a waiter
   subscribes late, or Redis restarts. Waiters recover by re-reading durable
   state (INV-07); latency suffers, correctness does not.
5. **Redis loss** — full data loss or partition. Coordination degrades to
   polling PostgreSQL; no committed result is lost (INV-08).
6. **PostgreSQL unavailability** — new work cannot start safely; `/ready`
   reports `503`; in-flight work waits or fails without recording phantom
   successes.
7. **Process crash / SIGKILL of the gateway** — in-memory waiter state dies;
   recovery is reconstruction from PostgreSQL on restart plus client retry
   with the same key.
8. **Slow network (gray failure)** — probes and waits always carry deadlines
   (Phase 0: 2 s `/ready` probes, 30 s idle timeout); nothing waits forever.

## What Phase 0 actually survives (Implemented, Validated)

- Malformed bytes on a connection: contained to that connection (400/close),
  process keeps serving (tested).
- Dependency processes down: `/ready` → `503` with per-dependency detail
  (tested with closed ports); liveness `/health` stays `200`.
- SIGINT/SIGTERM: ordered drain, no torn responses, exit 0 (exercised on
  every test-fixture teardown).
- Invalid configuration: refusal to start, exit 2 (tested).

## What Phase 0 does NOT survive (accepted, documented)

- Anything requiring durability: there is no state yet, so a crash loses
  nothing *and* guarantees nothing — the gateway is stateless by design.
- Redis/PostgreSQL outages beyond reporting them honestly on `/ready`.

## Recovery principles (binding on Phase 1+)

- **Durability before acknowledgement**: a result is returned only after its
  terminal record is committed.
- **Fencing before writing**: stale owners fail their epoch check and
  re-read; they never overwrite.
- **Polling is the correctness path; notification is the fast path.**
- **Client retry with the same key is always safe** (terminal states replay
  stored responses; `PENDING` re-attaches).
