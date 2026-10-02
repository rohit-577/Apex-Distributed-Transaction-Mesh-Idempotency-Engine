#pragma once

// Idempotency orchestration: HTTP-neutral business logic between the gateway
// and the durable + coordination layers
// (HTTP -> service -> repository/leases -> PostgreSQL + Redis).
//
// The service owns NO threads, NO sockets, and NO SQL. It is a blocking
// call (repository + lease + simulated operation) that must run on a worker
// thread — Session posts it to the DB pool and suspends (INV-01). One
// service instance is shared by all sessions; everything it touches (pool,
// leases, logger) is thread-safe and it keeps no request state, so
// concurrent handle() calls are independent. Authority stays in PostgreSQL
// either way (INV-17); Redis only ever decides liveness.
//
// Ownership model (Phase 2):
//   CASE A (no record):            win the Redis lease, then INSERT epoch 1.
//                                  Redis down or lease held => NO row is
//                                  created (fail closed, 503/202).
//   CASE B (PROCESSING, owner      lease key present => active owner => 202,
//          active):                non-waiting (multiplexing is later).
//   CASE C (PROCESSING, lease      win the lease, then CAS the fencing epoch
//          expired):               previous+1. Exactly one recoverer wins
//                                  per observed epoch (INV-19).
//   CASE D/E/F (terminal or        answered from PostgreSQL WITHOUT touching
//          fingerprint conflict):  Redis — replay survives Redis outage.
// A terminal write presents the owner's epoch IN the SQL predicate; a
// superseded generation affects zero rows (FENCING INVARIANT) and its
// result is discarded, never merged.
//
// Outcome contract (HTTP mapping lives in Session, kinds here):
// - Executed: first execution finished now (HTTP 200 or 500 from the op).
// - Recovered: executed as a recovery generation (epoch > 1). Same HTTP
//   shape as Executed; distinguished for logs/tests.
// - Replayed / InProgress / FingerprintConflict / FailedTerminal: Phase 1
//   meanings, unchanged.
// - StaleEpoch: our generation was superseded before our terminal write
//   landed (HTTP 409 stale_ownership_epoch). Our result is discarded; the
//   current generation's result stands.
// - RedisUnavailable: coordination unreachable; fail closed (HTTP 503
//   redis_unavailable). No row created, no write attempted.
// - StorageUnavailable: PostgreSQL unreachable (HTTP 503
//   storage_unavailable).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace apex::observability {
class Logger;
}

namespace apex::persistence {
class ConnectionPool;
class PgConnection;
class IdempotencyRepository;
struct IdempotencyRecord;
}  // namespace persistence

namespace apex::coordination {
class LeaseManager;
}

namespace apex::idempotency {

struct OperationRequest {
  std::string key;
  std::string fingerprint;
  std::string canonical_body;
};

struct OperationOutcome {
  enum class Kind {
    Executed,
    Recovered,
    Replayed,
    InProgress,
    FingerprintConflict,
    FailedTerminal,
    StaleEpoch,
    RedisUnavailable,
    StorageUnavailable,
  };

  Kind kind{Kind::StorageUnavailable};
  int http_status{503};
  std::string body;
  std::string content_type{"application/json"};
};

class IdempotencyService {
 public:
  // `leases` may be null (storage-only wiring): any path needing a lease
  // decision then answers RedisUnavailable instead of proceeding.
  IdempotencyService(std::shared_ptr<persistence::ConnectionPool> pool,
                     observability::Logger& logger,
                     std::shared_ptr<coordination::LeaseManager> leases = nullptr);

  // BLOCKING: performs lease + repository transactions and the simulated
  // operation on the caller's thread. Never throws: every failure maps to
  // an outcome (worker threads must never see an exception).
  [[nodiscard]] OperationOutcome handle(const OperationRequest& request);

 private:
  // CASE A: no durable row. Wins the Redis lease, then creates epoch 1 —
  // or fails closed (503) / defers (202) without creating anything.
  OperationOutcome handle_first_request(persistence::PgConnection& db,
                                        persistence::IdempotencyRepository& repo,
                                        const OperationRequest& request,
                                        const std::string& logged_key,
                                        const std::function<long long()>& elapsed_ms,
                                        OperationOutcome& done);

  // CASE B/C: PROCESSING row with our fingerprint. Lease present => 202;
  // lease absent => attempt recovery with the observed epoch.
  OperationOutcome handle_processing_seen(persistence::PgConnection& db,
                                          persistence::IdempotencyRepository& repo,
                                          const OperationRequest& request,
                                          const persistence::IdempotencyRecord& observed,
                                          const std::string& logged_key,
                                          const std::function<long long()>& elapsed_ms,
                                          OperationOutcome& done);

  // Holds `observed`'s lease: CAS fencing_epoch previous+1, then execute as
  // the new generation. Loses deterministically when the epoch moved.
  OperationOutcome recover_with_lease(persistence::PgConnection& db,
                                      persistence::IdempotencyRepository& repo,
                                      const OperationRequest& request,
                                      const persistence::IdempotencyRecord& observed,
                                      const std::string& logged_key,
                                      const std::function<long long()>& elapsed_ms,
                                      OperationOutcome& done);

  // Runs the operation and commits with `epoch` in the SQL predicate.
  // `executed_kind` is Executed (epoch 1) or Recovered (epoch > 1).
  OperationOutcome execute_owned(persistence::PgConnection& db,
                                 persistence::IdempotencyRepository& repo,
                                 const OperationRequest& request, std::int64_t epoch,
                                 const std::string& logged_key,
                                 const std::function<long long()>& elapsed_ms,
                                 OperationOutcome& done, OperationOutcome::Kind executed_kind);

  OperationOutcome redis_unavailable(OperationOutcome& done, const std::string& logged_key,
                                     const std::string& what = "coordination unreachable");

 private:
  std::shared_ptr<persistence::ConnectionPool> pool_;
  observability::Logger& logger_;
  std::shared_ptr<coordination::LeaseManager> leases_;
};

}  // namespace apex::idempotency
