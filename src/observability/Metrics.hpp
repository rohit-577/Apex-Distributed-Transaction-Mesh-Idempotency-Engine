#pragma once

// Lightweight process metrics (Phase 5): lock-free atomic counters with a
// snapshot + Prometheus-text renderer. Deliberately NOT a metrics platform:
// no histograms, no per-key/per-request labels, no remote push — a bounded
// set of uint64 counters plus a `/metrics` endpoint is the whole surface.
//
// Cardinality discipline: labels are fixed strings (outcome kinds, error
// classes). Raw idempotency keys, fingerprints, tokens, and bodies NEVER
// appear in metric names or label values — a malicious client cannot grow
// metric memory by sending arbitrary keys (tested: unique keys absent from
// rendered output).
//
// Threading: every counter is a relaxed atomic increment; snapshotting is a
// plain load per counter (slightly torn snapshots across counters are
// acceptable for operational telemetry — each counter is individually exact).

#include <atomic>
#include <cstdint>
#include <string>

namespace apex::observability {

struct MetricsSnapshot {
  // Requests.
  std::uint64_t requests_total{0};
  std::uint64_t validation_failures{0};
  std::uint64_t fingerprint_conflicts{0};
  std::uint64_t completed_replays{0};
  std::uint64_t failed_terminal_answers{0};
  std::uint64_t deferred_processing_answers{0};
  std::uint64_t waiters_started{0};
  std::uint64_t waiter_timeouts{0};
  std::uint64_t waiter_aborted{0};
  // Waiters that converged on a terminal result after waiting (as opposed to
  // immediate replay: those never suspend). wait_time_ms_total accumulates
  // their suspended durations (mean derivable; no histogram by design).
  std::uint64_t waiter_completions{0};
  std::uint64_t wait_time_ms_total{0};
  // Currently suspended waiters (gauge, not cumulative).
  std::uint64_t waiters_active{0};
  // Ownership.
  std::uint64_t lease_acquired{0};
  std::uint64_t lease_held{0};
  std::uint64_t lease_unavailable{0};
  std::uint64_t lease_releases{0};
  std::uint64_t lease_release_rejected{0};
  std::uint64_t recovery_attempts{0};
  std::uint64_t recovery_wins{0};
  std::uint64_t recovery_losses{0};
  std::uint64_t stale_rejections{0};
  // Execution.
  std::uint64_t executions{0};
  std::uint64_t executions_completed{0};
  std::uint64_t executions_failed{0};
  // Waiting.
  std::uint64_t notification_wakeups{0};
  std::uint64_t fallback_wakeups{0};
  std::uint64_t waiter_cancellations{0};
  // Dependencies.
  std::uint64_t pg_failures{0};
  std::uint64_t redis_failures{0};
  std::uint64_t subscriber_reconnects{0};
  // Recovery.
  std::uint64_t reaper_passes{0};
  std::uint64_t orphans_found{0};
  std::uint64_t orphans_recovered{0};
  std::uint64_t recovery_conflicts{0};
};

class Metrics {
 public:
  Metrics() = default;

  Metrics(const Metrics&) = delete;
  Metrics& operator=(const Metrics&) = delete;

