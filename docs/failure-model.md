# Apex Failure Model

Status: **Phase 0 + Phase 1 realities are Implemented; leases, waiters, and
distributed recovery remain Declared for later phases.** This document
defines what the system must survive and what it is allowed to sacrifice.

## Failures in scope

1. **Duplicate delivery** — same key arrives twice (retry, double-click,
   proxy replay), concurrently or sequentially. *Phase 1: handled durably
   (replay / 202 / 409 per state).*
2. **Owner crash mid-execution** — a `PROCESSING` row outlives its worker.
   *Phase 1: the row stays `PROCESSING` (orphan); duplicates get `202`.
   Reaping/adoption is lease-phase work, not pretended here.*
3. **Lease expiry with a live owner** — *later phase* (no leases exist yet;
   single-writer-per-acquisition holds instead, guarded by INV-15).
4. **Missed waiter notification** — *later phase* (no notification exists
   yet; there is nothing to miss — duplicates re-read the row every time).
5. **Redis loss** — *vacuous in Phase 1*: nothing is stored in Redis, so
   there is nothing to lose (INV-08 holds trivially).
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

## What Phase 1 actually survives (Implemented, Validated)

Everything from Phase 0, plus:

- Duplicate INSERT races: exactly one row, loser reads the winner (tested
  at raw-SQL level and through 2-way / 50-way HTTP fan-in).
- Transaction rollback: aborted work leaves no row (tested).
- Terminal-state writes after the fact: 0 rows affected, row intact
  (tested, including wrong-fingerprint attempts).
- Schema-level smuggling: CHECK constraints reject bad states even via raw
  SQL (tested, SQLSTATE 23514).
- Database process down: controlled `503`s, gateway keeps serving `/health`
  (tested live and in-suite).
- Malformed keys/bodies: `400`s before any database interaction (tested).

## What Phase 1 does NOT survive (accepted, documented)

- An orphaned `PROCESSING` row never resolves by itself — lease recovery is
  later-phase work. The row is *visible* (`completed_at IS NULL`) and
  *honest* (`202`, never a fabricated result), but not self-healing.
- Redis/PostgreSQL outages beyond reporting them and degrading safely.
- Anything requiring cross-node agreement: there is exactly one authority
  (this database) and no nodes to agree yet.

## Recovery principles (binding on later phases)

- **Durability before acknowledgement**: a result is returned only after its
  terminal record is committed.
- **Fencing before writing**: stale owners fail their epoch check and
  re-read; they never overwrite. *(Epochs arrive with coordination.)*
- **Polling is the correctness path; notification is the fast path.**
- **Client retry with the same key is always safe** (terminal states replay
  stored responses; `PROCESSING` re-observes).
