// Cross-system failure tests: Redis + PostgreSQL coordination windows.
// Each test names the crash window, the expected durable state, retry
// safety, recoverability, and the stale-owner verdict. Deterministic
// throughout: lease expiry is simulated by explicit key deletion (identical
// observable state to TTL expiry, proven real in R4) or by barrier-ordered
// repository calls — never by timing.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

using test::PgFixture;

std::string fp_of(const std::string& raw_body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", raw_body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return fp.hex;
}

config::Config cross_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 4;
  cfg.db_pool_size = 8;
  return cfg;
}

test::HttpResult post_key(std::uint16_t port, const std::string& key, const std::string& body) {
  return test::http_send_with_headers("127.0.0.1", port,
                                      boost::beast::http::verb::post, "/v1/operations", body,
                                      {{"Idempotency-Key", key},
                                       {"Content-Type", "application/json"}});
}

TEST_F(PgFixture, LeaseAcquiredButProcessDiesBeforePersisting) {
  // CASE 1: Redis lease taken, crash before the PostgreSQL row exists.
  // Expected: NO durable row (nothing to recover, nothing orphaned); the
  // lease key holds a foreign token; after expiry (simulated by delete) a new
  // owner proceeds to a full 200. Retry is safe at every step.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("case1");
  const std::string body = R"({"c":1})";

  const coordination::LeaseAttempt crashed = leases().try_acquire(key);
  ASSERT_EQ(crashed.result, coordination::LeaseAttempt::Result::Acquired);
  // ...process dies here: no PG interaction whatsoever...

  auto db = raw_connect();
  EXPECT_FALSE(repo().find_by_key(*db, key).has_value()) << "no row may exist";

  // A duplicate while the dead lease is held defers (202), creating nothing.
  {
    test::TestServer server(cross_config(), service());
    EXPECT_EQ(post_key(server.port(), key, body).status, 202);
    EXPECT_FALSE(repo().find_by_key(*db, key).has_value());
  }

  // Lease expiry (TTL in production, explicit delete here — same observable
  // state) frees the key; the next attempt owns it end to end.
  redis().del(coordination::LeaseManager::lease_key_for(key));
  {
    test::TestServer server(cross_config(), service());
    const test::HttpResult result = post_key(server.port(), key, body);
    EXPECT_EQ(result.status, 200);
  }
  EXPECT_EQ(repo().find_by_key(*db, key)->status,
            persistence::RecordStatus::Completed);
}

TEST_F(PgFixture, EpochPersistedButProcessDiesBeforeExecuting) {
  // CASE 2: fencing epoch advanced (1 -> 2), crash before the operation
  // runs. Expected: PROCESSING at epoch 2, recoverable again (liveness is
  // preserved: generations can keep advancing); a stale epoch-1 claim is
  // rejected; retry with the same key recovers and completes.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("case2");
  const std::string fp = fp_of(R"({"c":2})");
  auto db = raw_connect();
  ASSERT_EQ(repo().try_acquire(*db, key, fp, R"({"c":2})").outcome,
            persistence::AcquireOutcome::Created);
  ASSERT_EQ(repo().try_recover(*db, key, fp, 1), 2);
  // ...owner dies here, operation never ran...

  EXPECT_FALSE(repo().try_recover(*db, key, fp, 1).has_value()) << "epoch 1 is spent";
  EXPECT_EQ(repo().try_recover(*db, key, fp, 2), 3) << "ownership can advance again";

  // End to end: the same logical request recovers (now epoch 3 -> 4) and
  // completes. Retry is safe because execution never happened twice for one
  // generation — each generation executes at most once.
  test::TestServer server(cross_config(), service());
  EXPECT_EQ(post_key(server.port(), key, R"({"c":2})").status, 200);
  const auto record = repo().find_by_key(*db, key);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->status, persistence::RecordStatus::Completed);
  EXPECT_EQ(record->fencing_epoch, 4);
}

