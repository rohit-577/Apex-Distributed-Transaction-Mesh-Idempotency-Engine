# Apex — Distributed Transaction Mesh & Idempotency Engine

<p align="center">
  <strong>A production-oriented distributed idempotency and transaction-coordination engine built in C++20.</strong>
</p>

<p align="center">
  <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++20"></a>
  <a href="https://www.boost.org/"><img src="https://img.shields.io/badge/Boost.Asio-Async-005571?style=for-the-badge" alt="Boost.Asio"></a>
  <a href="https://www.boost.org/doc/libs/release/libs/beast/"><img src="https://img.shields.io/badge/Boost.Beast-HTTP-005571?style=for-the-badge" alt="Boost.Beast"></a>
  <a href="https://www.postgresql.org/"><img src="https://img.shields.io/badge/PostgreSQL-16-336791?style=for-the-badge&logo=postgresql&logoColor=white" alt="PostgreSQL"></a>
  <a href="https://redis.io/"><img src="https://img.shields.io/badge/Redis-7-DC382D?style=for-the-badge&logo=redis&logoColor=white" alt="Redis"></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/CMake-Build-064F8C?style=for-the-badge&logo=cmake&logoColor=white" alt="CMake"></a>
  <a href="https://github.com/google/googletest"><img src="https://img.shields.io/badge/GoogleTest-Test-4285F4?style=for-the-badge&logo=google&logoColor=white" alt="GoogleTest"></a>
  <a href="https://www.docker.com/"><img src="https://img.shields.io/badge/Docker-Development-2496ED?style=for-the-badge&logo=docker&logoColor=white" alt="Docker"></a>
</p>

<p align="center">
  <strong>Durable state.</strong> &nbsp;·&nbsp;
  <strong>Distributed ownership.</strong> &nbsp;·&nbsp;
  <strong>Fencing.</strong> &nbsp;·&nbsp;
  <strong>Crash recovery.</strong> &nbsp;·&nbsp;
  <strong>Request multiplexing.</strong>
</p>

---

## What is Apex?

Apex is a distributed transaction-coordination and idempotency engine designed around a difficult systems problem:

> **How do you safely handle the same logical request arriving concurrently, across multiple workers or nodes, while also surviving crashes, retries, lease expiration, dependency failures, and stale owners?**

A naive implementation might do this:

```text
request
   ↓
check key
   ↓
execute
   ↓
save result
```

That model breaks as soon as multiple workers race.

For example:

```text
                         ┌── Worker A ──┐
                         │              │
Client 1 ──┐             │              ▼
           ├── same key ─┼──────────> EXECUTE
Client 2 ──┤             │
Client 3 ──┘             └── Worker B ──> EXECUTE AGAIN
```

Now add a crash:

```text
Worker A
   │
   ├── acquires ownership
   ├── starts operation
   │
   X── process crashes
   │
   │ lease expires
   ▼
Worker B
   │
   ├── recovers abandoned work
   ├── obtains newer fencing epoch
   ├── executes
   └── commits
```

The difficult part is not simply detecting duplicates.

The difficult part is ensuring that an **old worker cannot successfully commit after a newer worker has taken over**.

Apex addresses that with a layered protocol:

```text
                     ┌─────────────────────────┐
                     │       HTTP Clients       │
                     └────────────┬────────────┘
                                  │
                                  ▼
                     ┌─────────────────────────┐
                     │ Boost.Asio / Beast      │
                     │ Async HTTP Gateway      │
                     └────────────┬────────────┘
                                  │
                                  ▼
                     ┌─────────────────────────┐
                     │   Idempotency Service   │
                     │                         │
                     │ Ownership               │
                     │ Fingerprinting          │
                     │ Recovery                │
                     │ Fencing                 │
                     │ Multiplexing            │
                     └───────┬─────────┬───────┘
                             │         │
                durable     │         │ coordination
                             │         │
                             ▼         ▼
                  ┌──────────────┐  ┌──────────────┐
                  │ PostgreSQL   │  │    Redis     │
                  │              │  │              │
                  │ State        │  │ Leases       │
                  │ Fingerprint  │  │ Pub/Sub      │
                  │ Response     │  │ Wake-ups     │
                  │ Fencing      │  │              │
                  └──────────────┘  └──────────────┘
                             ▲
                             │
                  ┌──────────┴──────────┐
                  │                     │
           Waiter Registry        Recovery Reaper
```

