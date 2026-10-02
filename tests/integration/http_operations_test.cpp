// POST /v1/operations end-to-end against real PostgreSQL + real Redis
// (gated). Covers the documented HTTP semantics: missing/invalid key, first
// request, active-owner PROCESSING duplicate (202), orphan recovery to a new
// epoch (200), COMPLETED replay, fingerprint conflict, FAILED terminal,
// storage/coordination-unavailable, and fail-closed behavior without leases.

#include <string>

#include <boost/beast/http/verb.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/pg_fixture.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/LeaseManager.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "observability/Logger.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

namespace http = boost::beast::http;
using test::PgFixture;

// Fingerprint hex for a body under the operations route, computed through
// the same production code the server uses.
std::string fingerprint_of(const std::string& raw_body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", raw_body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return fp.hex;
}

config::Config ops_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 4;
  cfg.db_pool_size = 8;
  cfg.postgres_host = "127.0.0.1";
  cfg.postgres_port = 5432;
  return cfg;
}

test::HttpResult post_ops(std::uint16_t port, const std::string& key, const std::string& body) {
  // The header is ALWAYS sent, even when `key` is empty: an empty value must
  // reach the validator (400 invalid/empty), while a missing header is a
  // different error (400 missing). Only the MissingKey test omits it.
  const test::HttpHeaders headers{{"Idempotency-Key", key}, {"Content-Type", "application/json"}};
  return test::http_send_with_headers("127.0.0.1", port, http::verb::post, "/v1/operations",
                                      body, headers);
}

TEST_F(PgFixture, MissingKeyIs400) {
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const test::HttpResult result = test::http_send("127.0.0.1", server.port(), http::verb::post,
                                                  "/v1/operations", R"({"a":1})");
  EXPECT_EQ(result.status, 400);
  EXPECT_NE(result.body.find("missing_idempotency_key"), std::string::npos);
}

TEST_F(PgFixture, InvalidKeysAre400WithReasons) {
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());

  const test::HttpResult empty = post_ops(server.port(), "", R"({"a":1})");
  // Empty header value: Beast drops nothing, so the validator sees "".
  EXPECT_EQ(empty.status, 400);
  EXPECT_NE(empty.body.find("invalid_idempotency_key"), std::string::npos);

  const test::HttpResult bad = post_ops(server.port(), "not a key!", R"({"a":1})");
  EXPECT_EQ(bad.status, 400);
  EXPECT_NE(bad.body.find("invalid_characters"), std::string::npos);

  const test::HttpResult huge = post_ops(server.port(), std::string(300, 'k'), R"({"a":1})");
  EXPECT_EQ(huge.status, 400);
  EXPECT_NE(huge.body.find("too_long"), std::string::npos);
}

TEST_F(PgFixture, InvalidJsonBodyIs400) {
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const test::HttpResult result =
      post_ops(server.port(), unique_key("badjson"), "this is not json");
  EXPECT_EQ(result.status, 400);
  EXPECT_NE(result.body.find("invalid_json_body"), std::string::npos);
}

TEST_F(PgFixture, WrongMethodIs405) {
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const test::HttpResult result =
      test::http_send("127.0.0.1", server.port(), http::verb::get, "/v1/operations");
  EXPECT_EQ(result.status, 405);
}

TEST_F(PgFixture, FirstRequestExecutesAndCompletes) {
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const std::string key = unique_key("first");

  const test::HttpResult result = post_ops(server.port(), key, R"({"b":2,"a":1})");
  EXPECT_EQ(result.status, 200);
  const nlohmann::json body = nlohmann::json::parse(result.body);
  EXPECT_EQ(body.at("result").get<std::string>(), "ok");
  // Echo carries the CANONICAL body (sorted keys), proving canonicalization
  // survived the round trip through durable storage.
  EXPECT_EQ(body.at("request"), nlohmann::json::parse(R"({"a":1,"b":2})"));
}

