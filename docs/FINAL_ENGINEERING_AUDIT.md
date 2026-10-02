# APEX Final Engineering Audit

Final record for the complete project (Phases 0–6). Every claim below is
either a measured fact (with command/log), an implemented mechanism (with
file + test), or an explicitly labeled limitation. Status of this document:
accurate as of the final commit; the verification table in §17 was produced
by real runs, not projections.

## 1. System overview

Apex is a C++20 async HTTP gateway (`POST /v1/operations` + `Idempotency-Key`)
demonstrating correct idempotency under concurrency and failure: durable
PostgreSQL state, Redis lease ownership with fencing epochs, single-execution
fan-in with async waiting, background orphan recovery, correlation IDs,
Prometheus metrics, and honestly measured benchmarks.

## 2. Architecture

```text
Client
  |
  v
HTTP / Beast (Asio I/O pool; never blocks — INV-01)
  |
  v
Session (async state machine: validate → pool → Wait→suspend → respond)
  |
  v                        +-----------------------+
IdempotencyService -------> | PostgreSQL 18 (libpq) | durable state,
  | read-first             |  key PK, fingerprint, | fingerprint,
  | lease-first            |  response, fencing    | response,
  | CAS recovery           |  epoch, request_body  | fencing epoch
  +--> LeaseManager -----> | Redis 7 (redis++)     | Lease: SET NX PX,
  |                        |  apex:lease:<key>     | Lua compare-del
  +--> publish/notify ----> | Redis pub/sub         | wake-only
  |                        |  apex:w:<hash>        |
  +--> WaiterRegistry (process-local slots, sharded, bounded)
  +--> OrphanReaper (background adoption, same service path)
```

## 3. Request lifecycle

1. Validate key (`400` codes) → 2. fingerprint body (`400` if not JSON) →
3. `POST` to DB pool (never I/O thread) → 4. `service.handle`:
   no row → lease → INSERT epoch 1 → execute → fenced commit → publish +
   notify + release → 200/500; PROCESSING + lease held → `Wait` verdict →
   Session suspends (registry + one timer, zero threads) → wake/timer →
   re-`handle()` → terminal ? respond : re-arm / 202 on deadline;
   PROCESSING + lease free → CAS recover → execute as N+1; terminal →
   replay immediately (no Redis); conflict → 409; stale write → 0 rows →
   409; Redis down on ownership paths → 503 (nothing created); PG down →
   503 (nothing committed).

## 4. Ownership protocol

`SET apex:lease:<key> <128-bit token> NX PX <APEX_LEASE_TTL_MS>` decides who
may try; PostgreSQL decides the row. Fresh token per attempt (OpenSSL
`RAND_bytes`). No renewal: overrunning owners keep epoch authority unless
superseded. Release is Lua compare-delete; always attempted (RAII), safe to
lose (TTL backstop). Redis down ⇒ fail closed before any row/write.

## 5. Fencing protocol

`fencing_epoch` per key (1 at creation; recovery assigns previous + 1 in a
row-locked `SELECT … FOR UPDATE` + CAS transaction). Terminal writes carry
`AND fencing_epoch = $N`; affected-row count is the verdict (1 = won,
0 = stale/terminal). Enforced in SQL, never application-only (FENCING
INVARIANT / INV-20). Stale generations answer `409 stale_ownership_epoch`;
their results are discarded, never merged.

## 6. Waiter multiplexing

`Wait` verdict → Session registers `(channel, weak self, strand wake)` in
the sharded `WaiterRegistry` → mandatory immediate durable re-check (closes
the check-then-register race by protocol, not by memory) → arm one timer
(min(recheck, remaining)) → wake/timer → re-`handle()`. Timeout (default
30 s) answers 202 and mutates nothing. Cap (default 1024/key) answers 202.
Disconnects release only the waiter slot (session destructor). Shutdown
wakes all with 202 while the loop runs.

## 7. Redis notification semantics

`apex:w:<128-bit hash of key+fp>`, empty payload, pattern-subscribed by one
dedicated thread. At-most-once, wake-only: every wake funnels into a durable
re-read. Missed messages converge via fallback timers; reconnects sweep all
waiters (`prod_all`). Reconnects are counted (`subscriber_reconnects`).

## 8. Recovery / reaper

