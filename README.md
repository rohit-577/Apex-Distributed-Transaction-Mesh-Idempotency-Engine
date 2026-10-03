# Apex — Distributed Transaction Mesh & Idempotency Engine

<p align="center">
  <strong>Distributed request coordination, durable idempotency, fencing, recovery, and in-flight request multiplexing — built in C++20.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" />
  <img src="https://img.shields.io/badge/Boost.Asio-Async-005571?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Boost.Beast-HTTP-005571?style=for-the-badge" />
  <img src="https://img.shields.io/badge/PostgreSQL-16-336791?style=for-the-badge&logo=postgresql&logoColor=white" />
  <img src="https://img.shields.io/badge/Redis-7-DC382D?style=for-the-badge&logo=redis&logoColor=white" />
  <img src="https://img.shields.io/badge/GoogleTest-Tests-4285F4?style=for-the-badge&logo=google&logoColor=white" />
  <img src="https://img.shields.io/badge/CMake-Build-064F8C?style=for-the-badge&logo=cmake&logoColor=white" />
  <img src="https://img.shields.io/badge/Docker-Development-2496ED?style=for-the-badge&logo=docker&logoColor=white" />
</p>

---

## Overview

**Apex** is a distributed transaction coordination and idempotency engine designed to make repeated requests safe under concurrency, retries, process crashes, lease expiration, network failures, and multi-node execution.

The core problem is deceptively simple:

> What happens when multiple clients submit the same logical operation at the same time — and the process executing it crashes halfway through?

Apex addresses this with a layered protocol combining:

- **PostgreSQL** for durable transaction state
- **Redis** for short-lived distributed ownership leases
- **Fencing epochs** for stale-owner protection
- **Local waiter multiplexing** for efficient in-flight duplicate handling
- **Redis Pub/Sub** for cross-node wake-up notifications
- **Background recovery** for abandoned processing records
- **Asynchronous C++ networking** using Boost.Asio and Boost.Beast
- **Structured observability** through logs and Prometheus-style metrics

The central design principle is:

> **Redis coordinates. PostgreSQL decides. Fencing prevents stale ownership from committing.**

Apex deliberately does **not** claim global exactly-once execution. Its correctness guarantees are scoped to the durable idempotency protocol and its simulated operation boundary. External side effects require their own idempotency or fencing mechanism.

---

## Why Apex?

A basic idempotency implementation often looks like:

```text
request
   ↓
check key
   ↓
execute
   ↓
save result
