// Waiter lifecycle tests (P3-04/05/06/08/09/10/13): replay without
// execution, missed-notification fallback, Redis-down behavior, and waiter
// timeout — all deterministic, no sleeps for synchronization.

#include <atomic>
#include <chrono>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config waiter_config() {
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

TEST_F(PgFixture, CompletedReplayExecutesNothing) {
  // P3-04: a terminal row replays with zero new executions, and Redis is
  // not required for the verdict (lease manager untouched by this path).
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(waiter_config(), node.service);
  const std::string key = unique_key("wl-replay");
  const std::string body = R"({"wl":4})";

  const test::HttpResult first = post_key(server.port(), key, body);
  ASSERT_EQ(first.status, 200);
  ASSERT_EQ(node.executor->executions(), 1u);

  const test::HttpResult second = post_key(server.port(), key, body);
  EXPECT_EQ(second.status, 200);
  EXPECT_EQ(second.body, first.body);
  EXPECT_EQ(node.executor->executions(), 1u) << "replay must not execute (INV-MUX-03)";
}

TEST_F(PgFixture, FailedReplayExecutesNothing) {
  // P3-05: FAILED terminal replays the stored failure with zero new
  // executions, preserving Phase 1/2 failure semantics.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(waiter_config(), node.service);
  const std::string key = unique_key("wl-failed");
  const std::string body = R"({"fail":true})";

  const test::HttpResult first = post_key(server.port(), key, body);
  ASSERT_EQ(first.status, 500);
  ASSERT_EQ(node.executor->executions(), 1u);

  const test::HttpResult second = post_key(server.port(), key, body);
  EXPECT_EQ(second.status, 409);
  EXPECT_NE(second.body.find("idempotency_already_failed"), std::string::npos);
  EXPECT_EQ(node.executor->executions(), 1u) << "failed replay must not execute";
}

TEST_F(PgFixture, ConflictingFingerprintNeverExecutes) {
  // P3-06 (INV-MUX-09): the conflicting request answers 409 and the
  // execution counter proves it never ran — only the original body did.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(waiter_config(), node.service);
  const std::string key = unique_key("wl-conflict");

  const test::HttpResult first = post_key(server.port(), key, R"({"v":1})");
  ASSERT_EQ(first.status, 200);
  ASSERT_EQ(node.executor->executions(), 1u);

  const test::HttpResult conflict = post_key(server.port(), key, R"({"v":2})");
  EXPECT_EQ(conflict.status, 409);
  EXPECT_NE(conflict.body.find("idempotency_key_in_use"), std::string::npos);
  EXPECT_EQ(node.executor->executions(), 1u) << "conflict must not execute (INV-MUX-09)";
}

TEST_F(PgFixture, MissedNotificationStillConvergesViaFallback) {
  // P3-08: the owner commits WITHOUT any broadcast reaching this waiter
  // (row completed directly at the repository level — observably identical
  // to a lost Pub/Sub message plus a missed local notify). The waiter's
  // fallback re-check finds the terminal row and converges. Correctness
  // never depended on delivery (INV-MUX-05).
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(waiter_config(), node.service);
  const std::string key = unique_key("wl-missed");
  const std::string body = R"({"wl":8})";
  const std::string fp = fp_of(body);
  const std::string channel = idempotency::WaiterRegistry::channel_for(key, fp);

  // Owner gated mid-execution (row PROCESSING, lease held).
  int owner_status = 0;
  std::thread owner([&] {
    owner_status = post_key(server.port(), key, body).status;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  // Waiter registers against live PROCESSING.
  int waiter_status = 0;
  std::string waiter_body;
  std::thread waiter([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    waiter_status = result.status;
    waiter_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node.registry->waiter_count(channel) == 1; }));

  // "Lose" every notification: commit the terminal row directly, bypassing
  // both publish and local notify. The waiter can only converge via its
  // fallback durable re-check.
  {
    auto db = raw_connect();
    ASSERT_TRUE(repo().complete(*db, key, fp, 1, 200, R"({"wl":"missed-ok"})",
                                "application/json"));
  }
  waiter.join();
  EXPECT_EQ(waiter_status, 200);
  EXPECT_EQ(waiter_body, R"({"wl":"missed-ok"})")
      << "waiter must return the DURABLE result, not a guess (INV-MUX-08)";

  // Release the abandoned owner: its late epoch-1 write is fenced (409),
  // proving the direct commit above is authoritative.
  node.executor->open_gate();
  owner.join();
  EXPECT_EQ(owner_status, 409);
  EXPECT_EQ(node.executor->executions(), 1u);
}

TEST_F(PgFixture, RedisDownProcessingFailsClosedTerminalReplays) {
  // P3-09: with Redis unreachable, PROCESSING + same fingerprint cannot take
  // a lease verdict => controlled 503 (fail closed, row untouched); a
  // COMPLETED row replays 200 without Redis (INV-MUX-10).
  REQUIRE_PG();
  const std::string processing_key = unique_key("wl-rdown-p");
  const std::string completed_key = unique_key("wl-rdown-c");
  const std::string body = R"({"wl":9})";
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, processing_key, fp_of(body), body).outcome,
              persistence::AcquireOutcome::Created);
  }
  // Seed COMPLETED through the live shared service (needs Redis).
  REQUIRE_REDIS();
  {
    test::TestServer live(waiter_config(), service());
    ASSERT_EQ(post_key(live.port(), completed_key, body).status, 200);
  }

  coordination::RedisEndpoint dead_endpoint;
  dead_endpoint.host = "127.0.0.1";
  dead_endpoint.port = test::acquire_closed_port();
  dead_endpoint.connect_timeout = 500ms;
  dead_endpoint.command_timeout = 500ms;
  dead_endpoint.pool_size = 1;
  auto dead_redis = std::make_shared<coordination::RedisClient>(std::move(dead_endpoint));
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto metrics = std::make_shared<apex::observability::Metrics>();
  auto dead_leases = std::make_shared<coordination::LeaseManager>(
      dead_redis, 10000ms, quiet, metrics);
  auto dead_service = make_service(shared_pool(), quiet, dead_leases);
  test::TestServer server(waiter_config(), dead_service);

  const test::HttpResult refused = post_key(server.port(), processing_key, body);
  EXPECT_EQ(refused.status, 503);
  EXPECT_NE(refused.body.find("redis_unavailable"), std::string::npos);
  auto db = raw_connect();
  const auto intact = repo().find_by_key(*db, processing_key);
  ASSERT_TRUE(intact.has_value());
  EXPECT_EQ(intact->status, persistence::RecordStatus::Processing);
  EXPECT_EQ(intact->fencing_epoch, 1);

  const test::HttpResult replayed = post_key(server.port(), completed_key, body);
  EXPECT_EQ(replayed.status, 200) << "replay must not need Redis (INV-MUX-10)";
}

