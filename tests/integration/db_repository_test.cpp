// Repository tests against a REAL PostgreSQL (gated by
// APEX_TEST_POSTGRES_CONN). No fakes: atomicity, races, rollbacks, and
// terminal-state guards are only meaningful against the actual database.

#include <atomic>
#include <latch>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_helpers.hpp"
#include "idempotency/Fingerprint.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex::persistence {
namespace {

using test::PgFixture;

const std::string kFpA =
    idempotency::sha256_hex("POST\n/v1/operations\n" + std::string(R"({"a":1})"));
const std::string kFpB =
    idempotency::sha256_hex("POST\n/v1/operations\n" + std::string(R"({"a":2})"));

TEST_F(PgFixture, AcquireCreatesProcessingAndFindRoundTrips) {
  REQUIRE_PG();
  const std::string key = unique_key("acquire");
  auto db = raw_connect();

  const AcquireResult created = repo().try_acquire(*db, key, kFpA, R"({})");
  EXPECT_EQ(created.outcome, AcquireOutcome::Created);
  EXPECT_EQ(created.record.status, RecordStatus::Processing);

  const std::optional<IdempotencyRecord> found = repo().find_by_key(*db, key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->key, key);
  EXPECT_EQ(found->fingerprint, kFpA);
  EXPECT_EQ(found->status, RecordStatus::Processing);
  EXPECT_FALSE(found->http_status.has_value());
}

TEST_F(PgFixture, SecondAcquireFindsTheSameRow) {
  REQUIRE_PG();
  const std::string key = unique_key("reacquire");
  auto db = raw_connect();

  EXPECT_EQ(repo().try_acquire(*db, key, kFpA, R"({})").outcome, AcquireOutcome::Created);
  const AcquireResult second = repo().try_acquire(*db, key, kFpA, R"({})");
  EXPECT_EQ(second.outcome, AcquireOutcome::Found);
  EXPECT_EQ(second.record.key, key);
  EXPECT_EQ(second.record.status, RecordStatus::Processing);
}

TEST_F(PgFixture, FindMissingKeyReturnsNullopt) {
  REQUIRE_PG();
  auto db = raw_connect();
  EXPECT_FALSE(repo().find_by_key(*db, unique_key("missing")).has_value());
}

TEST_F(PgFixture, CompleteStoresReplayableResponse) {
  REQUIRE_PG();
  const std::string key = unique_key("complete");
  auto db = raw_connect();
  EXPECT_EQ(repo().try_acquire(*db, key, kFpA, R"({})").outcome, AcquireOutcome::Created);

  EXPECT_TRUE(repo().complete(*db, key, kFpA, 1, 200, R"({"result":"ok"})", "application/json"));

  const std::optional<IdempotencyRecord> found = repo().find_by_key(*db, key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->status, RecordStatus::Completed);
  ASSERT_TRUE(found->http_status.has_value());
  EXPECT_EQ(*found->http_status, 200);
  EXPECT_EQ(found->response_body, R"({"result":"ok"})");
  EXPECT_EQ(found->response_content_type, "application/json");
}

TEST_F(PgFixture, TerminalWritesAreIdempotentGuards) {
  // TEST F: complete/fail affect exactly one row on PROCESSING and zero rows
  // afterwards. Terminal state stays intact no matter how often the
  // transition is attempted.
  REQUIRE_PG();
  const std::string key = unique_key("terminal");
  auto db = raw_connect();
  EXPECT_EQ(repo().try_acquire(*db, key, kFpA, R"({})").outcome, AcquireOutcome::Created);
  ASSERT_TRUE(repo().complete(*db, key, kFpA, 1, 200, "first", "application/json"));

  EXPECT_FALSE(repo().complete(*db, key, kFpA, 1, 200, "second", "application/json"));
  EXPECT_FALSE(repo().fail(*db, key, kFpA, 1, "late", "too late"));

  const std::optional<IdempotencyRecord> found = repo().find_by_key(*db, key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->status, RecordStatus::Completed);
  EXPECT_EQ(found->response_body, "first") << "terminal row must be byte-identical";
}

TEST_F(PgFixture, FailTransitionAndFailedIsTerminal) {
  REQUIRE_PG();
  const std::string key = unique_key("fail");
  auto db = raw_connect();
  EXPECT_EQ(repo().try_acquire(*db, key, kFpA, R"({})").outcome, AcquireOutcome::Created);

  EXPECT_TRUE(repo().fail(*db, key, kFpA, 1, "simulated_failure", "asked to fail"));
  EXPECT_FALSE(repo().fail(*db, key, kFpA, 1, "again", "again"));
  EXPECT_FALSE(repo().complete(*db, key, kFpA, 1, 200, "late", "application/json"));

  const std::optional<IdempotencyRecord> found = repo().find_by_key(*db, key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->status, RecordStatus::Failed);
  EXPECT_EQ(found->error_code, "simulated_failure");
}

TEST_F(PgFixture, TerminalWriteWithWrongFingerprintAffectsZeroRows) {
  // Defense in depth: even if a caller mixed up fingerprints, the guarded
  // UPDATE cannot move someone else's logical operation to terminal.
  REQUIRE_PG();
  const std::string key = unique_key("fpguard");
  auto db = raw_connect();
  EXPECT_EQ(repo().try_acquire(*db, key, kFpA, R"({})").outcome, AcquireOutcome::Created);

  EXPECT_FALSE(repo().complete(*db, key, kFpB, 1, 200, "wrong", "application/json"));
  EXPECT_EQ(repo().find_by_key(*db, key)->status, RecordStatus::Processing);
}

TEST_F(PgFixture, RollbackLeavesNoRowBehind) {
  REQUIRE_PG();
  const std::string key = unique_key("rollback");
  auto db = raw_connect();

  (void)db->exec("BEGIN");
  (void)db->exec_params(
      "INSERT INTO idempotency_records (idempotency_key, fingerprint, status)"
      " VALUES ($1, $2, 'PROCESSING')",
      {key, kFpA});
  // Visible inside the transaction...
  EXPECT_TRUE(repo().find_by_key(*db, key).has_value());
  (void)db->exec("ROLLBACK");

  // ...gone after rollback. A crashed creator (never committed) leaves the
  // same absence: there is no half-created record to recover.
  EXPECT_FALSE(repo().find_by_key(*db, key).has_value());
}

TEST_F(PgFixture, ConcurrentRawInsertsResolveToExactlyOneRow) {
  // The database — not application code — decides the race. Two threads fire
  // the same INSERT at the same instant (latch barrier, no sleeps); exactly
  // one wins and the loser gets SQLSTATE 23505 (unique_violation).
  REQUIRE_PG();
  const std::string key = unique_key("rawrace");
  std::latch go{1};
  std::atomic<int> successes{0};
  std::atomic<int> unique_violations{0};

  auto contender = [&] {
    try {
      auto db = raw_connect();
      go.wait();
      (void)db->exec_params(
          "INSERT INTO idempotency_records (idempotency_key, fingerprint, status)"
          " VALUES ($1, $2, 'PROCESSING')",
          {key, kFpA});
      ++successes;
    } catch (const PgError& e) {
      if (e.sqlstate() == "23505") {
        ++unique_violations;
      } else {
        ADD_FAILURE() << "unexpected error: " << e.what();
      }
    }
  };
  std::thread first(contender);
  std::thread second(contender);
  go.count_down();
  first.join();
  second.join();

  EXPECT_EQ(successes.load(), 1);
  EXPECT_EQ(unique_violations.load(), 1);

  auto db = raw_connect();
  const PgResult count = db->exec_params(
      "SELECT count(*) FROM idempotency_records WHERE idempotency_key = $1", {key});
  EXPECT_EQ(count.value(0, 0), "1") << "INV-12: exactly one durable record";
}

TEST_F(PgFixture, SchemaCheckConstraintsRejectBadStates) {
  // The CHECK constraints are a second line of defense behind the
  // repository: malformed rows cannot be smuggled in via raw SQL either.
  REQUIRE_PG();
  const std::string key = unique_key("check");
  auto db = raw_connect();

  try {
    (void)db->exec_params(
        "INSERT INTO idempotency_records (idempotency_key, fingerprint, status)"
        " VALUES ($1, $2, 'BOGUS')",
        {key, kFpA});
    FAIL() << "invalid status was accepted";
  } catch (const PgError& e) {
    EXPECT_EQ(e.sqlstate(), "23514") << "expected check_violation, got: " << e.what();
  }

  try {
    (void)db->exec_params(
        "INSERT INTO idempotency_records (idempotency_key, fingerprint, status,"
        " http_status, response_body, completed_at)"
        " VALUES ($1, $2, 'PROCESSING', 200, '{}', now())",
        {key, kFpA});
    FAIL() << "PROCESSING row with a result payload was accepted";
  } catch (const PgError& e) {
    EXPECT_EQ(e.sqlstate(), "23514") << "expected check_violation, got: " << e.what();
  }
  EXPECT_FALSE(repo().find_by_key(*db, key).has_value());
}

TEST_F(PgFixture, UnreachableDatabaseThrowsPgError) {
  // Connection failure is a controlled PgError (mapped to 503 upstream),
  // never a hang: the test conninfo carries a short connect_timeout.
  const std::string bad =
      "host=127.0.0.1 port=" + std::to_string(test::acquire_closed_port()) +
      " dbname=apex user=apex connect_timeout=2 application_name=apex-test";
  EXPECT_THROW(
      {
        try {
          PgConnection doomed(bad);
        } catch (const PgError& e) {
          EXPECT_NE(std::string(e.what()).find("PostgreSQL connection failed"),
                    std::string::npos);
          throw;
        }
      },
      PgError);
}

TEST_F(PgFixture, PoolRecyclesConnectionsAcrossCheckouts) {
  REQUIRE_PG();
  const std::string key = unique_key("pool");
  {
    persistence::ConnectionPool::Guard first = pool().acquire();
    EXPECT_EQ(repo().try_acquire(first.connection(), key, kFpA, R"({})").outcome,
              AcquireOutcome::Created);
  }  // Guard returns the connection here.
  {
    persistence::ConnectionPool::Guard second = pool().acquire();
    EXPECT_TRUE(
        repo().complete(second.connection(), key, kFpA, 1, 200, "{}", "application/json"));
  }
  auto db = raw_connect();
  EXPECT_EQ(repo().find_by_key(*db, key)->status, RecordStatus::Completed);
}

}  // namespace
}  // namespace apex::persistence
