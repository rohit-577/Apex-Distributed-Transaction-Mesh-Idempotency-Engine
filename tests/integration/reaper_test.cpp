// Background orphan-recovery tests (Phase 4): the reaper adopts abandoned
// PROCESSING rows through the production service path, races traffic and
// other instances to exactly one owner, respects eligibility, and shuts
// down cleanly. Deterministic: state-gated polls, barrier latches, no
// timing assumptions.

#include <chrono>
#include <latch>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/LeaseManager.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"
#include "recovery/OrphanReaper.hpp"

namespace apex {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config reaper_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 8;
  cfg.db_pool_size = 16;
  return cfg;
}

test::HttpResult post_key(std::uint16_t port, const std::string& key, const std::string& body) {
  return test::http_send_with_headers("127.0.0.1", port,
                                      boost::beast::http::verb::post, "/v1/operations", body,
                                      {{"Idempotency-Key", key},
                                       {"Content-Type", "application/json"}});
}

std::string fp_of(const std::string& body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return fp.hex;
}

std::string canonical_of(const std::string& body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return fp.canonical_body;
}

template <typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds bound = 15000ms) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

recovery::ReaperOptions fast_reaper_options() {
  // Prompt passes for tests (production uses 30 s). Interval timing only
  // schedules passes; every assertion below gates on durable state.
  recovery::ReaperOptions options;
  options.interval_ms = 200;
  options.batch_size = 10;
  options.eligible_after_ms = 0;
  return options;
}

TEST_F(PgFixture, ReaperAdoptsAbandonedOrphan) {
  // Planted PROCESSING (no lease, no traffic): the reaper adopts it through
  // service.handle, executing exactly once and committing epoch 2.
  REQUIRE_PG();
  REQUIRE_REDIS();
  auto executor = std::make_shared<GatedExecutor>(/*start_open=*/true);
  GateOpener guard(*executor);
  auto registry = std::make_shared<idempotency::WaiterRegistry>();
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto service = make_service(shared_pool(), quiet, shared_leases(), executor, registry,
                              shared_redis_client());

  const std::string key = unique_key("reaper-orphan");
  const std::string body = R"({"reap":"me"})";
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, fp_of(body), canonical_of(body)).outcome,
              persistence::AcquireOutcome::Created);
  }

  recovery::OrphanReaper reaper(service, shared_pool(), quiet, fast_reaper_options(),
                                   shared_metrics());
  reaper.start();
  ASSERT_TRUE(wait_until([&] {
    auto db = raw_connect();
    const auto record = repo().find_by_key(*db, key);
    return record.has_value() && record->status == persistence::RecordStatus::Completed;
  })) << "reaper never adopted the orphan";

  EXPECT_EQ(executor->executions(), 1u) << "exactly one recovery execution";
  {
    auto db = raw_connect();
    const auto record = repo().find_by_key(*db, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->fencing_epoch, 2);
  }
  EXPECT_GE(reaper.passes(), 1u);
  const auto metrics_after = shared_metrics()->snapshot();
  EXPECT_EQ(metrics_after.orphans_recovered, 1u) << "reaper recovery must be counted";
  EXPECT_GE(metrics_after.reaper_passes, 1u);
  reaper.stop();
}

TEST_F(PgFixture, ReaperSkipsActiveAndTerminalRows) {
  // Eligibility is real: a fresh PROCESSING row with a HELD lease and an
  // already-COMPLETED row are both left alone (the reaper resolves them
  // through handle(), which answers Wait/replay — never executes).
  REQUIRE_PG();
  REQUIRE_REDIS();
  auto executor = std::make_shared<GatedExecutor>(/*start_open=*/true);
  GateOpener guard(*executor);
  auto registry = std::make_shared<idempotency::WaiterRegistry>();
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto service = make_service(shared_pool(), quiet, shared_leases(), executor, registry,
                              shared_redis_client());

  const std::string active_key = unique_key("reaper-active");
  const std::string done_key = unique_key("reaper-done");
  const std::string body = R"({"reap":"skip"})";
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, active_key, fp_of(body), canonical_of(body)).outcome,
              persistence::AcquireOutcome::Created);
    ASSERT_EQ(repo().try_acquire(*db, done_key, fp_of(body), canonical_of(body)).outcome,
              persistence::AcquireOutcome::Created);
    ASSERT_TRUE(
        repo().complete(*db, done_key, fp_of(body), 1, 200, R"({"ok":1})", "application/json"));
  }
  const coordination::LeaseAttempt held = leases().try_acquire(active_key);
  ASSERT_EQ(held.result, coordination::LeaseAttempt::Result::Acquired);

  recovery::OrphanReaper reaper(service, shared_pool(), quiet, fast_reaper_options(),
                                   shared_metrics());
  reaper.start();
  // Gate on passes actually running (not on time): with an active lease and
  // a terminal row present, passes must observe and skip both.
  ASSERT_TRUE(wait_until([&] { return reaper.passes() >= 2; }));
  reaper.stop();

  EXPECT_EQ(executor->executions(), 0u) << "reaper must not execute for held/terminal rows";
  {
    auto db = raw_connect();
    EXPECT_EQ(repo().find_by_key(*db, active_key)->status,
              persistence::RecordStatus::Processing);
    EXPECT_EQ(repo().find_by_key(*db, active_key)->fencing_epoch, 1);
    EXPECT_EQ(repo().find_by_key(*db, done_key)->status,
              persistence::RecordStatus::Completed);
  }
  EXPECT_TRUE(leases().release(active_key, held.token));
}