TEST_F(PgFixture, CompletedDuplicateReplaysStoredResult) {
  // TEST D (HTTP level): byte-identical replay without re-execution. The
  // completed_at timestamp is captured from the database before and after:
  // unchanged means complete() never ran a second time.
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const std::string key = unique_key("replay");

  const test::HttpResult first = post_ops(server.port(), key, R"({"x":1})");
  EXPECT_EQ(first.status, 200);

  auto db = raw_connect();
  const persistence::PgResult stamped = db->exec_params(
      "SELECT completed_at FROM idempotency_records WHERE idempotency_key = $1", {key});
  ASSERT_EQ(stamped.rows(), 1);
  const std::string completed_at = stamped.value(0, 0);

  const test::HttpResult second = post_ops(server.port(), key, R"({"x":1})");
  EXPECT_EQ(second.status, 200);
  EXPECT_EQ(second.body, first.body) << "replay must be byte-identical";

  const persistence::PgResult restamped = db->exec_params(
      "SELECT completed_at FROM idempotency_records WHERE idempotency_key = $1", {key});
  EXPECT_EQ(restamped.value(0, 0), completed_at) << "terminal write ran twice";
}

TEST_F(PgFixture, DuplicateWhileProcessingGets202) {
  // CASE B: a PROCESSING row whose owner still holds the Redis lease is an
  // ACTIVE operation. The duplicate gets 202 without waiting (multiplexing
  // is a later phase) and, crucially, no recovery is attempted: the epoch
  // stays 1 and nothing executes.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("processing");
  {
    auto db = raw_connect();
    const persistence::AcquireResult acquired =
        repo().try_acquire(*db, key, fingerprint_of(R"({"p":1})"));
    ASSERT_EQ(acquired.outcome, persistence::AcquireOutcome::Created);
    ASSERT_EQ(acquired.record.fencing_epoch, 1);
  }
  // Simulate the still-active owner by holding its lease out-of-band.
  const coordination::LeaseAttempt owner = leases().try_acquire(key);
  ASSERT_EQ(owner.result, coordination::LeaseAttempt::Result::Acquired);

  test::TestServer server(ops_config(), service());
  const test::HttpResult result = post_ops(server.port(), key, R"({"p":1})");
  EXPECT_EQ(result.status, 202);
  EXPECT_NE(result.body.find("processing"), std::string::npos);

  auto db = raw_connect();
  const auto record = repo().find_by_key(*db, key);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->fencing_epoch, 1) << "active owner must not be recovered";
  EXPECT_EQ(record->status, persistence::RecordStatus::Processing);
}

TEST_F(PgFixture, OrphanedProcessingRecoversToANewEpoch) {
  // CASE C over HTTP: PROCESSING row, no lease held (owner gone). The
  // duplicate becomes the recovery owner: epoch 1 -> 2, executes, 200.
  // The replay afterwards is byte-identical.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("orphan");
  {
    auto db = raw_connect();
    const persistence::AcquireResult acquired =
        repo().try_acquire(*db, key, fingerprint_of(R"({"o":1})"));
    ASSERT_EQ(acquired.outcome, persistence::AcquireOutcome::Created);
  }
  ASSERT_FALSE(leases().is_held(key)) << "no owner may hold this lease";

  test::TestServer server(ops_config(), service());
  const test::HttpResult result = post_ops(server.port(), key, R"({"o":1})");
  EXPECT_EQ(result.status, 200);

  auto db = raw_connect();
  const auto record = repo().find_by_key(*db, key);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->status, persistence::RecordStatus::Completed);
  EXPECT_EQ(record->fencing_epoch, 2);

  const test::HttpResult replay = post_ops(server.port(), key, R"({"o":1})");
  EXPECT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body, result.body);
}

