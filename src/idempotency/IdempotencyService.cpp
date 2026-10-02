#include "idempotency/IdempotencyService.hpp"

#include <chrono>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/OperationExecutor.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex::idempotency {

namespace {

using persistence::IdempotencyRecord;
using persistence::RecordStatus;

// Releases a held lease when the holder's work is done, whatever the
// outcome. Destruction is the release: every exit path below either holds
// no lease (nothing armed) or releases exactly once. Release is token-
// guarded and no-throw, so running it unconditionally is always safe — a
// superseded token simply reports "rejected" (TEST R5 exercises this).
class LeaseReleaser {
 public:
  LeaseReleaser(coordination::LeaseManager* leases, std::string key, std::string token)
      : leases_(leases), key_(std::move(key)), token_(std::move(token)) {}

  LeaseReleaser(const LeaseReleaser&) = delete;
  LeaseReleaser& operator=(const LeaseReleaser&) = delete;

  ~LeaseReleaser() {
    if (leases_ != nullptr && !token_.empty()) {
      leases_->release(key_, token_);
    }
  }

 private:
  coordination::LeaseManager* leases_;
  std::string key_;
  std::string token_;
};

std::string in_progress_body() {
  return nlohmann::json({{"status", "processing"},
                         {"message",
                          "Operation accepted and in progress. Retry with the same "
                          "Idempotency-Key to receive the result."}})
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

std::string failed_terminal_body(const IdempotencyRecord& record) {
  return nlohmann::json({{"error", "idempotency_already_failed"},
                         {"message", "This Idempotency-Key already reached FAILED and terminal "
                                     "records never restart. Use a new key for a new attempt."},
                         {"failure",
                          {{"error_code", record.error_code},
                           {"error_message", record.error_message}}}})
      .dump();
}

std::string stale_body(std::int64_t presented_epoch) {
  return nlohmann::json(
             {{"error", "stale_ownership_epoch"},
              {"message", "A newer ownership generation took over this operation before our "
                          "result landed. Our result was discarded; the current generation's "
                          "result is authoritative. Retry with the same Idempotency-Key to "
                          "receive it."},
              {"presented_epoch", presented_epoch}})
      .dump();
}

std::string redis_unavailable_body() {
  return nlohmann::json({{"error", "redis_unavailable"},
                         {"message", "Lease coordination is unreachable. The request was NOT "
                                     "executed and no ownership was taken. Retry later with the "
                                     "same Idempotency-Key."}})
      .dump();
}

std::string storage_unavailable_body() {
  return nlohmann::json({{"error", "storage_unavailable"},
                         {"message", "Durable idempotency storage is unreachable. Retry later "
                                     "with the same Idempotency-Key."}})
      .dump();
}

// Maps an already-read record to its non-executing outcome. PROCESSING with
// our fingerprint yields WAIT (multiplexing): another generation owns the
// operation and this caller must suspend, never execute (INV-MUX-01).
OperationOutcome map_stored_record(const IdempotencyRecord& existing,
                                    const std::string& request_fingerprint) {
  OperationOutcome done;
  if (existing.fingerprint != request_fingerprint) {
    done.kind = OperationOutcome::Kind::FingerprintConflict;
    done.http_status = 409;
    done.body = conflict_body(existing.fingerprint);
    return done;
  }
  switch (existing.status) {
    case RecordStatus::Processing:
      done.kind = OperationOutcome::Kind::Wait;
      done.http_status = 0;  // Not an HTTP verdict: the caller suspends.
      done.body.clear();
      return done;
    case RecordStatus::Completed:
      done.kind = OperationOutcome::Kind::Replayed;
      done.http_status = existing.http_status.value_or(200);
      done.body = existing.response_body;
      done.content_type = existing.response_content_type.empty() ? "application/json"
                                                                 : existing.response_content_type;
      return done;
    case RecordStatus::Failed:
      done.kind = OperationOutcome::Kind::FailedTerminal;
      done.http_status = 409;
      done.body = failed_terminal_body(existing);
      return done;
  }
  done.kind = OperationOutcome::Kind::StorageUnavailable;
  done.http_status = 503;
  done.body = storage_unavailable_body();
  return done;
}

}  // namespace

IdempotencyService::IdempotencyService(ServiceDependencies deps, observability::Logger& logger)
    : deps_(std::move(deps)), logger_(logger) {}

OperationOutcome IdempotencyService::handle(const OperationRequest& request) {
  // Single counting funnel: every outcome kind maps to exactly one process
  // counter here, so no decision path can silently skip instrumentation.
  // Wait is intentionally uncounted (Session counts waiter starts once per
  // waiter lifetime; per-verdict counting would overcount recheck cycles).
  OperationOutcome outcome;
  try {
    outcome = handle_inner(request);
  } catch (const std::exception&) {
    // Unreachable (handle_inner is no-throw); kept so worker threads can
    // never die from an escaping exception.
    outcome.kind = OperationOutcome::Kind::StorageUnavailable;
    outcome.http_status = 503;
    outcome.body = storage_unavailable_body();
  }
  auto& metrics = *deps_.metrics;
  switch (outcome.kind) {
    case OperationOutcome::Kind::Executed:
    case OperationOutcome::Kind::Recovered:
      metrics.increment_executions();
      if (outcome.http_status >= 200 && outcome.http_status < 300) {
        metrics.increment_executions_completed();
      } else {
        metrics.increment_executions_failed();
      }
      break;
    case OperationOutcome::Kind::Replayed:
      metrics.increment_completed_replays();
      break;
    case OperationOutcome::Kind::InProgress:
      metrics.increment_deferred_processing_answers();
      break;
    case OperationOutcome::Kind::FingerprintConflict:
      metrics.increment_fingerprint_conflicts();
      break;
    case OperationOutcome::Kind::FailedTerminal:
      metrics.increment_failed_terminal_answers();
      break;
    case OperationOutcome::Kind::StaleEpoch:
      metrics.increment_stale_rejections();
      break;
    case OperationOutcome::Kind::RedisUnavailable:
      metrics.increment_lease_unavailable();
      break;
    case OperationOutcome::Kind::StorageUnavailable:
      metrics.increment_pg_failures();
      break;
    case OperationOutcome::Kind::Wait:
      break;  // Counted once per waiter lifetime in Session::enter_wait.
  }
  return outcome;
}

OperationOutcome IdempotencyService::handle_inner(const OperationRequest& request) {
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
  };
  const std::string logged_key = observability::safe_key(request.key) + " cid=" +
                                   (request.correlation_id.empty() ? "-" : request.correlation_id);