Traffic-driven (duplicate finds orphan → CAS → execute) and background
(`OrphanReaper`: bounded `SELECT … WHERE PROCESSING + body + idle`
via partial index → same `service.handle`). No second ownership mechanism;
epoch CAS admits exactly one winner across traffic/reapers/instances.

## 9. Failure matrix

| Failure | Expected behavior | Verified? |
|---|---|---|
| Redis unavailable (new ownership) | 503 `redis_unavailable`, nothing created | Yes: R7 hermetic, CASE 5, P3-09 |
| Redis unavailable after completion | Replay from PG | Yes: CASE 5, P3-09/10, live |
| Redis lease expires | Recovery possible | Yes: R4, orphan tests, H bench |
| Owner crashes | Recovery (traffic/reaper) | Yes: P3-11, reaper tests, restart smoke |
| Stale owner resumes | Fenced (0 rows) | Yes: T1–T7, F5–F9, P3-11/12 |
| PostgreSQL unavailable | Safe 503, nothing committed | Yes: dead-pool, backend-kill, PG restart test |
| PG transaction abort | No phantom state | Yes: backend-kill mid-transaction |
| Pub/Sub message lost | Durable fallback | Yes: P3-08/15, H scenario |
| Waiter disconnects | Durable op unaffected | Yes: 100/50 cancellation test |
| Waiter timeout | Durable state unchanged | Yes: P3-13 (row/lease/epoch intact) |
| Process restart | Safe recovery + replay | Yes: restart-smoke.ps1 (kill -9) |
| Concurrent duplicates | One logical execution | Yes: 2/50/100 fan-in with counters |
| Fingerprint conflict | 409, zero execution | Yes: P3-06/17 with counts |
| Graceful shutdown | No corruption, clean exit | Yes: P3-18, reaper shutdown, live exits |

## 10. Concurrency guarantees (measured, not claimed)

- 2/50/100-way same-key fan-in → exactly 1 execution, byte-identical
  convergence (execution counters, not logs).
- 8 keys × 8 threads → 8 executions, no cross-key interference.
- Mixed fingerprints → exactly the winner executes; losers 409/202.
- 8 concurrent recoverers, one observed epoch → exactly one winner.
- Cross-node (two instances): 1 execution, pub/sub convergence in ~50 ms.
- 100 waiters / 50 abrupt disconnects → 50 identical results, 1 execution,
  zero leaked slots.

## 11. API semantics

`200` first execution; waiting duplicates converge on the final result;
`202` only for timeout/shutdown/cap/no-row-window (transient, durable
state untouched); terminal replay byte-identical; `409` conflict / failed /
stale-epoch; `400` validation with machine-readable errors; `503`
`storage_unavailable` / `redis_unavailable`; `405` + `Allow`; `404`
unknown. `X-Request-ID` echoed (never fingerprinted). `GET /metrics`
Prometheus text; `GET /health` liveness; `GET /ready` PG+Redis reachability
(matches the fail-closed serving policy — audited, deliberate).

## 12. Configuration

Every `APEX_*` setting: documented name, default, valid range
(`README.md` + `Config.hpp` header), startup validation (exit 2 on
structural errors, fallback + warning otherwise — never silent clamping of
dangerous values... except documented fallbacks for malformed env, which
log warnings), safe failure behavior. Secrets never logged (audited:
passwords appear only in conninfo construction, never in log lines).

## 13. Observability

Logs: one line per event with `key=<safe-prefix>`, `fp=<hash>`,
`cid=<correlation>`, epoch transitions, outcomes, `latency_ms`; waiter
lifecycle events; no bodies/secrets. Metrics: 30+ fixed counters + 1 gauge
(`/metrics`, bounded cardinality — tested: live keys absent). Health/ready
as above. Startup logs node + full effective config (no secrets).

## 14. Migration lifecycle

V001 (records) → V002 (fencing_epoch) → V003 (request_body + partial
index). Ordered, append-only, individually idempotent (`IF NOT EXISTS`);
`Schema::ensure` applies all on every boot and fixture setup. Tested:
fresh DB, repeated application, upgrade from simulated Phase 1 DB
(V001-shape + legacy row → epoch defaults 1, body NULL, data intact,
fully functional afterwards).

## 15. Benchmark methodology

`tests/benchmark/apex_bench` (separate binary, never ctest): in-process
servers, per-thread blocking Beast clients, `steady_clock` latencies,
sorted percentiles; every scenario asserts execution counts/rows/bodies
and stops non-zero on any correctness failure. Two full runs + 60 s soak
+ restart-overlap run, all on the machine in §16.