TEST_F(PgFixture, StaleOwnerCannotOverwriteNewerGeneration) {
  // THE central race (T1..T7), deterministically staged:
  //   T1  A takes the lease + inserts (epoch 1), then "keeps executing"
  //       (the test simply withholds A's terminal write).
  //   T3  A's lease expires (explicit delete; R4 proves TTL does this).
  //   T4  B takes the free lease through the real service path.
  //   T5  B recovers to epoch 2 in PostgreSQL (inside service.handle).
  //   T6  B completes; B's result is authoritative (HTTP 200).
  //   T7  A wakes and presents epoch 1: ZERO rows affected, B intact,
  //       A's own release reports token mismatch.
  // No sleeps, no timing: the interleaving is explicitly controlled.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("stale-race");
  const std::string body = R"({"race":"stale"})";
  const std::string fp = fp_of(body);
  const std::string lease_key = coordination::LeaseManager::lease_key_for(key);

  // T1: owner A, exactly as the service would establish it.
  const coordination::LeaseAttempt lease_a = leases().try_acquire(key);
  ASSERT_EQ(lease_a.result, coordination::LeaseAttempt::Result::Acquired);
  {
    auto db = raw_connect();
    const persistence::AcquireResult created =
        repo().try_acquire(*db, key, fp, R"({"race":"stale"})");
    ASSERT_EQ(created.outcome, persistence::AcquireOutcome::Created);
    ASSERT_EQ(created.record.fencing_epoch, 1);
  }

  // T3: A's lease expires. (R4 proves TTL expiry produces this state.)
  redis().del(lease_key);
  ASSERT_FALSE(leases().is_held(key));

  // T4–T6: owner B, through the full production path (HTTP + service).
  test::TestServer server(cross_config(), service());
  const test::HttpResult b_result = post_key(server.port(), key, body);
  ASSERT_EQ(b_result.status, 200) << "B must recover and complete, got: " << b_result.body;

  {
    auto db = raw_connect();
    const auto b_row = repo().find_by_key(*db, key);
    ASSERT_TRUE(b_row.has_value());
    EXPECT_EQ(b_row->status, persistence::RecordStatus::Completed);
    EXPECT_EQ(b_row->fencing_epoch, 2);
    EXPECT_EQ(b_row->response_body, b_result.body);
  }

  // T7: A wakes up and commits with its superseded epoch.
  {
    auto db = raw_connect();
    EXPECT_FALSE(repo().complete(*db, key, fp, 1, 200, R"({"evil":"overwrite"})",
                                 "application/json"))
        << "stale epoch 1 must affect zero rows";
    EXPECT_FALSE(repo().fail(*db, key, fp, 1, "evil", "overwrite"))
        << "stale fail must affect zero rows";
  }

  // B's result is authoritative and byte-identical; A's release is rejected.
  {
    auto db = raw_connect();
    const auto final_row = repo().find_by_key(*db, key);
    ASSERT_TRUE(final_row.has_value());
    EXPECT_EQ(final_row->status, persistence::RecordStatus::Completed);
    EXPECT_EQ(final_row->fencing_epoch, 2);
    EXPECT_EQ(final_row->response_body, b_result.body);
  }
  EXPECT_FALSE(leases().release(key, lease_a.token)) << "A's token is spent";

  // Late duplicates converge on B's result, never A's phantom.
  const test::HttpResult late = post_key(server.port(), key, body);
  EXPECT_EQ(late.status, 200);
  EXPECT_EQ(late.body, b_result.body);
}

TEST_F(PgFixture, RedisDownDuringOwnershipAttemptFailsClosed) {
  // CASE 5: coordination unreachable mid-flow. New ownership answers 503
  // redis_unavailable AND creates no durable row (fail-closed leaves no
  // orphan); pre-existing COMPLETED rows still replay 200 without Redis.
  REQUIRE_PG();
  const std::string fresh_key = unique_key("case5-fresh");
  const std::string replay_key = unique_key("case5-replay");

  // Seed a COMPLETED row through the live service first.
  {
    test::TestServer live(cross_config(), service());
    REQUIRE_REDIS();
    const test::HttpResult seed = post_key(live.port(), replay_key, R"({"s":1})");
    ASSERT_EQ(seed.status, 200);
  }

  // Service wired to a dead Redis (live PostgreSQL).
  coordination::RedisEndpoint dead_endpoint;
  dead_endpoint.host = "127.0.0.1";
  dead_endpoint.port = test::acquire_closed_port();
  dead_endpoint.connect_timeout = std::chrono::milliseconds(500);
  dead_endpoint.command_timeout = std::chrono::milliseconds(500);
  dead_endpoint.pool_size = 1;
  auto dead_redis =
      std::make_shared<coordination::RedisClient>(std::move(dead_endpoint));
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto metrics = std::make_shared<apex::observability::Metrics>();
  auto dead_leases = std::make_shared<coordination::LeaseManager>(
      dead_redis, std::chrono::milliseconds(10000), quiet, metrics);
  auto dead_service = make_service(shared_pool(), quiet, dead_leases);

  test::TestServer server(cross_config(), dead_service);

  // Replay needs no lease: 200 even with Redis down.
  const test::HttpResult replayed = post_key(server.port(), replay_key, R"({"s":1})");
  EXPECT_EQ(replayed.status, 200);
  EXPECT_NE(replayed.body.find("\"result\":\"ok\""), std::string::npos);

  // New ownership needs a lease: fail closed, no row.
  const test::HttpResult refused = post_key(server.port(), fresh_key, R"({"s":2})");
  EXPECT_EQ(refused.status, 503);
  EXPECT_NE(refused.body.find("redis_unavailable"), std::string::npos);

  auto db = raw_connect();
  EXPECT_FALSE(repo().find_by_key(*db, fresh_key).has_value())
      << "fail-closed must not orphan a row";
}