  OperationOutcome done;
  const auto storage_failure = [&](const std::string& what) {
    logger_.error("idempotency key=" + logged_key + " storage failure: " + what);
    done.kind = OperationOutcome::Kind::StorageUnavailable;
    done.http_status = 503;
    done.body = storage_unavailable_body();
    return done;
  };

  try {
    persistence::ConnectionPool::Guard checkout = deps_.pool->acquire();
    persistence::PgConnection& db = checkout.connection();
    persistence::IdempotencyRepository repo;

    // Read-first: terminal rows and fingerprint conflicts are answered from
    // PostgreSQL WITHOUT touching Redis — so replay works during a Redis
    // outage and conflicts never consume leases.
    const std::optional<IdempotencyRecord> first_read = repo.find_by_key(db, request.key);
    if (first_read.has_value() &&
        (first_read->status != RecordStatus::Processing ||
         first_read->fingerprint != request.fingerprint)) {
      return map_stored_record(*first_read, request.fingerprint);
    }

    if (deps_.leases == nullptr) {
      return redis_unavailable(done, logged_key, "no lease manager wired");
    }

    if (!first_read.has_value()) {
      return handle_first_request(db, repo, request, logged_key, elapsed_ms, done);
    }
    return handle_processing_seen(db, repo, request, *first_read, logged_key, elapsed_ms, done);
  } catch (const std::exception& e) {
    return storage_failure(e.what());
  }
}