## 16. Measured benchmark results

Machine: AMD Ryzen 7 5700U (8C/16T laptop, shared load), ~13.8 GiB RAM,
Windows 11 10.0.26200, MSVC 19.51.36246 Release, CMake 4.4.3, Boost 1.92,
libpq 18.4, OpenSSL 3.6.5, redis-plus-plus 1.3.15/hiredis 1.4.1,
PostgreSQL 16.15 + Redis 7.4.11 (docker), 8 I/O threads, PG pool 16, Redis
pool 32, TTL 10 s. Full tables in `docs/benchmarking.md`; headline
measured facts: single-flight ~17 ms mean; replay p50 ~3.5 ms (~280 rps);
200-way fan-in → 1 execution converging in ~120–210 ms wall; cross-node
waiters converge ~51 ms mean; recovery ~20 ms; 50 waiters ride out a real
Redis restart converging on 1 execution; 60 s soak 2753 rounds, 0 errors,
flat ~13 MB.

## 17. Test matrix

162 tests total, hermetic subset skips infra-gated cases with reasons.
Per-phase suites (all passing, Debug + Release, live PG + Redis):

- Phase 0 (gateway/config/deps): config, router, http, dependency-check,
  parallel-health suites.
- Phase 1 (durable contracts): idempotency-contract, db-repository
  (incl. rollback/race/constraints/pool), http-operations (7 semantics).
- Phase 2 (ownership/fencing): lease-contract, lease-redis (R1–R5, R7),
  fencing (F1–F9), cross-system (CASE 1/2/5/6, T1–T7 race).
- Phase 3 (multiplexing): waiter-registry unit, multiplexing (P3-01/02/03,
  P3-16/17, cancellation), waiter-lifecycle (P3-04/05/06/08/09/10/11/13/18),
  cross-node (P3-14/15), subscriber spike, storm isolation.
- Phase 4 (resilience): reaper (adopt/skip/age/race/stop/shutdown),
  resilience-docker (reconnect, PG restart), restart-smoke.ps1,
  migration lifecycle, pool contention.
- Phase 5 (observability): correlation unit + echo + fingerprint
  independence, metrics funnel/endpoint/cardinality, waiter-convergence
  counting, window-A deferral counting, config (node/reaper/waiter vars).
- Phase 6 (benchmarks + audits): A–H + soak with correctness gates.

Skips (hermetic runs only): infra-gated tests skip with explicit
`APEX_TEST_*` reasons; R7 outage and registry/channel unit tests always
run. Zero unexplained skips; zero failures.

## 18. Known limitations (real, remaining)

- Single Redis dev instance (no Cluster/Sentinel/Redlock — by design scope).
- No TLS/auth on any listener (documented; dev loopback only).
- Orphans wait for traffic or the 30 s reaper (no instant adoption).
- Waiter timeout is polling-granular (recheck interval, default 1 s).
- Benchmarks are laptop numbers (shared load, loopback, instant simulated
  op): replay/fan-in shape transfers, absolute RPS does not.
- Simulated operation only: external side effects need their own
  idempotency (documented boundary, INV-10).
- `GET /ready` is TCP-level, not a SQL/RESP session (documented).

## 19. Security considerations

Keys validated (charset/length, never truncated/normalized); SQL fully
parameterized (`$N` only; DDL is static project source); Redis keys fixed
prefix + validated charset; channels are hashes (bounded, opaque);
passwords only in conninfo, never logged; tokens random per attempt,
never logged whole; bodies never logged; `/metrics` contains no request
data (tested); per-key waiter cap + body cap bound resource abuse;
`docker compose` is invoked from tests via fixed absolute compose path
(resilience tests only).

## 20. Exact-once scope / non-goals

Guaranteed: for one idempotency key + matching fingerprint, concurrent
requests coordinate so that one logical owner generation is authoritative
for the durable terminal result, while duplicates wait/replay that same
result; stale generations cannot overwrite; outages fail closed without
phantom state. NOT guaranteed: global exactly-once (external side effects
need their own fencing), cross-key ordering, latency bounds, progress
during total infra loss, or anything involving Redlock/Cluster/Streams/
TLS/auth/Kubernetes (all explicitly out of scope).