  void increment_requests_total() { ++requests_total_; }
  void increment_validation_failures() { ++validation_failures_; }
  void increment_fingerprint_conflicts() { ++fingerprint_conflicts_; }
  void increment_completed_replays() { ++completed_replays_; }
  void increment_failed_terminal_answers() { ++failed_terminal_answers_; }
  void increment_deferred_processing_answers() { ++deferred_processing_answers_; }
  void increment_waiters_started() { ++waiters_started_; }
  void increment_waiter_timeouts() { ++waiter_timeouts_; }
  void increment_waiter_aborted() { ++waiter_aborted_; }
  void increment_waiter_completions() { ++waiter_completions_; }
  void add_wait_time_ms(std::uint64_t ms) { wait_time_ms_total_ += ms; }
  void increment_waiters_active() { ++waiters_active_; }
  void decrement_waiters_active() { --waiters_active_; }
  void increment_lease_acquired() { ++lease_acquired_; }
  void increment_lease_held() { ++lease_held_; }
  void increment_lease_unavailable() { ++lease_unavailable_; }
  void increment_lease_releases() { ++lease_releases_; }
  void increment_lease_release_rejected() { ++lease_release_rejected_; }
  void increment_recovery_attempts() { ++recovery_attempts_; }
  void increment_recovery_wins() { ++recovery_wins_; }
  void increment_recovery_losses() { ++recovery_losses_; }
  void increment_stale_rejections() { ++stale_rejections_; }
  void increment_executions() { ++executions_; }
  void increment_executions_completed() { ++executions_completed_; }
  void increment_executions_failed() { ++executions_failed_; }
  void increment_notification_wakeups() { ++notification_wakeups_; }
  void increment_fallback_wakeups() { ++fallback_wakeups_; }
  void increment_waiter_cancellations() { ++waiter_cancellations_; }
  void increment_pg_failures() { ++pg_failures_; }
  void increment_redis_failures() { ++redis_failures_; }
  void increment_subscriber_reconnects() { ++subscriber_reconnects_; }
  void increment_reaper_passes() { ++reaper_passes_; }
  void increment_orphans_found() { ++orphans_found_; }
  void increment_orphans_recovered() { ++orphans_recovered_; }
  void increment_recovery_conflicts() { ++recovery_conflicts_; }

  [[nodiscard]] MetricsSnapshot snapshot() const;
  // Prometheus exposition format (text/plain; version 0.0.4), one
  // `apex_<name> <value>` line per counter plus HELP/TYPE headers.
  [[nodiscard]] std::string render_prometheus() const;

 private:
  std::atomic<std::uint64_t> requests_total_{0};
  std::atomic<std::uint64_t> validation_failures_{0};
  std::atomic<std::uint64_t> fingerprint_conflicts_{0};
  std::atomic<std::uint64_t> completed_replays_{0};
  std::atomic<std::uint64_t> failed_terminal_answers_{0};
  std::atomic<std::uint64_t> deferred_processing_answers_{0};
  std::atomic<std::uint64_t> waiters_started_{0};
  std::atomic<std::uint64_t> waiter_timeouts_{0};
  std::atomic<std::uint64_t> waiter_aborted_{0};
  std::atomic<std::uint64_t> waiter_completions_{0};
  std::atomic<std::uint64_t> wait_time_ms_total_{0};
  std::atomic<std::uint64_t> waiters_active_{0};
  std::atomic<std::uint64_t> lease_acquired_{0};
  std::atomic<std::uint64_t> lease_held_{0};
  std::atomic<std::uint64_t> lease_unavailable_{0};
  std::atomic<std::uint64_t> lease_releases_{0};
  std::atomic<std::uint64_t> lease_release_rejected_{0};
  std::atomic<std::uint64_t> recovery_attempts_{0};
  std::atomic<std::uint64_t> recovery_wins_{0};
  std::atomic<std::uint64_t> recovery_losses_{0};
  std::atomic<std::uint64_t> stale_rejections_{0};
  std::atomic<std::uint64_t> executions_{0};
  std::atomic<std::uint64_t> executions_completed_{0};
  std::atomic<std::uint64_t> executions_failed_{0};
  std::atomic<std::uint64_t> notification_wakeups_{0};
  std::atomic<std::uint64_t> fallback_wakeups_{0};
  std::atomic<std::uint64_t> waiter_cancellations_{0};
  std::atomic<std::uint64_t> pg_failures_{0};
  std::atomic<std::uint64_t> redis_failures_{0};
  std::atomic<std::uint64_t> subscriber_reconnects_{0};
  std::atomic<std::uint64_t> reaper_passes_{0};
  std::atomic<std::uint64_t> orphans_found_{0};
  std::atomic<std::uint64_t> orphans_recovered_{0};
  std::atomic<std::uint64_t> recovery_conflicts_{0};
};

}  // namespace apex::observability