TEST_F(PgFixture, WaiterTimeoutLeavesDurableStateUntouched) {
  // P3-13 (INV-MUX-06): a waiter that outlasts its deadline answers 202.
  // The timeout mutates NOTHING: row stays PROCESSING at epoch 1, the lease
  // stays held, no recovery is attempted, and the owner later completes
  // normally with a stable replay afterwards.
  REQUIRE_PG();
  REQUIRE_REDIS();
  idempotency::WaiterOptions fast;
  fast.timeout_ms = 600;
  fast.recheck_ms = 100;
  fast.max_waiters_per_key = 1024;
  NodeBundle node = make_node_with_options(fast, /*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(waiter_config(), node.service);
  const std::string key = unique_key("wl-timeout");
  const std::string body = R"({"wl":13})";
  const std::string channel = idempotency::WaiterRegistry::channel_for(key, fp_of(body));

  int owner_status = 0;
  std::thread owner([&] {
    owner_status = post_key(server.port(), key, body).status;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  const auto start = std::chrono::steady_clock::now();
  const test::HttpResult timed_out = post_key(server.port(), key, body);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(timed_out.status, 202);
  EXPECT_NE(timed_out.body.find("processing"), std::string::npos);
  EXPECT_GE(elapsed, 500ms) << "202 arrived before the deadline — did not actually wait";
  EXPECT_LT(elapsed, 8000ms) << "waiter hung far past its deadline";
  EXPECT_EQ(node.registry->waiter_count(channel), 0u) << "timed-out waiter leaked";

  // Durable state untouched by the timeout.
  {
    auto db = raw_connect();
    const auto record = repo().find_by_key(*db, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, persistence::RecordStatus::Processing);
    EXPECT_EQ(record->fencing_epoch, 1);
  }
  EXPECT_TRUE(leases().is_held(key)) << "timeout must not release the owner's lease";

  // The operation itself proceeds normally afterwards.
  node.executor->open_gate();
  owner.join();
  EXPECT_EQ(owner_status, 200);
  const test::HttpResult replay = post_key(server.port(), key, body);
  EXPECT_EQ(replay.status, 200);
  EXPECT_EQ(node.executor->executions(), 1u);
}

TEST_F(PgFixture, AbandonedOwnerIsRecoveredByWaiter) {
  // P3-11: the owner is abandoned mid-execution (gate never opens until the
  // end — the crash stands in for SIGKILL). The waiter does NOT fail: once
  // the lease lapses, its fallback re-check recovers (epoch 2) and completes
  // with 200. The abandoned owner's eventual write is fenced (its HTTP
  // response is 409 stale_ownership_epoch), proving Phase 2 machinery was
  // reused, not reinvented.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(waiter_config(), node.service);
  const std::string key = unique_key("wl-abandon");
  const std::string body = R"({"wl":11})";
  const std::string channel = idempotency::WaiterRegistry::channel_for(key, fp_of(body));

  int owner_status = 0;
  std::thread owner([&] {
    owner_status = post_key(server.port(), key, body).status;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  int waiter_status = 0;
  std::atomic<bool> waiter_done{false};
  std::string waiter_body;
  std::thread waiter([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    waiter_status = result.status;
    waiter_body = result.body;
    waiter_done.store(true);
  });
  ASSERT_TRUE(wait_until([&] { return node.registry->waiter_count(channel) == 1; }));

  // Owner "crashes": abandon it (gate stays shut) and lapse its lease. The
  // waiter must not fail the request — recovery takes over. Note the waiter
  // itself blocks inside its recovery execution until the gate opens below;
  // that is correct (it IS the new owner now), so the gate opens only after
  // its recovery commit is durable (executions==2 implies the epoch-2 CAS
  // committed, since the CAS precedes execute).
  redis().del(coordination::LeaseManager::lease_key_for(key));

  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 2; }, 15000ms))
      << "waiter never recovered to a new generation";
  node.executor->open_gate();
  waiter.join();
  owner.join();
  EXPECT_EQ(waiter_status, 200);
  {
    auto db = raw_connect();
    const auto record = repo().find_by_key(*db, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, persistence::RecordStatus::Completed);
    EXPECT_EQ(record->fencing_epoch, 2) << "waiter recovered as generation 2";
    EXPECT_EQ(record->response_body, waiter_body);
  }

  // The abandoned owner wakes into a fenced world: 409, result discarded.
  // (Gate already opened above; both threads joined.)
  EXPECT_EQ(owner_status, 409);
  EXPECT_EQ(node.executor->executions(), 2u) << "owner + recoverer executed once each";
  EXPECT_GE(shared_metrics()->snapshot().stale_rejections, 1u)
      << "stale owner rejection must be counted";
}

TEST_F(PgFixture, ShutdownWithSuspendedWaitersExitsCleanly) {
  // P3-18: suspended waiters (registered, timers armed, nothing in-flight on
  // the pools) must not deadlock or crash server teardown. No owner thread
  // is blocked here — the waiters suspend on a planted PROCESSING row with
  // an out-of-band held lease — so every pool join is prompt and the only
  // question is clean session/timer/subscriber teardown.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  node.subscriber->start();

  const std::string key = unique_key("wl-shutdown");
  const std::string body = R"({"wl":18})";
  const std::string channel = idempotency::WaiterRegistry::channel_for(key, fp_of(body));
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, fp_of(body), body).outcome,
              persistence::AcquireOutcome::Created);
  }
  const coordination::LeaseAttempt held = leases().try_acquire(key);
  ASSERT_EQ(held.result, coordination::LeaseAttempt::Result::Acquired);

  // Tolerant clients: server death mid-read surfaces as an exception, which
  // is an acceptable shutdown outcome (no response is possible then).
  // Every waiter thread catches it and records -1 (see loop below). Threads
  // are declared OUTSIDE the server scope so they are joined after teardown
  // (destroying a joinable thread would terminate).
  constexpr int kWaiters = 5;
  std::vector<std::thread> waiters;
  std::vector<int> statuses(kWaiters, 0);
  {
    test::TestServer server(waiter_config(), node.service);
    const std::uint16_t port = server.port();
    for (int i = 0; i < kWaiters; ++i) {
      waiters.emplace_back([&, i] {
        try {
          statuses[i] = post_key(port, key, body).status;
        } catch (const std::exception&) {
          statuses[i] = -1;  // Connection dropped by teardown: acceptable.
        }
      });
    }
    ASSERT_TRUE(wait_until([&] { return node.registry->waiter_count(channel) ==
                                              static_cast<std::size_t>(kWaiters); }))
        << "waiters never suspended";
    // Destroy the server while all 5 waiters are suspended. Must return:
    // acceptor closed, timers aborted (sessions settle-destroy), ioc and
    // pool threads joined, no deadlock, no crash.
  }
  for (std::thread& waiter : waiters) {
    waiter.join();
  }
  // All waiter registrations were released by abort paths.
  EXPECT_EQ(node.registry->waiter_count(channel), 0u) << "waiter registrations leaked";

  // Subscriber stops promptly (bounded by the socket read timeout).
  const auto stop_start = std::chrono::steady_clock::now();
  node.subscriber->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - stop_start, 15s) << "subscriber stop hung";

  // The process is fully functional afterwards on the same service.
  {
    test::TestServer server(waiter_config(), node.service);
    EXPECT_EQ(test::http_send("127.0.0.1", server.port(), boost::beast::http::verb::get,
                              "/health")
                  .status,
              200);
    EXPECT_EQ(post_key(server.port(), unique_key("wl-after"), R"({"ok":1})").status,
              200);
  }
  EXPECT_TRUE(leases().release(key, held.token));
}

}  // namespace
}  // namespace apex