---

# Core Design Principle

Apex intentionally separates **durable correctness** from **ephemeral coordination**.

> **Redis coordinates. PostgreSQL decides. Fencing prevents stale ownership from committing.**

Redis is used for:

- short-lived ownership leases
- distributed coordination
- cross-node wake-up notifications

PostgreSQL is responsible for:

- durable idempotency state
- request fingerprints
- stored responses
- transaction state
- fencing epochs
- authoritative commit decisions

This distinction is one of the most important architectural decisions in the project.

---

# The Problem Apex Solves

Suppose a client sends:

```http
POST /v1/operations
Idempotency-Key: payment-123
Content-Type: application/json

{
  "amount": 500,
  "currency": "INR"
}
```

The client may retry because of:

- a network timeout
- a lost response
- a client crash
- a proxy retry
- a server restart
- temporary infrastructure failure

The same request may therefore reach Apex multiple times.

Apex needs to distinguish:

### Same key + same request

```text
payment-123
     +
same fingerprint
     ↓
duplicate
```

from:

### Same key + different request

```text
payment-123
     +
different fingerprint
     ↓
CONFLICT
```

The second request must never silently become a different operation under the same idempotency key.

---

# Request Lifecycle

A typical request follows this path:

```text
                    HTTP request
                         │
                         ▼
                Validate request
                         │
                         ▼
                Compute fingerprint
                         │
                         ▼
             Read durable PostgreSQL state
                         │
          ┌──────────────┼──────────────┐
          │              │              │
          ▼              ▼              ▼
       COMPLETED      PROCESSING      no row
          │              │              │
          ▼              ▼              ▼
       replay       wait/recover     acquire
                                      lease
                                        │
                                        ▼
                                  create owner
                                        │
                                        ▼
                                  execute once
                                        │
                                        ▼
                              fenced PostgreSQL
                                   commit
                                        │
                                        ▼
                                  notify waiters
                                        │
                                        ▼
                                  return result
```

---

# Durable State Machine

Every idempotency key has durable state in PostgreSQL.

```text
                    ┌──────────────┐
                    │    absent    │
                    └──────┬───────┘
                           │
                           │ first owner
                           ▼
                    ┌──────────────┐
                    │  PROCESSING  │
                    └──────┬───────┘
                           │
                 ┌─────────┴─────────┐
                 │                   │
                 ▼                   ▼
          ┌─────────────┐     ┌─────────────┐
          │  COMPLETED  │     │    FAILED   │
          └─────────────┘     └─────────────┘
```

Terminal states are immutable.

A completed result can therefore be replayed without contacting Redis.

A failed terminal result can likewise be returned without executing the operation again.

---

# Request Fingerprinting

Apex computes a deterministic fingerprint from the request payload.

This creates an important invariant:

```text
same idempotency key
        +
same request
        =
same fingerprint
```

while:

```text
same idempotency key
        +
different request
        =
conflict
```

This prevents accidental reuse of an idempotency key for a different logical operation.

---

# Distributed Ownership

When a new request needs to execute, Apex attempts to acquire a short-lived Redis lease.

The ownership primitive follows the atomic Redis pattern:

```text
SET apex:lease:<key> <unique-token> NX PX <ttl>
```

Each attempt receives a unique random ownership token.

The token is also used during release so that one owner cannot accidentally release another owner's lease.

Conceptually:

```text
Worker A
   │
   ├── SET lease NX PX
   │
   ├── SUCCESS
   │
   ▼
 owns key
```

while:

```text
Worker B
   │
   ├── SET lease NX PX
   │
   └── FAIL
       │
       ▼
    does not own key
```

The Redis lease is a **liveness mechanism**, not the final correctness authority.

---

# Why Fencing Exists

A lease can expire while the original worker is still executing.

Consider:

```text
Time ───────────────────────────────────────────────────>

Worker A:
          acquire
            │
            ├──────────── execute ────────────────┐
            │                                      │
            │ lease expires                        │
            ▼                                      │
Worker B:                                           │
          acquire                                   │
            │                                       │
            ├── epoch = 2                           │
            ├── execute                             │
            └── commit                              │
                                                    │
Worker A:                                           │
            └──────────── stale commit ─────────────┘
```