TEST_F(PgFixture, PostgresFailureDuringOwnershipFailsSafe) {
  // CASE 6, deterministically staged at two levels:
  // (a) Service level with PG fully down: 503 storage_unavailable BEFORE any
  //     lease side-effect (checkout precedes coordination), so no lease key
  //     is created for the refused key.
  // (b) Repository level with the backend killed mid-transaction: the op
  //     throws PgError, the aborted transaction commits nothing, the epoch
  //     is untouched, and a fresh connection recovers normally. This is the
  //     exact "PG dies between lease-acquire and repo-op" window: no partial
  //     terminal write can exist (single-statement atomicity), retry is safe,
  //     and the epoch CAS remains the sole decider.
  REQUIRE_PG();
  REQUIRE_REDIS();

  // (a) PG down: fail before any lease side-effect.
  {
    auto dead_pool = std::make_shared<persistence::ConnectionPool>(
        "host=127.0.0.1 port=" + std::to_string(test::acquire_closed_port()) +
            " dbname=apex user=apex connect_timeout=2 application_name=apex-test",
        /*max_size=*/2);
    apex::observability::Logger quiet(apex::observability::Level::Error);
    auto dead_pg_service = make_service(dead_pool, quiet, shared_leases());
    test::TestServer server(cross_config(), dead_pg_service);
    const std::string key = unique_key("case6-down");
    const test::HttpResult refused = post_key(server.port(), key, R"({"s":6})");
    EXPECT_EQ(refused.status, 503);
    EXPECT_NE(refused.body.find("storage_unavailable"), std::string::npos);
    EXPECT_FALSE(
        redis().get(coordination::LeaseManager::lease_key_for(key)).has_value())
        << "refused request must not take a lease";
  }

  // (b) Backend killed mid-transaction: abort commits nothing.
  {
    const std::string key = unique_key("case6-killed");
    const std::string fp = fp_of(R"({"s":6})");
    auto victim = raw_connect();
    ASSERT_EQ(repo().try_acquire(*victim, key, fp, R"({"s":6})").outcome,
              persistence::AcquireOutcome::Created);

    // Another backend terminates the victim's connection mid-transaction.
    const persistence::PgResult pid = victim->exec("SELECT pg_backend_pid()");
    const std::string pid_text = pid.value(0, 0);
    (void)victim->exec("BEGIN");
    {
      auto killer = raw_connect();
      const persistence::PgResult killed = killer->exec_params(
          "SELECT pg_terminate_backend($1)", {pid_text});
      EXPECT_EQ(killed.value(0, 0), "t") << "backend kill must succeed";
    }
    // EXPECT_THROW cannot take the nodiscard call directly (macro argument
    // splitting), so the throwing statement is wrapped in a void lambda.
    const auto recover_on_dead_connection = [&] {
      (void)repo().try_recover(*victim, key, fp, 1);
    };
    EXPECT_THROW(recover_on_dead_connection(), persistence::PgError);

    // Aborted transaction left nothing: epoch untouched, still PROCESSING.
    auto fresh = raw_connect();
    const auto record = repo().find_by_key(*fresh, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, persistence::RecordStatus::Processing);
    EXPECT_EQ(record->fencing_epoch, 1);

    // A fresh connection recovers normally: liveness preserved.
    EXPECT_EQ(repo().try_recover(*fresh, key, fp, 1), 2);
  }
}

}  // namespace
}  // namespace apex
