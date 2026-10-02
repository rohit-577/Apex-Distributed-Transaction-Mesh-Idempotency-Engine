# Apex Benchmarking

Status: **Methodology Declared; harness Planned; results: NONE.**
Per project rules, no performance number may be reported until a real
benchmark executes. This document defines how numbers will be earned so that
future claims are checkable.

## What will be measured (Phase 1+, when there is engine behavior to measure)

1. **Baseline gateway** — `/health` throughput and p50/p99 latency, N
   concurrent keep-alive connections, Debug vs Release.
2. **Idempotent write path** — first-execution vs replayed-key latency;
   waiter fan-in scaling (1 owner + K waiters, time-to-last-response).
3. **Degraded modes** — Redis down (poll fallback) and cold-path cost of
   fencing checks, reported as deltas against the healthy baseline.

## Method (binding when the harness lands)

- Harness lives in `tests/benchmark/` (separate target, never part of
  `ctest` gate timing).
- Fixed machine description, fixed commit hash, fixed `docker compose`
  images, 3+ runs, warm-up excluded, p50/p99/max + throughput reported —
  never a single mean.
- `GET /health` numbers and engine numbers are reported separately; no
  mixing gateway overhead into engine claims.
- Any claim in README/docs must link the run log (command, config, results).

## Current numbers

None. Anything resembling a benchmark result dated before the harness
exists is fabrication and must be removed on sight.
