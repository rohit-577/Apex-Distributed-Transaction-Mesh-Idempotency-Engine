#include "idempotency/IdempotencyService.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "idempotency/SimulatedOperation.hpp"
#include "observability/Logger.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex::idempotency {

namespace {

constexpr std::size_t kLoggedKeyPrefix = 16;

std::string in_progress_body() {
  return nlohmann::json({{"status", "processing"},
                         {"message", "Operation accepted and in progress. Retry with the same "
                                     "Idempotency-Key to receive the result. (Waiter multiplexing "
                                     "arrives in a later phase.)"}})
      .dump();
}

std::string conflict_body(const std::string& fingerprint) {
  return nlohmann::json({{"error", "idempotency_key_in_use"},
                         {"message", "This Idempotency-Key was already used for a different "
                                     "request. Retrying a different operation under the same key "
                                     "is rejected and was not executed."},
                         {"fingerprint", fingerprint}})
      .dump();
}

std::string failed_terminal_body(const persistence::IdempotencyRecord& record) {
  return nlohmann::json({{"error", "idempotency_already_failed"},
                         {"message", "This Idempotency-Key already reached FAILED and terminal "
                                     "records never restart. Use a new key for a new attempt."},
                         {"failure",
                          {{"error_code", record.error_code},
                           {"error_message", record.error_message}}}})
      .dump();
}

std::string unavailable_body() {
  return nlohmann::json({{"error", "storage_unavailable"},
                         {"message", "Durable idempotency storage is unreachable. Retry later "
                                     "with the same Idempotency-Key."}})
      .dump();
}

}  // namespace

IdempotencyService::IdempotencyService(std::shared_ptr<persistence::ConnectionPool> pool,
                                       observability::Logger& logger)
    : pool_(std::move(pool)), logger_(logger) {}

std::string IdempotencyService::safe_key(const std::string& key) {
  if (key.size() <= kLoggedKeyPrefix) {
    return key;
  }
  return key.substr(0, kLoggedKeyPrefix) + "...(len=" + std::to_string(key.size()) + ")";
}

OperationOutcome IdempotencyService::handle(const OperationRequest& request) {
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
  };
  const std::string logged_key = safe_key(request.key);

  OperationOutcome done;
  try {
    persistence::ConnectionPool::Guard checkout = pool_->acquire();
    persistence::PgConnection& db = checkout.connection();
    persistence::IdempotencyRepository repo;

    const persistence::AcquireResult acquired = repo.try_acquire(db, request.key,
                                                                request.fingerprint);
    if (acquired.outcome == persistence::AcquireOutcome::Created) {
      logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                   " transition=none->PROCESSING");
      // The simulated operation runs HERE, on this worker thread, between
      // the two durable transactions. A crash in this window leaves a
      // PROCESSING row: documented orphan, recovered by the later lease
      // phase (see docs/failure-model.md). No other transaction can observe
      // a half-written result because the result lands in one guarded UPDATE.
      const SimulatedResult op = run_simulated(request.canonical_body);
      if (op.success) {
        const bool transitioned =
            repo.complete(db, request.key, request.fingerprint, op.http_status, op.body,
                          op.content_type);
        // transitioned is false only if the row left PROCESSING under us,
        // which Phase 1 has no writer for — treat as storage-level surprise.
        logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                     " transition=PROCESSING->COMPLETED stored=" +
                     (transitioned ? "yes" : "NO") +
                     " latency_ms=" + std::to_string(elapsed_ms()));
        done.kind = OperationOutcome::Kind::Executed;
        done.http_status = op.http_status;
        done.body = op.body;
        done.content_type = op.content_type;
        return done;
      }
      const bool transitioned = repo.fail(db, request.key, request.fingerprint, op.error_code,
                                          op.error_message);
      logger_.warning("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                      " transition=PROCESSING->FAILED reason=" + op.error_code +
                      " stored=" + (transitioned ? "yes" : "NO") +
                      " latency_ms=" + std::to_string(elapsed_ms()));
      done.kind = OperationOutcome::Kind::Executed;
      done.http_status = op.http_status;
      done.body = op.body;
      done.content_type = op.content_type;
      return done;
    }

    const persistence::IdempotencyRecord& existing = acquired.record;
    if (existing.fingerprint != request.fingerprint) {
      logger_.warning("idempotency key=" + logged_key + " conflict stored_fp=" +
                      existing.fingerprint + " request_fp=" + request.fingerprint);
      done.kind = OperationOutcome::Kind::FingerprintConflict;
      done.http_status = 409;
      done.body = conflict_body(existing.fingerprint);
      return done;
    }
    switch (existing.status) {
      case persistence::RecordStatus::Processing:
        logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                     " duplicate-while-PROCESSING latency_ms=" + std::to_string(elapsed_ms()));
        done.kind = OperationOutcome::Kind::InProgress;
        done.http_status = 202;
        done.body = in_progress_body();
        return done;
      case persistence::RecordStatus::Completed: {
        logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                     " replay-COMPLETED latency_ms=" + std::to_string(elapsed_ms()));
        done.kind = OperationOutcome::Kind::Replayed;
        done.http_status = existing.http_status.value_or(200);
        done.body = existing.response_body;
        done.content_type = existing.response_content_type.empty()
                                ? "application/json"
                                : existing.response_content_type;
        return done;
      }
      case persistence::RecordStatus::Failed:
        logger_.warning("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                        " duplicate-after-FAILED reason=" + existing.error_code);
        done.kind = OperationOutcome::Kind::FailedTerminal;
        done.http_status = 409;
        done.body = failed_terminal_body(existing);
        return done;
    }
  } catch (const std::exception& e) {
    // Any database failure (unreachable, rollback, lost race invariant)
    // becomes a controlled 503. The key detail is logged; the message is
    // not echoed to the client (it may contain connection internals).
    logger_.error("idempotency key=" + logged_key + " storage failure: " + e.what());
    done.kind = OperationOutcome::Kind::StorageUnavailable;
    done.http_status = 503;
    done.body = unavailable_body();
    return done;
  }
  done.kind = OperationOutcome::Kind::StorageUnavailable;
  done.http_status = 503;
  done.body = unavailable_body();
  return done;
}

}  // namespace apex::idempotency
