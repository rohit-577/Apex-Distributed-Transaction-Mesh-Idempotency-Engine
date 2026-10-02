-- V002 — Durable fencing epoch (Phase 2).
--
-- Adds the ownership-generation counter that makes stale-owner rejection
-- possible. Applied after V001 by persistence::Schema::ensure (ordered file
-- list); the statement is IF NOT EXISTS-safe, so re-applying is a no-op.
--
-- Design notes:
-- * fencing_epoch counts OWNERSHIP GENERATIONS of one idempotency key, not
--   wall-clock time and not Redis TTLs. 1 = the generation that created the
--   row. Each recovery (orphan adopted after lease expiry) durably assigns
--   previous + 1 inside a transaction (INV-18, INV-19).
-- * Existing Phase 1 rows backfill to 1 via the DEFAULT: they were all
--   created by a single implicit generation, which is exactly what epoch 1
--   means. No data migration beyond the default is required.
-- * The epoch is enforced in SQL, never just in C++: every terminal write
--   carries AND fencing_epoch = $N (FENCING INVARIANT). A stale owner
--   presenting a superseded epoch affects zero rows.
-- * No Redis-side epoch exists. Redis holds liveness (the lease); PostgreSQL
--   holds authority (the epoch). Confusing the two is the central error this
--   schema prevents.

ALTER TABLE idempotency_records
  ADD COLUMN IF NOT EXISTS fencing_epoch BIGINT NOT NULL DEFAULT 1;

COMMENT ON COLUMN idempotency_records.fencing_epoch IS
  'Ownership generation of this key. 1 = creating generation; recovery assigns previous + 1 atomically. Terminal writes must present the current epoch (FENCING INVARIANT).';

-- Guards the epoch domain at the data level: epochs start at 1 and only
-- move forward (recovery increments; nothing decrements or resets).
DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'chk_fencing_epoch_positive'
  ) THEN
    ALTER TABLE idempotency_records
      ADD CONSTRAINT chk_fencing_epoch_positive CHECK (fencing_epoch >= 1);
  END IF;
END
$$;
