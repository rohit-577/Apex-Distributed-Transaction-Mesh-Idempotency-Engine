// Concurrency tests for durable idempotency (Phase 1), against REAL
// PostgreSQL + a REAL gateway. No fakes, no sleeps: simultaneous starts use
// a std::latch barrier so races are reproduced by coordination, not timing.
//
// TEST A: 2 concurrent same-key/same-fingerprint requests.
// TEST B: 50 concurrent same-key requests.
// TEST C: same key, different fingerprints, concurrently.

#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <boost/beast/http/verb.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/pg_fixture.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

namespace http = boost::beast::http;
using test::PgFixture;

config::Config race_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 8;
  cfg.db_pool_size = 16;
  return cfg;
}

test::HttpResult post_key(std::uint16_t port, const std::string& key, const std::string& body) {
  return test::http_send_with_headers("127.0.0.1", port, http::verb::post, "/v1/operations",
                                      body,
                                      {{"Idempotency-Key", key},
                                       {"Content-Type", "application/json"}});
}

int count_rows_for(const std::string& key) {
  // Fresh connection per call: never shared across the racing threads.
  persistence::PgConnection db(*test::pg_test_conninfo());
  const persistence::PgResult count = db.exec_params(
      "SELECT count(*) FROM idempotency_records WHERE idempotency_key = $1", {key});
  return std::stoi(count.value(0, 0));
}

TEST_F(PgFixture, TwoConcurrentSameKeyRequestsResolveToOneRecord) {
  // TEST A: deterministic winner/loser decided by the PRIMARY KEY, not by
  // application timing. Both responses are valid terminal answers (200
  // executed/replayed, or 202 seen-PROCESSING); the record count is exactly
  // one; a follow-up replay is byte-identical to the first 200.
  REQUIRE_PG();
  test::TestServer server(race_config(), service());
  const std::string key = unique_key("raceA");
  const std::string body = R"({"race":"a"})";

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(2, 0);
  std::vector<std::string> bodies(2);
  for (int i = 0; i < 2; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      const test::HttpResult result = post_key(server.port(), key, body);
      statuses[i] = result.status;
      bodies[i] = result.body;
    });
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  for (int status : statuses) {
    EXPECT_TRUE(status == 200 || status == 202) << "unexpected status " << status;
  }
  EXPECT_EQ(count_rows_for(key), 1) << "INV-12 violated";

  const test::HttpResult replay = post_key(server.port(), key, body);
  EXPECT_EQ(replay.status, 200);
  const std::string executed_body =
      (statuses[0] == 200) ? bodies[0] : (statuses[1] == 200 ? bodies[1] : "");
  if (!executed_body.empty()) {
    EXPECT_EQ(replay.body, executed_body);
  }
  // And the replay is a well-formed operation result in any case.
  EXPECT_NE(replay.body.find("\"result\":\"ok\""), std::string::npos);
}

TEST_F(PgFixture, FiftyConcurrentSameKeyRequestsStayConsistent) {
  // TEST B: 50-way fan-in. Every contender observes a consistent record, no
  // uniqueness violation escapes as an uncontrolled error (no 409/500/503),
  // exactly one durable row exists afterwards.
  REQUIRE_PG();
  constexpr int kContenders = 50;
  test::TestServer server(race_config(), service());
  const std::string key = unique_key("raceB");
  const std::string body = R"({"race":"b","n":50})";

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(kContenders, 0);
  for (int i = 0; i < kContenders; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      statuses[i] = post_key(server.port(), key, body).status;
    });
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  int executed = 0;
  int in_progress = 0;
  for (int status : statuses) {
    EXPECT_TRUE(status == 200 || status == 202) << "status " << status << " escaped";
    executed += (status == 200) ? 1 : 0;
    in_progress += (status == 202) ? 1 : 0;
  }
  EXPECT_GE(executed, 1) << "at least one contender must execute-or-replay to 200";
  EXPECT_EQ(count_rows_for(key), 1) << "INV-12 violated under 50-way fan-in";

  const test::HttpResult replay = post_key(server.port(), key, body);
  EXPECT_EQ(replay.status, 200);
  EXPECT_NE(replay.body.find("\"result\":\"ok\""), std::string::npos);
}

TEST_F(PgFixture, SameKeyDifferentFingerprintsConflictCleanly) {
  // TEST C: two different logical operations race under one key. Exactly one
  // becomes the record; the other is rejected with 409 — never executed as
  // the winner, never creating a second row.
  REQUIRE_PG();
  test::TestServer server(race_config(), service());
  const std::string key = unique_key("raceC");
  const std::string body_a = R"({"side":"a"})";
  const std::string body_b = R"({"side":"b"})";

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(2, 0);
  for (int i = 0; i < 2; ++i) {
    const std::string body = (i == 0) ? body_a : body_b;
    threads.emplace_back([&, i, body] {
      go.wait();
      statuses[i] = post_key(server.port(), key, body).status;
    });
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  // Exactly one side inserted (200 after executing); the other found the
  // row with a different fingerprint (409). A 202 is impossible here: it
  // requires same-fingerprint + PROCESSING, and the fingerprints differ.
  EXPECT_TRUE((statuses[0] == 200 && statuses[1] == 409) ||
              (statuses[0] == 409 && statuses[1] == 200))
      << statuses[0] << " vs " << statuses[1];
  const bool first_won = (statuses[0] == 200);

  EXPECT_EQ(count_rows_for(key), 1);

  // The winner's logical operation replays; the loser's never runs.
  const std::string winner_body = first_won ? body_a : body_b;
  const std::string loser_body = first_won ? body_b : body_a;
  const test::HttpResult replay = post_key(server.port(), key, winner_body);
  EXPECT_EQ(replay.status, 200);
  const test::HttpResult conflict_again = post_key(server.port(), key, loser_body);
  EXPECT_EQ(conflict_again.status, 409);
  EXPECT_NE(conflict_again.body.find("idempotency_key_in_use"), std::string::npos);
}

}  // namespace
}  // namespace apex