Without fencing, Worker A could potentially commit after Worker B.

Apex solves this using a durable PostgreSQL fencing epoch.

```text
Worker A
epoch = 1

       ↓ ownership becomes stale

Worker B
epoch = 2

       ↓

Worker A attempts commit
       ↓
PostgreSQL checks epoch = 1
       ↓
0 rows affected
       ↓
STALE OWNER
```

Worker B's epoch 2 commit succeeds.

---

# Fencing Is Enforced by PostgreSQL

The fencing condition is part of the SQL write itself.

Conceptually:

```sql
UPDATE idempotency_records
SET
    status = 'COMPLETED',
    response_body = $response
WHERE
    key = $key
    AND status = 'PROCESSING'
    AND fingerprint = $fingerprint
    AND fencing_epoch = $epoch;
```

The application does not merely check the epoch and then assume it is still valid.

Instead:

```text
SQL affected rows = 1
        │
        └── owner is still authoritative

SQL affected rows = 0
        │
        └── owner is stale or state changed
```

This makes PostgreSQL's transactional state boundary the final decision point.

---

# Recovery

Apex supports recovery of abandoned `PROCESSING` records.

Recovery can be triggered by:

### Incoming traffic

```text
request
   ↓
find PROCESSING record
   ↓
determine recovery eligibility
   ↓
acquire lease
   ↓
advance fencing epoch
   ↓
execute
   ↓
fenced commit
```

### Background reaper

```text
background reaper
       │
       ▼
bounded scan
       │
       ▼
eligible PROCESSING records
       │
       ▼
same recovery path
       │
       ▼
lease + fencing CAS
       │
       ▼
execute
       │
       ▼
fenced commit
```

The reaper does not introduce a separate ownership protocol.

Recovery goes through the same correctness path used by normal requests.

---

# In-Flight Request Multiplexing

Apex does not require every duplicate request to independently wait on the database.

Within a process, duplicate requests can be multiplexed through a local waiter registry.

```text
                 same idempotency key
                         │
          ┌──────────────┼──────────────┐
          │              │              │
          ▼              ▼              ▼
       Request A      Request B      Request C
          │              │              │
          ▼              ▼              ▼
        OWNER          WAITER          WAITER
          │              │              │
          └──────────────┼──────────────┘
                         │
                         ▼
                  one operation
                         │
                         ▼
                   one result
                         │
              ┌──────────┼──────────┐
              ▼          ▼          ▼
           Request A  Request B  Request C
```

Only the owner executes the operation.

Waiters suspend asynchronously and re-check durable state when completion becomes observable.

---

# Cross-Node Waiting

Local waiters are not enough when duplicate requests land on different Apex processes.

Apex therefore uses Redis Pub/Sub as a **wake-up optimization**.

```text
Node A
  │
  ├── owner executes
  │
  ├── PostgreSQL COMMIT
  │
  └── publish completion notification
                │
                ▼
             Redis
                │
        ┌───────┴───────┐
        ▼               ▼
     Node B           Node C
     waiter           waiter
        │               │
        └───────┬───────┘
                ▼
         re-check PostgreSQL
```

The ordering is important:

```text
1. durable PostgreSQL commit
2. notification
```

Never:

```text
1. notification
2. durable commit
```

because a notification must never be interpreted as proof that durable state exists.

---

# Lost Notifications Are Safe

Redis Pub/Sub provides wake-up semantics, not durable message delivery.

A notification can be lost.

Apex therefore treats notifications as an optimization.

If a notification is missed:

```text
missed notification
       ↓
timer/reconnect/recheck
       ↓
PostgreSQL
       ↓
observe durable state
       ↓
return result
```

Correctness does not depend on Pub/Sub delivery.

---

# Failure Model

Apex explicitly handles failure at every important boundary.

| Failure | Expected behavior |
|---|---|
| Redis unavailable | New ownership fails closed |
| PostgreSQL unavailable | Request fails without committing state |
| Owner crashes | Processing record can be recovered |
| Lease expires | Recovery can establish newer ownership |
| Stale owner commits | PostgreSQL fencing rejects it |
| Pub/Sub message lost | Waiters fall back to durable-state rechecks |
| Redis reconnects | Subscriber/waiter lifecycle recovers |
| PostgreSQL restarts | Durable state remains authoritative |
| Waiter disconnects | Waiter slot is released |
| Waiter times out | Durable state remains untouched |
| Process restarts | Completed results remain replayable |
| Reaper races with another recovery | PostgreSQL CAS/fencing decides |
| Graceful shutdown | Accepting stops before worker infrastructure is destroyed |

