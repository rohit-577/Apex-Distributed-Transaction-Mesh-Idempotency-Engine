#include "recovery/OrphanReaper.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include "idempotency/IdempotencyService.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex::recovery {

OrphanReaper::OrphanReaper(std::shared_ptr<idempotency::IdempotencyService> service,
                           std::shared_ptr<persistence::ConnectionPool> pool,
                           observability::Logger& logger, ReaperOptions options,
                           std::shared_ptr<observability::Metrics> metrics)
    : service_(std::move(service)),
      pool_(std::move(pool)),
      logger_(logger),
      metrics_(std::move(metrics)),
      options_(options) {}

OrphanReaper::~OrphanReaper() {
  stop();
}

void OrphanReaper::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return;
  }
  started_ = true;
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void OrphanReaper::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
      return;
    }
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
}

void OrphanReaper::run() {
  // Wakeable sleep between passes: prompt shutdown, zero busy looping.
  // The interval is scheduling, never correctness — a missed or late pass
  // only delays adoption, and traffic-driven recovery covers the gap.
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    lock.unlock();
    try {
      run_pass();
    } catch (const std::exception& e) {
      // Background coordination thread: never die on a pass failure.
      // run_pass already funnels expected failures into warnings; this is
      // the backstop for the unexpected.
      logger_.error(std::string("orphan-reaper pass failed unexpectedly: ") + e.what());
    } catch (...) {
      logger_.error("orphan-reaper pass failed with unknown error");
    }
    lock.lock();
    wake_.wait_for(lock, std::chrono::milliseconds(options_.interval_ms),
                   [this] { return stop_; });
  }
}

void OrphanReaper::run_pass() {
  ++passes_;
  metrics_->increment_reaper_passes();
  std::vector<persistence::IdempotencyRepository::OrphanCandidate> orphans;
  try {
    persistence::ConnectionPool::Guard checkout = pool_->acquire();
    orphans = persistence::IdempotencyRepository{}.find_orphans(
        checkout.connection(), options_.batch_size, options_.eligible_after_ms);
  } catch (const std::exception& e) {
    // PostgreSQL down (or pool closed at shutdown): log and try the next
    // pass. Nothing was decided, nothing is half-done — the scan is
    // read-only and each adoption is independently atomic.
    logger_.warning(std::string("orphan-reaper pass skipped (storage unavailable?): ") + e.what());
    return;
  }
  if (orphans.empty()) {
    return;
  }
  candidates_seen_ += orphans.size();
  for (std::size_t i = 0; i < orphans.size(); ++i) {
    metrics_->increment_orphans_found();
  }
  logger_.info("orphan-reaper pass: " + std::to_string(orphans.size()) + " candidate(s)");
  for (const auto& orphan : orphans) {
    // THE critical reuse: the exact production path (lease -> CAS ->
    // execute -> fenced write). Races with traffic, other reapers, and
    // other instances all collapse into the epoch CAS — one winner per
    // generation, everyone else waits/replays/retries. handle() is
    // no-throw by contract, so one bad row cannot kill the pass.
    const idempotency::OperationRequest request{orphan.key, orphan.fingerprint,
                                                orphan.canonical_body};
    const idempotency::OperationOutcome outcome = service_->handle(request);
    using Kind = idempotency::OperationOutcome::Kind;
    if (outcome.kind == Kind::Executed || outcome.kind == Kind::Recovered) {
      ++recovered_;
      metrics_->increment_orphans_recovered();
      logger_.info("orphan-reaper recovered key=" + observability::safe_key(orphan.key));
    } else if (outcome.kind != Kind::StorageUnavailable &&
               outcome.kind != Kind::RedisUnavailable) {
      // Lost the race or found settled state: contention worth counting,
      // degradation is counted at its own funnel (pg/lease counters).
      metrics_->increment_recovery_conflicts();
    }
  }
}

}  // namespace apex::recovery
