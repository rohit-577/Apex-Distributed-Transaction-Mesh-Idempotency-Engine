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
// never deleted) and the write was safely ignored.
//
// Fencing strategy (FENCING INVARIANT, Phase 2): every terminal write ALSO
// carries `AND fencing_epoch = $N`. The epoch counts ownership generations
// (1 = creating generation; each recovery assigns previous + 1 atomically).
// A stale owner presenting a superseded epoch affects zero rows — enforced
// by the database in the predicate, never merely checked in C++ first.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  // Ownership generation that created or last recovered this record.
  // 1 = creating generation. Only the holder of the CURRENT epoch may
  // transition the record to terminal (FENCING INVARIANT).
  std::int64_t fencing_epoch{1};
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
  // returns the conflicting/committed row that won the race. The canonical
  // body is stored for background re-execution (V003); it is never logged.
  // Throws PgError on database failure (callers map to 503).
  [[nodiscard]] AcquireResult try_acquire(PgConnection& db, const std::string& key,
                                          const std::string& fingerprint,
                                          const std::string& canonical_body);

  [[nodiscard]] std::optional<IdempotencyRecord> find_by_key(PgConnection& db,
                                                             const std::string& key);

  // PROCESSING -> COMPLETED, storing the reproducible response. Returns true
  // iff this call performed the transition AS THE CURRENT EPOCH; false
  // leaves the record untouched — already terminal, or a stale owner whose
  // generation was superseded (TEST F / INV-15 / FENCING INVARIANT).
  [[nodiscard]] bool complete(PgConnection& db, const std::string& key,
                              const std::string& fingerprint, std::int64_t epoch,
                              int http_status, const std::string& body,
                              const std::string& content_type);

  // PROCESSING -> FAILED, storing the failure marker. Same epoch-guarded
  // zero-row verdict.
  [[nodiscard]] bool fail(PgConnection& db, const std::string& key,
                          const std::string& fingerprint, std::int64_t epoch,
                          const std::string& error_code, const std::string& error_message);

  // Recovery ownership: advances an orphaned PROCESSING row from
  // `expected_epoch` to `expected_epoch + 1` and returns the new epoch, or
  // nullopt when the row is missing, terminal, fingerprint-mismatched, or
  // already advanced past `expected_epoch` (a competing recoverer won).
  //
  // Atomicity: SELECT ... FOR UPDATE holds the row lock for the whole
  // transaction, so concurrent recoverers serialize; the exact-epoch
  // predicate in the UPDATE makes all but the first fail deterministically
  // (INV-19). Never SELECT-then-UPDATE without the lock.
  [[nodiscard]] std::optional<std::int64_t> try_recover(PgConnection& db,
                                                        const std::string& key,
                                                        const std::string& fingerprint,
                                                        std::int64_t expected_epoch);

  // Orphan candidates for background recovery: PROCESSING rows with a stored
  // body idle longer than `idle_longer_than`, oldest first, at most `limit`.
  // Served by the partial index (no full scan). Eligibility here is only a
  // hint — the caller still acquires the lease and CASes the epoch per row,
  // so concurrent traffic/reapers/instances resolve to exactly one owner.
  struct OrphanCandidate {
    std::string key;
    std::string fingerprint;
    std::string canonical_body;
  };

  [[nodiscard]] std::vector<OrphanCandidate> find_orphans(PgConnection& db, int limit,
                                                          long long idle_longer_than_ms);
};

}  // namespace apex::persistence
