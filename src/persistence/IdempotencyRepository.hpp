#pragma once

// Idempotency-record repository: the ONLY code that speaks SQL about
// idempotency state (AGENTS.md §2). Blocking — call on database worker
// threads, never on Asio I/O threads.
//
// Atomicity strategy (INV-16): acquisition is a single
// `INSERT ... ON CONFLICT (idempotency_key) DO NOTHING` inside an explicit
// transaction — never SELECT-then-INSERT. When two transactions race for
// the same key, PostgreSQL serializes them on the unique index: the loser
// waits, then inserts nothing, then reads the winner's committed row. The
// outcome is deterministic and decided entirely by the database.
//
// Terminal-write strategy (INV-15): COMPLETED/FAILED writes carry
// `WHERE status = 'PROCESSING' (+ fingerprint)`. The affected-row count is
// the verdict: 1 means this call transitioned the record, 0 means the
// record was already terminal (or vanished, which cannot happen — rows are
// never deleted in Phase 1) and the write was safely ignored.

#include <optional>
#include <string>

namespace apex::persistence {

class PgConnection;

enum class RecordStatus {
  Processing,
  Completed,
  Failed,
};

[[nodiscard]] const char* to_string(RecordStatus status);
[[nodiscard]] RecordStatus status_from_string(const std::string& status);

struct IdempotencyRecord {
  std::string key;
  std::string fingerprint;
  RecordStatus status{RecordStatus::Processing};
  // Populated for COMPLETED records (INV-13: enough to replay the response).
  std::optional<int> http_status;
  std::string response_body;
  std::string response_content_type;
  // Populated for FAILED records.
  std::string error_code;
  std::string error_message;
};

enum class AcquireOutcome {
  Created,  // This call inserted the PROCESSING row.
  Found,    // A row already existed; `record` holds its current state.
};

struct AcquireResult {
  AcquireOutcome outcome{AcquireOutcome::Found};
  IdempotencyRecord record;
};

class IdempotencyRepository {
 public:
  // Atomically establishes the PROCESSING row for (key, fingerprint), or
  // returns the conflicting/committed row that won the race. Throws PgError
  // on database failure (callers map to 503).
  [[nodiscard]] AcquireResult try_acquire(PgConnection& db, const std::string& key,
                                          const std::string& fingerprint);

  [[nodiscard]] std::optional<IdempotencyRecord> find_by_key(PgConnection& db,
                                                             const std::string& key);

  // PROCESSING -> COMPLETED, storing the reproducible response. Returns true
  // iff this call performed the transition; false leaves a terminal record
  // untouched (TEST F / INV-15).
  [[nodiscard]] bool complete(PgConnection& db, const std::string& key,
                              const std::string& fingerprint, int http_status,
                              const std::string& body, const std::string& content_type);

  // PROCESSING -> FAILED, storing the failure marker. Same zero-row verdict.
  [[nodiscard]] bool fail(PgConnection& db, const std::string& key,
                          const std::string& fingerprint, const std::string& error_code,
                          const std::string& error_message);
};

}  // namespace apex::persistence