---

# Correctness Guarantees

Apex is designed around explicit invariants rather than informal assumptions.

### Durable idempotency

For a given idempotency key:

```text
one durable record
```

### Fingerprint consistency

A key cannot silently represent two different requests.

### Terminal immutability

Once a request reaches a terminal state:

```text
COMPLETED
```

or:

```text
FAILED
```

that state is not overwritten by another execution.

### Fencing

A stale owner cannot successfully perform the fenced terminal commit.

### Recovery serialization

Concurrent recovery attempts compete through the same durable coordination mechanism.

### Waiter isolation

A waiter does not become an owner merely because it waited.

### Notification independence

Correctness does not depend on receiving a Redis Pub/Sub notification.

### Bounded resources

Waiters, request bodies, and other externally influenced resources are bounded.

---

# Important Scope Boundary

Apex does **not** claim that arbitrary external side effects become globally exactly-once.

For example:

```text
             Apex
               │
               ▼
       External Payment API
               │
               ▼
             Bank
```

Suppose:

```text
1. Apex starts payment
2. Payment succeeds externally
3. Apex crashes
4. Apex never records the response
```

No local PostgreSQL fencing mechanism can magically undo the external payment.

External side effects therefore need their own idempotency/fencing protocol.

Apex's exactly-once-style guarantee is intentionally scoped to its **durable idempotency protocol and protected operation boundary**.

That limitation is a correctness requirement, not a missing feature.

---

# HTTP API

## `POST /v1/operations`

The primary idempotent operation endpoint.

Example:

```http
POST /v1/operations
Idempotency-Key: order-12345
Content-Type: application/json
```

```json
{
  "operation": "create_order",
  "amount": 500,
  "currency": "INR"
}
```

The server computes a request fingerprint and coordinates ownership using the idempotency key.

---

## Duplicate request

If the original request is still executing, the duplicate can wait for the same durable result through the waiter/multiplexing path.

If the result has already been stored:

```text
duplicate
   ↓
PostgreSQL
   ↓
stored result
   ↓
replay
```

No new execution is required.

---

## Conflicting request

If the same idempotency key is reused with a different fingerprint:

```text
same key
   +
different fingerprint
   ↓
conflict
```

The conflicting request is rejected rather than silently executing a different operation.

---

# Operational Endpoints

## Health

```http
GET /health
```

Process liveness endpoint.

---

## Readiness

```http
GET /ready
```

Checks the dependencies required by Apex's fail-closed serving policy.

---

## Metrics

```http
GET /metrics
```

Prometheus-compatible text output.

Example:

```text
# HELP apex_requests_total Total POST /v1/operations requests handled.
# TYPE apex_requests_total counter
apex_requests_total 0
```

---

# Observability

Apex provides structured operational visibility without exposing request contents or secrets.

Representative log fields include:

```text
timestamp
level
node
correlation_id
idempotency_key
fingerprint
fencing_epoch
outcome
latency_ms
```

Sensitive data is intentionally excluded.

Apex does not log:

- request bodies
- passwords
- complete lease tokens
- database credentials
- other secret material

---

# Correlation IDs

Requests can carry a correlation identifier through the request lifecycle.

The correlation ID is:

- validated
- generated when required
- echoed appropriately
- included in structured logs
- independent from the request fingerprint

This keeps operational tracing separate from correctness identity.

---

# Metrics

Apex exposes fixed-cardinality metrics covering areas such as:

- request counts
- validation failures
- fingerprint conflicts
- completed replays
- ownership attempts
- ownership failures
- lease failures
- recovery
- fencing outcomes
- waiter starts
- waiter completions
- waiter timeouts
- waiter cancellations
- active waiters
- reaper activity
- subscriber activity
- Redis failures
- PostgreSQL failures

User-controlled idempotency keys are not used as metric labels.

This prevents unbounded metric-cardinality growth.

---

# Architecture Principles

## 1. PostgreSQL is the durable authority

