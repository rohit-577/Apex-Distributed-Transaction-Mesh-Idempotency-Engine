#include "persistence/IdempotencyRepository.hpp"

#include <stdexcept>

#include "persistence/PgConnection.hpp"

namespace apex::persistence {

namespace {

// Column order shared by every SELECT in this file.
constexpr const char* kSelectByKey =
    "SELECT idempotency_key, fingerprint, status, http_status, response_body,"
    " response_content_type, error_code, error_message, fencing_epoch"
    " FROM idempotency_records WHERE idempotency_key = $1";

IdempotencyRecord row_to_record(const PgResult& result, int row) {
  IdempotencyRecord record;
  record.key = result.value(row, 0);
  record.fingerprint = result.value(row, 1);
  record.status = status_from_string(result.value(row, 2));
  if (!result.value_is_null(row, 3)) {
    record.http_status = std::stoi(result.value(row, 3));
  }
  if (!result.value_is_null(row, 4)) {
    record.response_body = result.value(row, 4);
  }
  if (!result.value_is_null(row, 5)) {
    record.response_content_type = result.value(row, 5);
  }
  if (!result.value_is_null(row, 6)) {
    record.error_code = result.value(row, 6);
  }
  if (!result.value_is_null(row, 7)) {
    record.error_message = result.value(row, 7);
  }
  // fencing_epoch is NOT NULL with a CHECK (>= 1); a NULL or malformed
  // value here means schema/code drift — std::stoll throws loudly rather
  // than this function silently coercing to a wrong epoch.
  record.fencing_epoch = std::stoll(result.value(row, 8));
  return record;
}

}  // namespace

const char* to_string(RecordStatus status) {
  switch (status) {
    case RecordStatus::Processing:
      return "PROCESSING";
    case RecordStatus::Completed:
      return "COMPLETED";
    case RecordStatus::Failed:
      return "FAILED";
  }
  return "UNKNOWN";
}

RecordStatus status_from_string(const std::string& status) {
  if (status == "PROCESSING") {
    return RecordStatus::Processing;
  }
  if (status == "COMPLETED") {
    return RecordStatus::Completed;
  }
  if (status == "FAILED") {
    return RecordStatus::Failed;
  }
  // The CHECK constraint guarantees only these three values exist; anything
  // else means the schema and the code disagree — fail loudly, never coerce.
  throw PgError("Unknown idempotency record status in database: '" + status + "'");
}

AcquireResult IdempotencyRepository::try_acquire(PgConnection& db, const std::string& key,
                                                 const std::string& fingerprint) {
  // Transaction 1 (create): the INSERT either establishes our PROCESSING row
  // or — on conflict — establishes nothing. Under concurrency the database
  // decides the winner via the primary key; the loser blocks inside this
  // INSERT until the winner commits or rolls back, so what it reads next is
  // always a settled outcome. No application-level check-then-insert exists.
  // (void): BEGIN/COMMIT/ROLLBACK return command-ok results by contract.
  (void)db.exec("BEGIN");
  bool committed = false;
  try {
    PgResult inserted = db.exec_params(
        "INSERT INTO idempotency_records (idempotency_key, fingerprint, status, fencing_epoch)"
        " VALUES ($1, $2, 'PROCESSING', 1)"
        " ON CONFLICT (idempotency_key) DO NOTHING"
        " RETURNING idempotency_key, fingerprint, status, fencing_epoch",
        {key, fingerprint});
    if (inserted.rows() == 1) {
      (void)db.exec("COMMIT");
      committed = true;
      AcquireResult result;
      result.outcome = AcquireOutcome::Created;
      result.record.key = key;
      result.record.fingerprint = fingerprint;
      result.record.status = RecordStatus::Processing;
      result.record.fencing_epoch = 1;  // First ownership generation (F1).
      return result;
    }
    (void)db.exec("ROLLBACK");
    committed = true;  // Rolled back cleanly: nothing of ours to commit.
  } catch (...) {
    if (!committed) {
      try {
        (void)db.exec("ROLLBACK");
      } catch (...) {
        // The connection is likely broken; the original error is the one
        // that matters. Swallowing the rollback failure is deliberate and
        // documented: the pool destroys dead connections on release.
      }
    }
    throw;
  }

  // Transaction 2 (read, autocommit): the winner's row is committed by now
  // (we waited inside the INSERT), so a plain SELECT sees it. Rows are never
  // deleted in Phase 1 — absence here is an invariant violation, not a
  // "not found" to be tolerated.
  std::optional<IdempotencyRecord> existing = find_by_key(db, key);
  if (!existing) {
    throw PgError("Idempotency acquire lost a race that cannot be lost: key '" + key +
                  "' conflicted on INSERT but has no row.");
  }
  AcquireResult result;
  result.outcome = AcquireOutcome::Found;
  result.record = *std::move(existing);
  return result;
}

std::optional<IdempotencyRecord> IdempotencyRepository::find_by_key(PgConnection& db,
                                                                    const std::string& key) {
  PgResult result = db.exec_params(kSelectByKey, {key});
  if (result.rows() == 0) {
    return std::nullopt;
  }
  return row_to_record(result, 0);
}

bool IdempotencyRepository::complete(PgConnection& db, const std::string& key,
                                     const std::string& fingerprint, std::int64_t epoch,
                                     int http_status, const std::string& body,
                                     const std::string& content_type) {
  // FENCING INVARIANT, enforced HERE in the predicate: the write counts only
  // when this caller presents the CURRENT epoch. A superseded generation
  // (lease expired, epoch advanced by recovery) affects zero rows — the
  // stale result is discarded and the current owner's result stands.
  PgResult result = db.exec_params(
      "UPDATE idempotency_records"
      " SET status = 'COMPLETED', http_status = $2, response_body = $3,"
      " response_content_type = $4, updated_at = now(), completed_at = now()"
      " WHERE idempotency_key = $1 AND status = 'PROCESSING' AND fingerprint = $5"
      " AND fencing_epoch = $6",
      {key, std::to_string(http_status), body, content_type, fingerprint,
       std::to_string(epoch)});
  return result.command_tuples() == "1";
}

bool IdempotencyRepository::fail(PgConnection& db, const std::string& key,
                                 const std::string& fingerprint, std::int64_t epoch,
                                 const std::string& error_code, const std::string& error_message) {
  PgResult result = db.exec_params(
      "UPDATE idempotency_records"
      " SET status = 'FAILED', error_code = $2, error_message = $3,"
      " updated_at = now(), completed_at = now()"
      " WHERE idempotency_key = $1 AND status = 'PROCESSING' AND fingerprint = $4"
      " AND fencing_epoch = $5",
      {key, error_code, error_message, fingerprint, std::to_string(epoch)});
  return result.command_tuples() == "1";
}

std::optional<std::int64_t> IdempotencyRepository::try_recover(PgConnection& db,
                                                                const std::string& key,
                                                                const std::string& fingerprint,
                                                                std::int64_t expected_epoch) {
  // One transaction, row lock held throughout:
  //   1. SELECT ... FOR UPDATE serializes concurrent recoverers on the row.
  //   2. Eligibility (PROCESSING + same fingerprint + epoch UNMOVED since
  //      the caller observed it) is decided on the locked row.
  //   3. The increment re-asserts the exact epoch in its own predicate, so
  //      the database — not the caller — awards the new generation.
  // Exactly one recoverer per observed epoch can win (INV-19); everyone else
  // sees nullopt and must re-read (202 / replay / conflict, never assume).
  (void)db.exec("BEGIN");
  bool settled = false;
  try {
    PgResult locked = db.exec_params(
        "SELECT fencing_epoch, status, fingerprint FROM idempotency_records"
        " WHERE idempotency_key = $1 FOR UPDATE",
        {key});
    if (locked.rows() != 1) {
      (void)db.exec("ROLLBACK");
      settled = true;
      return std::nullopt;  // CASE A won the race elsewhere, or key unknown.
    }
    const std::int64_t current = std::stoll(locked.value(0, 0));
    const bool processing = locked.value(0, 1) == "PROCESSING";
    const bool same_fp = locked.value(0, 2) == fingerprint;
    if (!processing || !same_fp || current != expected_epoch) {
      (void)db.exec("ROLLBACK");
      settled = true;
      // Terminal row, foreign fingerprint, or a generation newer than the one
      // we observed: ownership is NOT ours. The caller re-reads and answers
      // from the durable state it finds.
      return std::nullopt;
    }
    PgResult bumped = db.exec_params(
        "UPDATE idempotency_records"
        " SET fencing_epoch = fencing_epoch + 1, updated_at = now()"
        " WHERE idempotency_key = $1 AND status = 'PROCESSING' AND fingerprint = $2"
        " AND fencing_epoch = $3"
        " RETURNING fencing_epoch",
        {key, fingerprint, std::to_string(expected_epoch)});
    if (bumped.rows() != 1) {
      (void)db.exec("ROLLBACK");
      settled = true;
      return std::nullopt;  // Defensive: unreachable under the row lock.
    }
    const std::int64_t next = std::stoll(bumped.value(0, 0));
    (void)db.exec("COMMIT");
    settled = true;
    return next;  // previous + 1, durably (INV-18).
  } catch (...) {
    if (!settled) {
      try {
        (void)db.exec("ROLLBACK");
      } catch (...) {
      }
    }
    throw;
  }
}

}  // namespace apex::persistence