OperationOutcome IdempotencyService::handle_first_request(
    persistence::PgConnection& db, persistence::IdempotencyRepository& repo,
    const OperationRequest& request, const std::string& logged_key,
    const std::function<long long()>& elapsed_ms, OperationOutcome& done) {
  // CASE A: no durable row. The Redis lease decides who may create it; the
  // database decides the row. Neither system alone is sufficient.
  const coordination::LeaseAttempt attempt = deps_.leases->try_acquire(request.key);
  if (attempt.result == coordination::LeaseAttempt::Result::RedisUnavailable) {
    // Fail closed BEFORE creating anything: no row, no orphan, safe retry.
    return redis_unavailable(done, logged_key);
  }
  if (attempt.result == coordination::LeaseAttempt::Result::HeldByOther) {
    // Crash window A: a previous holder took the lease but never inserted
    // (or hasn't committed yet). Its generation owns the key right now and
    // there is no durable row to wait on, so defer without creating a rival
    // row. The retry converges once the row exists.
    logger_.info("idempotency key=" + logged_key + " lease held with no durable row (202)");
    done.kind = OperationOutcome::Kind::InProgress;
    done.http_status = 202;
    done.body = in_progress_body();
    return done;
  }
  LeaseReleaser releaser(deps_.leases.get(), request.key, attempt.token);

  const persistence::AcquireResult acquired =
      repo.try_acquire(db, request.key, request.fingerprint, request.canonical_body);
  if (acquired.outcome == persistence::AcquireOutcome::Created) {
    logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                 " transition=none->PROCESSING epoch=1");
    return execute_owned(db, repo, request, /*epoch=*/1, logged_key, elapsed_ms, done,
                         OperationOutcome::Kind::Executed);
  }
  // Narrow but real: the previous holder's INSERT committed between our
  // find_by_key and our INSERT. We hold the lease, so recovery with the
  // observed epoch is the correct continuation (not a conflict, not a 202).
  return recover_with_lease(db, repo, request, acquired.record, logged_key, elapsed_ms, done);
}

OperationOutcome IdempotencyService::handle_processing_seen(
    persistence::PgConnection& db, persistence::IdempotencyRepository& repo,
    const OperationRequest& request, const IdempotencyRecord& observed,
    const std::string& logged_key, const std::function<long long()>& elapsed_ms,
    OperationOutcome& done) {
  // CASE B/C: PROCESSING row, same fingerprint. The Redis key tells liveness:
  // present => an owner is (or may still be) active => WAIT for it (the
  // caller suspends; multiplexing, INV-MUX-01). Absent is only a HINT
  // (INV-09): the epoch CAS below decides ownership.
  bool held = false;
  try {
    held = deps_.leases->is_held(request.key);
  } catch (const std::exception& e) {
    return redis_unavailable(done, logged_key, e.what());
  }
  if (held) {
    logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                 " duplicate-while-PROCESSING owner-active => wait latency_ms=" +
                 std::to_string(elapsed_ms()));
    done.kind = OperationOutcome::Kind::Wait;
    done.http_status = 0;
    done.body.clear();
    return done;
  }

  const coordination::LeaseAttempt attempt = deps_.leases->try_acquire(request.key);
  if (attempt.result == coordination::LeaseAttempt::Result::RedisUnavailable) {
    return redis_unavailable(done, logged_key);
  }
  if (attempt.result == coordination::LeaseAttempt::Result::HeldByOther) {
    // Lost the lease race after seeing no lease: whoever took it owns
    // recovery now. Re-read the durable row instead of assuming: it may
    // already be terminal (respond from it) or still PROCESSING (wait on
    // the new holder). Never 202 blindly — the state may have moved.
    const std::optional<IdempotencyRecord> fresh = repo.find_by_key(db, request.key);
    if (!fresh.has_value()) {
      logger_.error("idempotency key=" + logged_key + " lost lease race and row vanished");
      done.kind = OperationOutcome::Kind::StorageUnavailable;
      done.http_status = 503;
      done.body = storage_unavailable_body();
      return done;
    }
    return map_stored_record(*fresh, request.fingerprint);
  }
  LeaseReleaser releaser(deps_.leases.get(), request.key, attempt.token);
  return recover_with_lease(db, repo, request, observed, logged_key, elapsed_ms, done);
}

OperationOutcome IdempotencyService::recover_with_lease(
    persistence::PgConnection& db, persistence::IdempotencyRepository& repo,
    const OperationRequest& request, const IdempotencyRecord& observed,
    const std::string& logged_key, const std::function<long long()>& elapsed_ms,
    OperationOutcome& done) {
  // CASE C: advance exactly one generation past what we observed. A
  // concurrent recoverer presenting the same epoch loses deterministically;
  // a row that moved on (terminal, or newer epoch) yields nullopt and we
  // answer from the fresh durable state instead of assuming.
  deps_.metrics->increment_recovery_attempts();
  const std::optional<std::int64_t> next =
      repo.try_recover(db, request.key, request.fingerprint, observed.fencing_epoch);
  if (!next.has_value()) {
    deps_.metrics->increment_recovery_losses();
    const std::optional<IdempotencyRecord> fresh = repo.find_by_key(db, request.key);
    if (!fresh.has_value()) {
      // Rows are never deleted: reaching here means storage-level surprise.
      logger_.error("idempotency key=" + logged_key + " lost recovery race and row vanished");
      done.kind = OperationOutcome::Kind::StorageUnavailable;
      done.http_status = 503;
      done.body = storage_unavailable_body();
      return done;
    }
    logger_.info("idempotency key=" + logged_key + " lost epoch race, re-read status=" +
                 persistence::to_string(fresh->status));
    return map_stored_record(*fresh, request.fingerprint);
  }

  logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
               " ownership-recovered epoch=" + std::to_string(observed.fencing_epoch) + "->" +
               std::to_string(*next));
  deps_.metrics->increment_recovery_wins();
  return execute_owned(db, repo, request, *next, logged_key, elapsed_ms, done,
                       OperationOutcome::Kind::Recovered);
}