Redis can restart.

Redis can lose notifications.

Redis can become temporarily unavailable.

The durable idempotency state remains in PostgreSQL.

---

## 2. Leases provide liveness

The Redis lease answers:

```text
"Who currently appears to own this work?"
```

It does not alone answer:

```text
"Who is allowed to commit?"
```

That decision is protected by PostgreSQL fencing.

---

## 3. Fencing makes stale work harmless

A previous owner may continue executing after losing ownership.

Apex assumes this can happen.

The important property is:

```text
stale execution
      ↓
attempted durable commit
      ↓
fencing predicate
      ↓
rejected
```

---

## 4. Notifications are optimizations

Pub/Sub improves convergence latency.

It is not a durable source of truth.

---

## 5. Recovery uses the same correctness path

Traffic-driven recovery and background recovery share the same ownership and fencing logic.

This avoids having two subtly different recovery implementations.

---

## 6. Correctness comes before throughput

A fast distributed system that violates its invariants is not considered successful.

Benchmarks therefore verify correctness properties such as execution count and durable state while measuring performance.

---

# Technology Stack

| Layer | Technology |
|---|---|
| Language | C++20 |
| Networking | Boost.Asio |
| HTTP | Boost.Beast |
| Database | PostgreSQL 16 |
| Coordination | Redis 7 |
| Redis Client | redis-plus-plus / hiredis |
| Serialization | nlohmann/json |
| Cryptography / random tokens | OpenSSL |
| Testing | GoogleTest |
| Build | CMake |
| Dependency Management | vcpkg |
| Containers | Docker Compose |
| Metrics | Prometheus-style exposition |

---

# Project Structure

```text
Apex Distributed Transaction Mesh & Idempotency Engine/
│
├── src/
│   ├── api/
│   ├── coordination/
│   ├── core/
│   ├── db/
│   ├── http/
│   ├── observability/
│   ├── recovery/
│   ├── redis/
│   └── ...
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── concurrency/
│   ├── recovery/
│   └── ...
│
├── scripts/
│   └── bench.ps1
│
├── docs/
│   ├── architecture.md
│   ├── invariants.md
│   ├── operations.md
│   ├── benchmarking.md
│   └── FINAL_ENGINEERING_AUDIT.md
│
├── migrations/
│   ├── V001__idempotency_records.sql
│   ├── V002__fencing_epoch.sql
│   └── V003__recovery_support.sql
│
├── CMakeLists.txt
├── docker-compose.yml
├── vcpkg.json
├── AGENTS.md
├── .env.example
└── README.md
```

---

# Running Apex

## Requirements

Development requires:

- C++20-capable compiler
- CMake
- vcpkg
- Docker Desktop
- Git

The project was developed and validated using MSVC on Windows with PostgreSQL and Redis running through Docker.

---

## 1. Clone

```powershell
git clone https://github.com/rohit-577/Apex-Distributed-Transaction-Mesh-Idempotency-Engine.git

cd "Apex-Distributed-Transaction-Mesh-Idempotency-Engine"
```

---

## 2. Start PostgreSQL and Redis

```powershell
docker compose up -d
```

Verify the containers:

```powershell
docker compose ps
```

---

## 3. Configure environment

Copy the example environment file:

```powershell
Copy-Item .env.example .env
```

Review the development configuration before running the service.

---

## 4. Build

For a Release build:

```powershell
cmake --build build --config Release
```

---

## 5. Start Apex

```powershell
.\build\Release\apex.exe
```

The default development server listens on:

```text
http://127.0.0.1:8080
```

---

## 6. Check health

```powershell
curl.exe http://127.0.0.1:8080/health
```

---

## 7. Check readiness

```powershell
curl.exe http://127.0.0.1:8080/ready
```

---

## 8. Check metrics

```powershell
curl.exe http://127.0.0.1:8080/metrics
```

---

# Testing

Testing is a major part of the project rather than an afterthought.

The test matrix covers:

```text
HTTP behavior
configuration
PostgreSQL state transitions
idempotency contracts
fingerprint conflicts
concurrent ownership
Redis leases
fencing epochs
stale-owner races
cross-system races
request multiplexing
waiter lifecycle
timeouts
cancellation
client disconnects
Redis reconnects
PostgreSQL restart
process restart
background recovery
reaper races
graceful shutdown
observability
migrations
benchmark correctness
failure injection
stress / soak behavior
```