TEST_F(PgFixture, ReaperRespectsEligibilityAge) {
  // Rows idle for less than eligible_after_ms are not picked up (efficiency
  // filter for actively-worked rows). Deterministic: eligible_after far in
  // the future => nothing happens; then direct traffic recovery still works.
  REQUIRE_PG();
  REQUIRE_REDIS();
  auto executor = std::make_shared<GatedExecutor>(/*start_open=*/true);
  GateOpener guard(*executor);
  auto registry = std::make_shared<idempotency::WaiterRegistry>();
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto service = make_service(shared_pool(), quiet, shared_leases(), executor, registry,
                              shared_redis_client());

  const std::string key = unique_key("reaper-age");
  const std::string body = R"({"reap":"age"})";
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, fp_of(body), canonical_of(body)).outcome,
              persistence::AcquireOutcome::Created);
  }

  recovery::ReaperOptions patient = fast_reaper_options();
  patient.eligible_after_ms = 3600000;  // 1 h: nothing is that idle in a test.
  recovery::OrphanReaper reaper(service, shared_pool(), quiet, patient, shared_metrics());
  reaper.start();
  // Passes run but find nothing eligible: the row stays untouched.
  ASSERT_TRUE(wait_until([&] { return reaper.passes() >= 2; }));
  reaper.stop();

  EXPECT_EQ(executor->executions(), 0u) << "fresh row must not be reaped";
  {
    auto db = raw_connect();
    EXPECT_EQ(repo().find_by_key(*db, key)->status, persistence::RecordStatus::Processing);
  }
}

TEST_F(PgFixture, ReaperRacesTrafficToExactlyOneOwner) {
  // Reaper + live HTTP request + second logical instance (direct handle)
  // converge on one execution for the same orphan. All three use the same
  // counting executor; the epoch CAS admits exactly one generation.
  REQUIRE_PG();
  REQUIRE_REDIS();
  auto executor = std::make_shared<GatedExecutor>(/*start_open=*/true);
  GateOpener guard(*executor);
  apex::observability::Logger quiet(apex::observability::Level::Error);

  auto registry_a = std::make_shared<idempotency::WaiterRegistry>();
  auto service_a = make_service(shared_pool(), quiet, shared_leases(), executor, registry_a,
                                shared_redis_client());
  auto registry_b = std::make_shared<idempotency::WaiterRegistry>();
  auto service_b = make_service(shared_pool(), quiet, shared_leases(), executor, registry_b,
                                shared_redis_client());

  const std::string key = unique_key("reaper-race");
  const std::string body = R"({"reap":"race"})";
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, fp_of(body), canonical_of(body)).outcome,
              persistence::AcquireOutcome::Created);
  }

  test::TestServer server(reaper_config(), service_a);
  recovery::OrphanReaper reaper(service_b, shared_pool(), quiet, fast_reaper_options(),
                                   shared_metrics());
  reaper.start();

  // Traffic and reaper start together (latch): whoever CASes first owns it.
  std::latch go{1};
  int http_status = 0;
  std::thread traffic([&] {
    go.wait();
    http_status = post_key(server.port(), key, body).status;
  });
  go.count_down();
  traffic.join();
  reaper.stop();

  EXPECT_TRUE(http_status == 200 || http_status == 202) << http_status;
  EXPECT_EQ(executor->executions(), 1u) << "exactly one recovery execution across all contenders";
  {
    auto db = raw_connect();
    const auto record = repo().find_by_key(*db, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, persistence::RecordStatus::Completed);
    EXPECT_EQ(record->fencing_epoch, 2);
  }
  // Converged either way: replay the winner's result through traffic.
  EXPECT_EQ(post_key(server.port(), key, body).status, 200);
}

TEST_F(PgFixture, ReaperStopsPromptlyOnShutdown) {
  // Shutdown order primitive: stop() returns promptly even mid-schedule,
  // is idempotent, and a stopped reaper never runs another pass.
  REQUIRE_PG();
  REQUIRE_REDIS();
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto service = make_service(shared_pool(), quiet, shared_leases());
  recovery::OrphanReaper reaper(service, shared_pool(), quiet, fast_reaper_options(),
                                   shared_metrics());
  reaper.start();
  EXPECT_TRUE(wait_until([&] { return reaper.passes() >= 2; })) << "reaper never ran";

  const auto stop_start = std::chrono::steady_clock::now();
  reaper.stop();
  reaper.stop();  // Idempotent: second call is a no-op, never hangs.
  EXPECT_LT(std::chrono::steady_clock::now() - stop_start, 10s) << "reaper stop hung";

  const std::uint64_t passes_at_stop = reaper.passes();
  std::this_thread::sleep_for(400ms);
  EXPECT_EQ(reaper.passes(), passes_at_stop) << "stopped reaper ran another pass";
}

}  // namespace
}  // namespace apex
