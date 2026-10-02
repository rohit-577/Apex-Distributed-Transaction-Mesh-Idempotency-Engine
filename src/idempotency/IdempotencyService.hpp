#pragma once

// Idempotency orchestration: HTTP-neutral business logic between the gateway
// and the durable + coordination layers
// (HTTP -> service -> repository/leases -> PostgreSQL + Redis).
//
// The service owns NO threads, NO sockets, and NO SQL. It is a blocking
// call (repository + lease + simulated operation) that must run on a worker
// thread — Session posts it to the DB pool and suspends (INV-01). One
// service instance is shared by all sessions; everything it touches (pool,
// leases, executor, registry, redis, logger) is thread-safe and it keeps no
// request state, so concurrent handle() calls are independent. Authority
// stays in PostgreSQL either way (INV-17); Redis only ever decides liveness.
//
// Ownership model (Phase 2, preserved):
//   CASE A (no record):            win the Redis lease, then INSERT epoch 1.
//                                  Redis down or lease held => NO row is
//                                  created (fail closed, 503/202).
//   CASE B (PROCESSING, owner      lease key present => the owner is (or may
//          active):                still be) active => WAIT (Phase 3: the
//                                  waiter suspends; multiplexing arrives here).
//   CASE C (PROCESSING, lease      win the lease, then CAS the fencing epoch
//          expired):               previous+1. Exactly one recoverer wins
//                                  per observed epoch (INV-19).
//   CASE D/E/F (terminal or        answered from PostgreSQL WITHOUT touching
//          fingerprint conflict):  Redis — replay survives Redis outage.
// A terminal write presents the owner's epoch IN the SQL predicate; a
// superseded generation affects zero rows (FENCING INVARIANT) and its
// result is discarded, never merged.
//
// Multiplexing model (Phase 3): handle() never blocks waiting. When the
// verdict is "another generation owns this", it returns Wait immediately;
// the caller (Session) suspends asynchronously and re-invokes handle() on
// wake/timeout/fallback — the SAME decision engine, so waiters that observe
// a dead owner transparently become recoverers. Owner completion
// broadcasts (publish + local notify) AFTER the durable commit; waiters
// always re-read PostgreSQL before answering (INV-MUX-03/04).
//
// Outcome contract (HTTP mapping lives in Session, kinds here):
// - Executed / Recovered: this call executed (epoch 1 / epoch > 1).
// - Wait: another generation owns the operation; caller should suspend and
//   re-invoke (NOT an HTTP status).
// - Replayed / InProgress / FingerprintConflict / FailedTerminal: Phase 2
//   meanings. InProgress now only covers "lease held but NO durable row"
//   (crash window A) — observed PROCESSING always waits instead.
// - StaleEpoch / RedisUnavailable / StorageUnavailable: unchanged.

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
class RedisClient;
}  // namespace apex::coordination

namespace apex::idempotency {

class OperationExecutor;
class WaiterRegistry;

struct OperationRequest {
  std::string key;
  std::string fingerprint;
  std::string canonical_body;
};

struct OperationOutcome {
  enum class Kind {
    Executed,
    Recovered,
    Wait,
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

struct ServiceDependencies {
  std::shared_ptr<persistence::ConnectionPool> pool;
  // Nullable: any path needing a lease decision answers RedisUnavailable.
  std::shared_ptr<coordination::LeaseManager> leases;
  std::shared_ptr<OperationExecutor> executor;
  std::shared_ptr<WaiterRegistry> registry;
  // Publish path for completion broadcast (shared with LeaseManager's
  // client in production; BEST-EFFORT — publish failure never fails the
  // already-committed operation).
  std::shared_ptr<coordination::RedisClient> redis;
};

class IdempotencyService {
 public:
  IdempotencyService(ServiceDependencies deps, observability::Logger& logger);

  // BLOCKING: performs lease + repository transactions and the operation
  // on the caller's thread. Never throws: every failure maps to an outcome
  // (worker threads must never see an exception). Never WAITS: an owned
  // operation elsewhere yields Wait for the caller to suspend on.
  [[nodiscard]] OperationOutcome handle(const OperationRequest& request);

  // Shared waiter registry (Session drives async waiting through it).
  [[nodiscard]] std::shared_ptr<WaiterRegistry> registry() const { return deps_.registry; }

 private:
  // CASE A: no durable row. Wins the Redis lease, then creates epoch 1 —
  // or fails closed (503) / defers (202) without creating anything.
  OperationOutcome handle_first_request(persistence::PgConnection& db,
                                        persistence::IdempotencyRepository& repo,
                                        const OperationRequest& request,
                                        const std::string& logged_key,
                                        const std::function<long long()>& elapsed_ms,
                                        OperationOutcome& done);

  // CASE B/C: PROCESSING row with our fingerprint. Lease present => Wait;
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

  // Runs the operation and commits with `epoch` in the SQL predicate, then
  // broadcasts completion (publish + local notify, best-effort, AFTER the
  // commit). `executed_kind` is Executed (epoch 1) or Recovered (epoch > 1).
  OperationOutcome execute_owned(persistence::PgConnection& db,
                                 persistence::IdempotencyRepository& repo,
                                 const OperationRequest& request, std::int64_t epoch,
                                 const std::string& logged_key,
                                 const std::function<long long()>& elapsed_ms,
                                 OperationOutcome& done, OperationOutcome::Kind executed_kind);

  OperationOutcome redis_unavailable(OperationOutcome& done, const std::string& logged_key,
                                     const std::string& what = "coordination unreachable");

  // Post-commit broadcast: best-effort Redis publish + local registry
  // notify. Never throws; never affects the committed result.
  void broadcast_completion(const std::string& key, const std::string& fingerprint,
                            const std::string& logged_key);

  ServiceDependencies deps_;
  observability::Logger& logger_;
};

}  // namespace apex::idempotency