Run the Release test suite:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Infrastructure-dependent tests require PostgreSQL and Redis to be available.

---

# Concurrency Testing

Apex explicitly tests concurrent duplicate requests rather than relying on sequential unit tests.

Representative correctness scenarios include:

```text
2-way fan-in
50-way fan-in
100-way fan-in
200-way benchmark fan-in
```

The important invariant is:

```text
N duplicate requests
        ↓
1 operation execution
        ↓
N converged responses
```

The project verifies this through execution counters and durable state rather than merely inspecting logs.

---

# Failure-Injection Testing

The project also exercises failure scenarios that are difficult to reproduce through normal functional tests.

Examples include:

```text
owner process termination
lease expiration
stale-owner races
Redis restart
PostgreSQL restart
subscriber reconnect
waiter disconnect
waiter timeout
reaper/recovery races
shutdown during activity
reconnect storms
```

A previously discovered waiter-timer lifecycle failure during reconnect stress was fixed structurally using:

- timer generation guards
- explicit waiter re-registration
- timer-paced transient retries
- exception containment in background components
- no-throw waiter release paths

The restart-overlap scenario was then repeatedly re-executed successfully.

---

# Benchmarking

Apex includes a dedicated benchmark executable:

```text
apex_bench
```

and a benchmark wrapper:

```text
scripts/bench.ps1
```

The benchmark suite covers:

```text
A — single-flight
B — replay
C — duplicate fan-in
D — multi-key concurrency
E — recovery
F — cross-node behavior
G — saturation
H — failure/restart overlap
```

Every benchmark scenario is correctness-gated.

A result is not considered successful merely because it is fast.

The harness verifies properties including:

```text
execution count
durable row state
response convergence
response bytes
fencing behavior
```

---

# Measured Development Results

The final benchmark environment was:

```text
CPU:        AMD Ryzen 7 5700U
CPU:        8 cores / 16 threads
Memory:     ~13.8 GiB
OS:         Windows 11
Compiler:   MSVC 19.51
CMake:      4.4.3
Boost:      1.92
PostgreSQL: 16.15
Redis:      7.4.11
I/O threads: 8
PG pool:    16
Redis pool: 32
Lease TTL:  10 seconds
```

Representative measured results:

| Scenario | Result |
|---|---:|
| Single-flight | ~17 ms mean |
| Completed replay | ~3.5 ms p50 |
| 200-way fan-in | 1 execution |
| 8 concurrent keys | 8 executions |
| Cross-node convergence | ~51 ms mean |
| Recovery | ~20 ms |
| 60-second soak | 0 errors |
| 50 waiters / 50 disconnects | 1 execution, no leaked slots |

These measurements are from the development environment above.

They are **not presented as universal production capacity guarantees**.

---

# Stress / Soak Testing

The final engineering cycle included repeated soak testing.

Representative result:

```text
Duration:       60 seconds
Rounds:         2753 / 2705
Total requests: 5500+
Errors:         0
Memory:         stable after warm-up
```

The workload was also repeated rather than relying on a single successful run.

The project intentionally avoids presenting one benchmark number as proof of production scalability.

---

# Security & Defensive Boundaries

Apex applies validation and resource limits around externally influenced inputs.

### Idempotency keys

- bounded length
- restricted character set
- never silently truncated

### Request bodies

- bounded size
- not written to logs

### Redis keys

- fixed namespace
- validated input
- bounded construction

### Redis ownership tokens

- cryptographically random
- unique per ownership attempt
- never logged in full

### SQL

All database operations use parameterized queries.

### Metrics

User-controlled values are not emitted as metric labels.

### Secrets

Passwords and credentials are not included in source-controlled configuration or operational output.

---

# Resource & Lifecycle Safety

Apex explicitly handles:

- bounded waiter registration
- waiter cancellation
- waiter timeout
- client disconnect
- process shutdown
- background reaper shutdown
- Redis subscriber shutdown
- database pool lifecycle
- Redis pool lifecycle

Shutdown follows an explicit ordering rather than relying on destructors to accidentally establish the correct dependency order.

Conceptually:

```text
stop accepting
      ↓
stop recovery activity
      ↓
release waiter infrastructure
      ↓
stop Redis subscribers
      ↓
stop I/O
      ↓
destroy dependency pools
```

