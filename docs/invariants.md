# Apex Concurrency & Correctness Invariants

Every invariant is numbered so tests and code can reference it (e.g.
`// Enforces INV-03`). Status: **Enforced** (code + test exist),
**Declared** (agreed, implementation is Phase 1+), **Not validated**.

## Phase 0 — Enforced

- **INV-01 — No blocking work on I/O threads.** Handlers either compute
  inline (Router: pure function) or suspend asynchronously
  (DependencyChecker: resolve/connect/timer only). *Enforced by design;
  validated by the 32-client concurrency test completing without stalls.*
- **INV-02 — Bounded per-connection memory.** Request bodies are capped at
  1 MiB (`Session::kMaxBodyBytes`); oversize yields `413`, never unbounded
  growth. *Enforced; validated by code review (a dedicated >1 MiB wire test
  is Phase 1 work).*
- **INV-03 — Controlled errors, no exceptions across I/O.** Every failure
  maps to a JSON status or a clean close; `main()` is the only
  `try/catch`, at the outermost layer. *Enforced; validated by the
  malformed-bytes test.*
- **INV-04 — Graceful shutdown drains.** `stop()` closes the acceptor only;
  sessions finish their current exchange; threads join before destruction.
  *Enforced; validated by fixture teardown on every integration test.*
- **INV-05 — Configuration is validated before serving.** Invalid config →
  exit code 2, no socket opened. *Enforced; validated by config tests.*

## Declared for Phase 1+ (not yet enforced — the engine does not exist)

- **INV-06 — Single execution per idempotency key.** Concurrent duplicates
  attach as waiters to one in-flight execution; the business operation runs
  at most once per key per fencing epoch.
- **INV-07 — Waiters never trust notification alone.** A wake-up (Redis
  pub/sub or otherwise) is a hint; the waiter re-reads the durable
  PostgreSQL record and only returns after the record reaches a terminal
  state. Missed notifications therefore delay but never corrupt.
- **INV-08 — PostgreSQL is the sole source of truth.** Redis content is
  always re-derivable; loss of Redis degrades performance, never correctness.
- **INV-09 — Stale owners cannot overwrite.** Leases expire while owners may
  still run; every state transition carries a fencing token (monotonic per
  key, issued with the lease) and the durable compare-and-swap rejects
  writes from superseded epochs.
- **INV-10 — Exactly-once is scoped, never absolute.** The guarantee is
  stated as "the recorded *effect* applies once within the idempotency
  boundary (key + fencing epoch + terminal record)"; retries outside that
  boundary are the client's responsibility. No broader claim is permitted in
  docs, logs, or code comments.
- **INV-11 — Every invariant above gets a test.** Unit for pure logic,
  concurrency for races (run under repetition/stress), failure for crash and
  partition behavior.

## Non-invariants (explicitly NOT promised)

- Request ordering across different keys; any latency bound on Phase 0
  `/ready`; durability of anything while PostgreSQL is unreachable
  (`/ready` reports `503` instead).
