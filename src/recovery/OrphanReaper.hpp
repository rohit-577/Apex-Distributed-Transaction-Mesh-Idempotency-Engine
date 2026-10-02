#pragma once

// Background orphan recovery: traffic-independent adoption of abandoned
// PROCESSING rows. Phase 2 left orphans waiting for the next same-key
// request; the reaper closes that gap on a schedule.
//
// The reaper invents NO ownership mechanism. Each pass:
//   1. SELECTs a bounded batch of orphan candidates (PROCESSING + stored
//      body + idle past the eligibility threshold, oldest first — served by
//      the partial index, never a full scan).
//   2. Runs each candidate through the SAME IdempotencyService::handle()
//      every HTTP request uses: lease acquisition, epoch CAS, fenced
//      terminal write. Concurrent traffic, other reapers, and other
//      instances resolve through the identical CAS — exactly one owner per
//      generation, no double execution, fencing intact.
//   3. Candidates whose lease is held (active owner) or already terminal
//      resolve to Wait/replay inside handle() and are simply skipped.
//
// Threading: ONE dedicated thread sleeping on a condition variable between
// passes (wakeable, no busy loop, never an Asio I/O thread, never per
// request). stop() is idempotent, joins boundedly (in-flight handle() calls
// are ordinary short operations), and never throws. Shutdown order (main):
// stop acceptor -> stop reaper -> registry shutdown -> subscriber stop ->
// stop io_context.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace apex::observability {
class Logger;
class Metrics;
}

namespace apex::persistence {
class ConnectionPool;
}

namespace apex::idempotency {
class IdempotencyService;
}

namespace apex::recovery {

struct ReaperOptions {
  // Pass interval. Missed passes (slow pass) simply shift the schedule;
  // correctness never depends on pass timing.
  long long interval_ms{30000};
  // Max candidates per pass (bounds one pass's DB + Redis + pool pressure).
  int batch_size{10};
  // Only rows idle longer than this are eligible. Keeps the reaper off
  // actively-worked rows whose owner is merely slow (their lease is held
  // anyway, so handle() would Wait — this is purely an efficiency filter).
  long long eligible_after_ms{30000};
};

class OrphanReaper {
 public:
  // `metrics` is required: passes, candidates, adoptions, and contention
  // outcomes are all counted (no silent background work).
  OrphanReaper(std::shared_ptr<idempotency::IdempotencyService> service,
               std::shared_ptr<persistence::ConnectionPool> pool,
               observability::Logger& logger, ReaperOptions options,
               std::shared_ptr<observability::Metrics> metrics);

  OrphanReaper(const OrphanReaper&) = delete;
  OrphanReaper& operator=(const OrphanReaper&) = delete;

  ~OrphanReaper();

  // Starts the background thread (idempotent). Safe to call before the
  // server listens; the first pass simply finds nothing or recovers.
  void start();
  // Signals stop and joins the thread. Idempotent, never throws.
  void stop();

  // Observability (Phase 5 wires these into metrics; atomics make them
  // free to maintain now).
  [[nodiscard]] std::uint64_t passes() const { return passes_.load(); }
  [[nodiscard]] std::uint64_t candidates_seen() const { return candidates_seen_.load(); }
  [[nodiscard]] std::uint64_t recovered() const { return recovered_.load(); }

 private:
  void run();
  void run_pass();

  std::shared_ptr<idempotency::IdempotencyService> service_;
  std::shared_ptr<persistence::ConnectionPool> pool_;
  observability::Logger& logger_;
  std::shared_ptr<observability::Metrics> metrics_;
  ReaperOptions options_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_{false};
  bool started_{false};
  std::atomic<std::uint64_t> passes_{0};
  std::atomic<std::uint64_t> candidates_seen_{0};
  std::atomic<std::uint64_t> recovered_{0};
};

}  // namespace apex::recovery
