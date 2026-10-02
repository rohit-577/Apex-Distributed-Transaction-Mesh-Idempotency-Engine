-- V001 — Durable idempotency records (Phase 1).
--
-- Single source of truth for the Phase 1 schema. Applied by:
--   1. the server at startup (best-effort; see persistence::Schema), and
--   2. the test fixtures before every PostgreSQL-gated test run.
-- Every statement is IF NOT EXISTS-safe, so applying twice is a no-op.
--
-- Design notes (each column exists for a documented reason):
-- * The idempotency key IS the primary key. INV-12 (at most one record per
--   key) is enforced by the database itself, not by application logic, and
--   concurrent INSERTs serialize on this constraint (INV-16). No surrogate
--   id: there is no second access path that would justify one, and every
--   lookup in Phase 1 is by key.
-- * No secondary indexes: the only access pattern is point lookup by key.
-- * Terminal-state protection is a data rule, not just code: the CHECK
--   constraints require terminal rows to carry their result payload, and all
--   terminal writes use `WHERE status = 'PROCESSING'` so a late or repeated
--   write affects zero rows instead of overwriting (INV-15 territory).
-- * Fencing/epoch columns are deliberately ABSENT. Ownership fencing belongs
--   to the later coordination phase and will arrive as V002. This table
--   records durable state only.

CREATE TABLE IF NOT EXISTS idempotency_records (
  -- Client-supplied key, stored exactly as received: never truncated, never
  -- normalized (normalization could merge distinct client keys). Validated in
  -- C++ (<= 255 chars, restricted charset); TEXT here imposes no extra limit.
  idempotency_key TEXT PRIMARY KEY,

  -- Hex-encoded SHA-256 over method + normalized route + canonical body.
  -- Same key + same fingerprint = same logical operation (replay safe).
  -- Same key + different fingerprint = client error (409, never executed).
  fingerprint TEXT NOT NULL,

  -- PROCESSING -> COMPLETED | PROCESSING -> FAILED. Terminal states are
  -- final in Phase 1; nothing transitions out of them.
  status TEXT NOT NULL
    CHECK (status IN ('PROCESSING', 'COMPLETED', 'FAILED')),

  -- Stored HTTP response for COMPLETED records: enough to reproduce the
  -- original response byte-for-byte without re-executing anything.
  http_status INTEGER NULL,
  response_body TEXT NULL,
  response_content_type TEXT NULL,

  -- Machine-readable failure marker for FAILED records (e.g.
  -- 'simulated_failure'). The 409 returned for a FAILED duplicate carries
  -- this so the client can distinguish "failed before" from "in use".
  error_code TEXT NULL,
  error_message TEXT NULL,

  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  -- Set exactly once, on the PROCESSING -> terminal transition. NULL means
  -- "not terminal yet" and is load-bearing for operators inspecting stuck
  -- PROCESSING rows (whose recovery belongs to the later lease phase).
  completed_at TIMESTAMPTZ NULL,

  -- A PROCESSING row must not carry a result yet: it represents work that
  -- has produced no durable outcome.
  CONSTRAINT chk_processing_has_no_result CHECK (
    status <> 'PROCESSING'
    OR (http_status IS NULL AND completed_at IS NULL AND error_code IS NULL)
  ),
  -- A COMPLETED row must carry the full reproducible response.
  CONSTRAINT chk_completed_has_result CHECK (
    status <> 'COMPLETED'
    OR (http_status IS NOT NULL AND response_body IS NOT NULL AND completed_at IS NOT NULL)
  ),
  -- A FAILED row must say why, and when it became terminal.
  CONSTRAINT chk_failed_has_error CHECK (
    status <> 'FAILED'
    OR (error_code IS NOT NULL AND completed_at IS NOT NULL)
  )
);

COMMENT ON TABLE idempotency_records IS
  'Durable idempotency state. Sole source of truth for request deduplication (INV-08).';
COMMENT ON COLUMN idempotency_records.idempotency_key IS
  'Client key, exact match. PRIMARY KEY enforces one record per key (INV-12, INV-16).';
COMMENT ON COLUMN idempotency_records.fingerprint IS
  'SHA-256 hex of the canonical request; distinguishes replay from conflict.';
COMMENT ON COLUMN idempotency_records.status IS
  'PROCESSING, COMPLETED, or FAILED. Terminal states are final in Phase 1.';
COMMENT ON COLUMN idempotency_records.completed_at IS
  'NULL until terminal. Operators use it to spot stuck PROCESSING rows.';
