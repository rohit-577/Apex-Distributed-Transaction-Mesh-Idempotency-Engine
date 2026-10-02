#include "observability/Metrics.hpp"

#include <sstream>

namespace apex::observability {

MetricsSnapshot Metrics::snapshot() const {
  MetricsSnapshot snap;
  snap.requests_total = requests_total_.load();
  snap.validation_failures = validation_failures_.load();
  snap.fingerprint_conflicts = fingerprint_conflicts_.load();
  snap.completed_replays = completed_replays_.load();
  snap.failed_terminal_answers = failed_terminal_answers_.load();
  snap.deferred_processing_answers = deferred_processing_answers_.load();
  snap.waiters_started = waiters_started_.load();
  snap.waiter_timeouts = waiter_timeouts_.load();
  snap.waiter_aborted = waiter_aborted_.load();
  snap.waiter_completions = waiter_completions_.load();
  snap.wait_time_ms_total = wait_time_ms_total_.load();
  snap.waiters_active = waiters_active_.load();
  snap.lease_acquired = lease_acquired_.load();
  snap.lease_held = lease_held_.load();
  snap.lease_unavailable = lease_unavailable_.load();
  snap.lease_releases = lease_releases_.load();
  snap.lease_release_rejected = lease_release_rejected_.load();
  snap.recovery_attempts = recovery_attempts_.load();
  snap.recovery_wins = recovery_wins_.load();
  snap.recovery_losses = recovery_losses_.load();
  snap.stale_rejections = stale_rejections_.load();
  snap.executions = executions_.load();
  snap.executions_completed = executions_completed_.load();
  snap.executions_failed = executions_failed_.load();
  snap.notification_wakeups = notification_wakeups_.load();
  snap.fallback_wakeups = fallback_wakeups_.load();
  snap.waiter_cancellations = waiter_cancellations_.load();
  snap.pg_failures = pg_failures_.load();
  snap.redis_failures = redis_failures_.load();
  snap.subscriber_reconnects = subscriber_reconnects_.load();
  snap.reaper_passes = reaper_passes_.load();
  snap.orphans_found = orphans_found_.load();
  snap.orphans_recovered = orphans_recovered_.load();
  snap.recovery_conflicts = recovery_conflicts_.load();
  return snap;
}

std::string Metrics::render_prometheus() const {
  const MetricsSnapshot snap = snapshot();
  std::ostringstream out;
  out << "# HELP apex_requests_total Total POST /v1/operations requests handled.\n"
         "# TYPE apex_requests_total counter\n"
         "apex_requests_total "
      << snap.requests_total << "\n";
  const auto line = [&out](const char* name, std::uint64_t value) {
    out << "# TYPE apex_" << name << " counter\napex_" << name << " " << value << "\n";
  };
  line("validation_failures", snap.validation_failures);
  line("fingerprint_conflicts", snap.fingerprint_conflicts);
  line("completed_replays", snap.completed_replays);
  line("failed_terminal_answers", snap.failed_terminal_answers);
  line("deferred_processing_answers", snap.deferred_processing_answers);
  line("waiters_started", snap.waiters_started);
  line("waiter_timeouts", snap.waiter_timeouts);
  line("waiter_aborted", snap.waiter_aborted);
  line("waiter_completions", snap.waiter_completions);
  line("wait_time_ms_total", snap.wait_time_ms_total);
  out << "# TYPE apex_waiters_active gauge\napex_waiters_active " << snap.waiters_active << "\n";
  line("lease_acquired", snap.lease_acquired);
  line("lease_held", snap.lease_held);
  line("lease_unavailable", snap.lease_unavailable);
  line("lease_releases", snap.lease_releases);
  line("lease_release_rejected", snap.lease_release_rejected);
  line("recovery_attempts", snap.recovery_attempts);
  line("recovery_wins", snap.recovery_wins);
  line("recovery_losses", snap.recovery_losses);
  line("stale_rejections", snap.stale_rejections);
  line("executions", snap.executions);
  line("executions_completed", snap.executions_completed);
  line("executions_failed", snap.executions_failed);
  line("notification_wakeups", snap.notification_wakeups);
  line("fallback_wakeups", snap.fallback_wakeups);
  line("waiter_cancellations", snap.waiter_cancellations);
  line("pg_failures", snap.pg_failures);
  line("redis_failures", snap.redis_failures);
  line("subscriber_reconnects", snap.subscriber_reconnects);
  line("reaper_passes", snap.reaper_passes);
  line("orphans_found", snap.orphans_found);
  line("orphans_recovered", snap.orphans_recovered);
  line("recovery_conflicts", snap.recovery_conflicts);
  return out.str();
}

}  // namespace apex::observability
