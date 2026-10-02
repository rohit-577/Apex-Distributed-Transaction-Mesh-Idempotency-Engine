# Apex Lease Ownership & Fencing (Phase 2)

Status: **Implemented and tested.** This document answers the thirteen
questions the phase brief requires. Claim discipline applies throughout:
no "exactly once", no "linearizable", no "fault tolerant" — only the
precise guarantee in §11.

## 1. Why Redis exists

Duplicate requests need an answer to "who executes?" that works across
gateway processes sharing nothing but the database. PostgreSQL alone can
serialize row creation, but it cannot express *liveness*: a `PROCESSING`
row with no live owner must become recoverable without waiting forever and
without human intervention. Redis provides that liveness signal — a lease
with a TTL — so a new generation can take over an orphaned operation after
the previous owner's signal dies.

## 2. Why Redis is not the durable source of truth

Every verdict the system acts on comes from PostgreSQL: row existence,
status, fingerprint, epoch. Redis answers only "is a value stored?" — a
hint that gates the recovery attempt, never the decision. If Redis loses
all data, in-flight duplications degrade to safe deferrals/unavailability
and committed results replay untouched (INV-08, tested with Redis down).

## 3. Why a random owner token is needed

After a lease expires, the previous holder may still be alive (slow, not
dead). Re-acquisition must distinguish generation N from generation N+1,
or a stale holder could delete/act on its successor's lease. PID, thread,
timestamp, and counters are all forgeable or reusable across restarts;
128-bit cryptographic randomness is the only identity that cannot collide
in practice. A fresh token is minted per acquisition attempt — even
immediate retries — so no two holders share identity.

## 4. Why safe release requires token comparison

Blind `DEL` deletes whoever holds the key *now*, not who held it when you
checked. Interleaving without atomicity: A reads token (matches) → A's
lease expires → B acquires (B's token stored) → A `DEL`s → B's lease is
destroyed while B believes it owns the operation → two concurrent owners.
The Lua compare-and-delete makes check-and-delete one atomic step, closing
exactly that interleaving (tested: R5).

## 5. Why TTL expiration can leave an old process executing

The TTL bounds the *signal*, not the *work*. Expiry fires while the owner
thread is mid-operation (slow downstream, scheduling stall, long body);
Redis has no handle on the owner's process and cannot stop it. The owner
wakes up holding a superseded epoch — and must then fail safely. This is
why the lease alone is never sufficient (principle 4) and why §6 exists.

## 6. Why fencing epochs are required

Because (§5) old owners keep running, *some* durable, monotonic,
per-operation counter must order generations so the database can tell
"current" from "superseded". Wall-clock cannot (skew, ties); TTL cannot
(it only says "signal died"); a local counter cannot (processes don't
share it). The fencing epoch in PostgreSQL — assigned previous + 1 inside
a row-locked transaction — is that counter (INV-18, INV-19).

## 7. Why PostgreSQL enforces the fencing condition

Application-level "read epoch, compare, then write" reintroduces the race
it pretends to solve (two writers can both read epoch 1, both see
"current", both write). The condition must live in the mutating statement
itself — `... AND fencing_epoch = $N` — so serialization is the database's
job, not the application's hope. The affected-row count is the verdict:
1 = this generation transitioned, 0 = superseded or already terminal
(FENCING INVARIANT, tested F4–F9 plus the T1–T7 race).

## 8. What happens when Redis fails

Fail closed for ownership: new acquisitions and recoveries answer 503
`redis_unavailable` having created no row and taken no lease. Replay of
`COMPLETED`, `FAILED`-terminal answers, and fingerprint conflicts never
touch Redis and keep working. The gateway stays up; `/health` is
unaffected. Release failures are safe by TTL. (Tested: R7, CASE 5,
replay-with-Redis-down.)

## 9. What happens when PostgreSQL fails

503 `storage_unavailable` before any lease side-effect (checkout precedes
coordination, so a refused request never takes a lease). Mid-transaction
backend death aborts the transaction — nothing partial commits — and the
epoch is untouched, so a fresh connection recovers normally. The
`LeaseReleaser` releases any held lease on every exit path. (Tested:
dead-pool service, backend-kill mid-transaction.)

## 10. What happens when a process crashes

- Before PG persist (lease held, no row): no orphan exists; the lease
  expires; the next attempt proceeds cleanly (CASE 1, tested).
- After epoch persist, before execution: `PROCESSING` at the new epoch,
  recoverable again — generations keep advancing, stale claims rejected
  (CASE 2, tested).
- Mid-execution: orphaned `PROCESSING`; next same-fingerprint duplicate
  recovers (epoch + 1) and completes; the crashed generation, if it was
  merely stalled, is fenced on wake (CASE 3/4, T1–T7 race tested).
- Checked-out pool connections die with the process; PostgreSQL aborts
  their transactions. No half-written terminal state can exist
  (single-statement atomicity).

## 11. What this phase guarantees (precise)

**Durable idempotency with lease-based ownership and fencing:** for one
idempotency key, at most one durable record exists; each ownership
generation holds a unique Redis token and a unique durable epoch; only
the generation presenting the current epoch can commit a terminal
transition; stale generations affect zero rows and their results are
discarded; Redis outage fails closed without corrupting or orphaning
durable state; committed results replay without Redis. That is the whole
claim. It does not cover cross-node exactly-once side effects outside the
fenced record, unbounded wait-free progress under continuous Redis outage,
or self-healing of orphaned rows without a new request arriving.

## 12. What this phase deliberately does NOT guarantee

- No waiter multiplexing: duplicates of an *active* owner get 202, not a
  shared wait (next phase).
- No cross-request waiting at all: recovery executes inline on the
  recovering request's worker.
- No renewal: an owner that overruns its TTL keeps its epoch authority
  unless superseded — by design, not by accident.
- No Redlock, no quorum, no multi-instance Redis: single `redis:7` dev
  instance; calling it Redlock would be false.
- No orphan reaping without traffic: an orphaned row waits for the next
  same-key request (or operator action), it is not background-healed.

## 13. Why this is not Redlock

Redlock is quorum acquisition across N independent masters with clock-
drift compensation and a validity-time computation. This phase does
`SET NX PX` on ONE instance for liveness only, while correctness rests on
PostgreSQL predicates. Different algorithm, different guarantees, no
quorum math anywhere. The single-instance dev Redis is stated plainly in
`docker-compose.yml` and `docs/architecture.md`.