OperationOutcome IdempotencyService::execute_owned(
    persistence::PgConnection& db, persistence::IdempotencyRepository& repo,
    const OperationRequest& request, std::int64_t epoch, const std::string& logged_key,
    const std::function<long long()>& elapsed_ms, OperationOutcome& done,
    OperationOutcome::Kind executed_kind) {
  // The operation runs HERE, on this worker thread, holding the Redis lease
  // and the durable epoch. A crash in this window orphans a PROCESSING row
  // at our epoch — recoverable by the next generation (failure-model.md).
  // The terminal write presents our epoch IN the predicate: if recovery
  // advanced past us while we ran, we affect zero rows and our result is
  // discarded (FENCING INVARIANT) — never merged, never overwritten.
  const ExecutionResult op = deps_.executor->execute(request.canonical_body);
  const bool transitioned = op.success
                                ? repo.complete(db, request.key, request.fingerprint, epoch,
                                                op.http_status, op.body, op.content_type)
                                : repo.fail(db, request.key, request.fingerprint, epoch,
                                            op.error_code, op.error_message);
  if (!transitioned) {
    logger_.warning("idempotency key=" + logged_key + " fp=" + request.fingerprint +
                    " STALE-EPOCH rejection presented=" + std::to_string(epoch));
    done.kind = OperationOutcome::Kind::StaleEpoch;
    done.http_status = 409;
    done.body = stale_body(epoch);
    return done;
  }
  logger_.info("idempotency key=" + logged_key + " fp=" + request.fingerprint +
               " transition=PROCESSING->" + (op.success ? "COMPLETED" : "FAILED") +
               " epoch=" + std::to_string(epoch) +
               " latency_ms=" + std::to_string(elapsed_ms()));
  broadcast_completion(request.key, request.fingerprint, logged_key);
  done.kind = executed_kind;
  done.http_status = op.http_status;
  done.body = op.body;
  done.content_type = op.content_type;
  return done;
}

void IdempotencyService::broadcast_completion(const std::string& key,
                                              const std::string& fingerprint,
                                              const std::string& logged_key) {
  // Owner completion order (§14): the durable commit above is already done,
  // so notification is pure wake-up from here on. Publish first (cross-node
  // waiters), then local registry (same-process waiters); EITHER may fail
  // or be missed without harming correctness, because every woken waiter
  // re-reads PostgreSQL and every unwoken waiter falls back to its timer.
  // Publish is best-effort: a RedisError here must NOT fail the committed
  // operation (CASE 1 of the failure matrix).
  const std::string channel = WaiterRegistry::channel_for(key, fingerprint);
  if (deps_.redis != nullptr) {
    try {
      const long long receivers = deps_.redis->publish(channel, "");
      logger_.debug("idempotency key=" + logged_key + " published completion receivers=" +
                    std::to_string(receivers));
    } catch (const std::exception& e) {
      logger_.warning("idempotency key=" + logged_key +
                      " completion publish failed (waiters fall back to recheck): " + e.what());
    }
  }
  if (deps_.registry != nullptr) {
    const std::size_t woken = deps_.registry->notify(channel);
    logger_.debug("idempotency key=" + logged_key +
                  " local waiters woken=" + std::to_string(woken));
  }
}

OperationOutcome IdempotencyService::redis_unavailable(OperationOutcome& done,
                                                       const std::string& logged_key,
                                                       const std::string& what) {
  logger_.error("idempotency key=" + logged_key + " coordination failure: " + what);
  done.kind = OperationOutcome::Kind::RedisUnavailable;
  done.http_status = 503;
  done.body = redis_unavailable_body();
  return done;
}

}  // namespace apex::idempotency
