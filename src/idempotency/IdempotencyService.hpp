#pragma once

// Idempotency orchestration: HTTP-neutral business logic between the gateway
// and the repository (HTTP -> service -> repository -> PostgreSQL).
//
// The service owns NO threads, NO sockets, and NO SQL. It is a blocking
// call (repository + simulated operation) that must run on a database
// worker thread — Session posts it to the DB pool and suspends (INV-01).
// One service instance is shared by all sessions; all shared state it
// touches (pool, logger) is thread-safe, and it keeps no request state
// itself, so concurrent handle() calls are independent (INV-17: authority
// stays in PostgreSQL either way).
//
// Outcome contract (HTTP mapping lives in Session, kinds here):
// - Executed: first execution finished now (HTTP 200 or 500 from the op).
// - Replayed: same key + fingerprint hit a COMPLETED row; stored response
//   returned without executing anything (INV-13).
// - InProgress: same key + fingerprint is PROCESSING. Deliberate 202, no
//   waiting — multiplexing arrives in a later phase.
// - FingerprintConflict: same key, different fingerprint. 409, the
//   operation is never executed as the original (INV-03/INV-14).
// - FailedTerminal: same key + fingerprint hit a FAILED row. 409 carrying
//   the original failure; terminal states never restart (INV-15).
// - StorageUnavailable: the database could not be reached. 503.

#include <memory>
#include <string>

namespace apex::observability {
class Logger;
}

namespace apex::persistence {
class ConnectionPool;
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
    Replayed,
    InProgress,
    FingerprintConflict,
    FailedTerminal,
    StorageUnavailable,
  };

  Kind kind{Kind::StorageUnavailable};
  int http_status{503};
  std::string body;
  std::string content_type{"application/json"};
};

class IdempotencyService {
 public:
  IdempotencyService(std::shared_ptr<persistence::ConnectionPool> pool,
                     observability::Logger& logger);

  // BLOCKING: performs repository transactions and the simulated operation
  // on the caller's thread. Never throws: every failure maps to an outcome
  // (the DB pool threads must never see an exception).
  [[nodiscard]] OperationOutcome handle(const OperationRequest& request);

  // Safe key representation for logs: keys are opaque client tokens, so only
  // a prefix is logged, plus the length for disambiguation. Fingerprints
  // (SHA-256 hex) are always safe to log in full. Bodies are never logged.
  [[nodiscard]] static std::string safe_key(const std::string& key);

 private:
  std::shared_ptr<persistence::ConnectionPool> pool_;
  observability::Logger& logger_;
};

}  // namespace apex::idempotency