This is particularly important for asynchronous systems where callbacks may otherwise outlive the resources they reference.

---

# What Apex Does NOT Use

The project deliberately avoids several mechanisms that are unnecessary for its current correctness model.

```text
Redis Cluster
Redis Sentinel
Redlock
Redis Streams
Kubernetes
Service Mesh
TLS infrastructure
Authentication platform
Distributed tracing platform
Downstream exactly-once guarantees
```

These are scope decisions rather than accidental omissions.

The core objective is to build and verify the idempotency/fencing/recovery protocol correctly before introducing additional distributed infrastructure.

---

# Known Limitations

Apex is a serious distributed-systems implementation, but it is not presented as a complete cloud platform.

Current limitations include:

- single Redis development instance
- no Redis Cluster/Sentinel
- no Redlock
- no TLS/authentication layer
- orphan recovery has a bounded detection interval
- waiter timeout behavior is recheck-granular
- benchmark numbers are development-machine measurements
- the protected operation is simulated
- readiness checks dependency reachability rather than full semantic health
- external side effects require their own idempotency/fencing
- no downstream exactly-once guarantee

These boundaries are documented explicitly because distributed-systems correctness depends as much on stating what a system does **not** guarantee as what it does.

---

# Engineering Documentation

The repository contains deeper engineering documentation:

| Document | Purpose |
|---|---|
| `docs/architecture.md` | Architecture and component boundaries |
| `docs/invariants.md` | Correctness invariants and enforcement |
| `docs/operations.md` | Runtime and operational behavior |
| `docs/benchmarking.md` | Benchmark methodology and results |
| `docs/FINAL_ENGINEERING_AUDIT.md` | Final engineering audit |

---

# Development History

Apex was developed incrementally so that each major distributed-systems mechanism could be validated before the next one was introduced.

```text
Phase 0
Gateway Foundation
        │
        ▼
Phase 1
Durable PostgreSQL Idempotency
        │
        ▼
Phase 2
Redis Leases + Fencing
        │
        ▼
Phase 3
In-Flight Request Multiplexing
        │
        ▼
Phase 4
Recovery + Resilience Hardening
        │
        ▼
Phase 5
Observability + Operations
        │
        ▼
Phase 6
Benchmarking + Stress + Final Audit
```

This sequencing was intentional.

The project did not begin with Redis Pub/Sub, benchmarking, or distributed recovery.

It first established a durable correctness boundary and then progressively added coordination, recovery, multiplexing, and operational capabilities around it.

---

# Final Engineering Status

**Apex — Phases 0 through 6 complete.**

The final engineering cycle verified:

- durable idempotency
- request fingerprinting
- terminal-state immutability
- distributed ownership
- lease handling
- fencing epochs
- stale-owner rejection
- concurrent recovery
- in-flight request multiplexing
- waiter lifecycle
- cross-node wake-up
- missed notification recovery
- Redis failure handling
- PostgreSQL failure handling
- process restart behavior
- background reaping
- graceful shutdown
- structured logging
- correlation IDs
- Prometheus-style metrics
- correctness-gated benchmarks
- stress/soak behavior
- source-level audits
- repository hygiene

The final implementation intentionally stops at this boundary rather than adding speculative infrastructure.

---

# Engineering Takeaways

Apex is fundamentally an exploration of one idea:

> **Distributed correctness should be enforced where durable state is committed, not inferred from ephemeral coordination.**

The system therefore treats:

```text
Redis
    → coordination

Pub/Sub
    → notification

Waiter Registry
    → request optimization

Reaper
    → recovery trigger

PostgreSQL
    → durable authority

Fencing Epoch
    → stale-owner protection
```

That separation is what allows the system to reason about:

```text
concurrency
    +
crashes
    +
retries
    +
lease expiration
    +
stale workers
    +
reconnects
    +
recovery
```

without making correctness depend on every distributed component behaving perfectly.

---

<p align="center">
  <strong>APEX</strong>
  <br>
  Distributed transaction coordination with durable correctness at the state boundary.
</p>

<p align="center">
  <sub>C++20 · Boost.Asio · Boost.Beast · PostgreSQL · Redis · Fencing · Recovery · Multiplexing</sub>
</p>
