-- V003 — Background-recovery support (Phase 4).
--
-- Two additions, both justified by the orphan reaper's single access
-- pattern (see src/recovery/):
--
-- 1. request_body: the canonical request body of the creating request.
--    Traffic-driven recovery never needs it (the HTTP request carries the
--    body), but BACKGROUND recovery re-executes an operation with no HTTP
--    request present — the body must come from durable state. NULL for
--    pre-V003 rows (they recover traffic-driven only, as before); every
--    new INSERT stores it. It is the CANONICAL body (already normalized),
--    never raw bytes.
--
-- 2. Partial index on (updated_at) WHERE status = 'PROCESSING': serves ONLY
--    the reaper's eligibility scan
--      (status + body present + idle longer than X, oldest first, bounded).
--    Every other access path is by primary key and uses no index. A full
--    table scan per pass would not scale with completed-row history, hence
--    this single purpose-built partial index — the only secondary index in
--    the schema, documented here rather than added speculatively.

ALTER TABLE idempotency_records
  ADD COLUMN IF NOT EXISTS request_body TEXT NULL;

COMMENT ON COLUMN idempotency_records.request_body IS
  'Canonical request body for background re-execution. NULL on pre-V003 rows (traffic recovery only). Never logged.';

CREATE INDEX IF NOT EXISTS idx_processing_updated_at
  ON idempotency_records (updated_at)
  WHERE status = 'PROCESSING';

COMMENT ON INDEX idx_processing_updated_at IS
  'Serves the orphan-reaper eligibility scan only. All other access is by primary key.';