TEST_F(PgFixture, NullLeasesFailClosedWithoutCreatingOrphans) {
  // A service with no lease manager cannot take ownership: first requests
  // fail closed (503 redis_unavailable) and, critically, NO durable row is
  // created — fail-closed must not orphan PROCESSING rows.
  REQUIRE_PG();
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto leaseless = std::make_shared<idempotency::IdempotencyService>(
      shared_pool(), quiet, /*leases=*/nullptr);
  test::TestServer server(ops_config(), leaseless);

  const std::string key = unique_key("nolease");
  const test::HttpResult result = post_ops(server.port(), key, R"({"a":1})");
  EXPECT_EQ(result.status, 503);
  EXPECT_NE(result.body.find("redis_unavailable"), std::string::npos);

  auto db = raw_connect();
  EXPECT_FALSE(repo().find_by_key(*db, key).has_value()) << "fail-closed left an orphan";
}

TEST_F(PgFixture, SameKeyDifferentFingerprintIs409Conflict) {
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const std::string key = unique_key("conflict");

  const test::HttpResult first = post_ops(server.port(), key, R"({"v":1})");
  EXPECT_EQ(first.status, 200);

  const test::HttpResult second = post_ops(server.port(), key, R"({"v":2})");
  EXPECT_EQ(second.status, 409);
  EXPECT_NE(second.body.find("idempotency_key_in_use"), std::string::npos);

  // The original record is untouched by the conflicting attempt.
  auto db = raw_connect();
  const std::optional<persistence::IdempotencyRecord> record = repo().find_by_key(*db, key);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->status, persistence::RecordStatus::Completed);
  const test::HttpResult replay = post_ops(server.port(), key, R"({"v":1})");
  EXPECT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body, first.body);
}

TEST_F(PgFixture, FailedTerminalPolicyIs409WithOriginalFailure) {
  // TEST E: the simulated failure lands the record in FAILED; retrying the
  // same logical request returns 409 with the original failure attached and
  // never restarts the operation.
  REQUIRE_PG();
  test::TestServer server(ops_config(), service());
  const std::string key = unique_key("failed");

  const test::HttpResult first = post_ops(server.port(), key, R"({"fail":true})");
  EXPECT_EQ(first.status, 500);
  EXPECT_NE(first.body.find("simulated_failure"), std::string::npos);

  const test::HttpResult second = post_ops(server.port(), key, R"({"fail":true})");
  EXPECT_EQ(second.status, 409);
  EXPECT_NE(second.body.find("idempotency_already_failed"), std::string::npos);
  EXPECT_NE(second.body.find("simulated_failure"), std::string::npos);

  // ...and a different fingerprint under the same key is still a conflict,
  // not a fresh start (terminal means terminal).
  const test::HttpResult third = post_ops(server.port(), key, R"({"fail":false})");
  EXPECT_EQ(third.status, 409);
}

TEST_F(PgFixture, UnreachableDatabaseIs503) {
  // A service wired to a dead database maps every failure to a controlled
  // 503 — the gateway stays up and keeps serving /health.
  config::Config cfg = ops_config();
  auto dead_pool = std::make_shared<persistence::ConnectionPool>(
      "host=127.0.0.1 port=" + std::to_string(test::acquire_closed_port()) +
          " dbname=apex user=apex connect_timeout=2 application_name=apex-test",
      /*max_size=*/2);
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto dead_service =
      std::make_shared<idempotency::IdempotencyService>(dead_pool, quiet);
  // NOTE: dead_pool must outlive the server (service holds it too).
  test::TestServer server(cfg, dead_service);

  const test::HttpResult result =
      post_ops(server.port(), unique_key("down"), R"({"a":1})");
  EXPECT_EQ(result.status, 503);
  EXPECT_NE(result.body.find("storage_unavailable"), std::string::npos);

  EXPECT_EQ(test::http_send("127.0.0.1", server.port(), http::verb::get, "/health").status,
            200);
}

}  // namespace
}  // namespace apex
