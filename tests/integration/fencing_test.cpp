// Fencing-epoch tests against REAL PostgreSQL (gated). F1 first epoch, F2
// recovery epoch, F3 monotonicity, F4 current-owner commit, F5 stale commit
// rejected, F6/F7 stale writes on terminal states, F8 concurrent recovery
// (one winner), F9 racing terminal writes (current generation wins).

#include <atomic>
#include <cstdint>
#include <latch>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "idempotency/Fingerprint.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex::persistence {
namespace {

using test::PgFixture;

const std::string kFp =
    idempotency::sha256_hex("POST\n/v1/operations\n" + std::string(R"({"f":1})"));
const std::string kFpOther =
    idempotency::sha256_hex("POST\n/v1/operations\n" + std::string(R"({"f":2})"));

TEST_F(PgFixture, FirstOwnerReceivesEpochOne) {
  // F1: the creating generation is epoch 1, durably.
  REQUIRE_PG();
  const std::string key = unique_key("epoch1");
  auto db = raw_connect();

  const AcquireResult created = repo().try_acquire(*db, key, kFp);
  ASSERT_EQ(created.outcome, AcquireOutcome::Created);
  EXPECT_EQ(created.record.fencing_epoch, 1);

  const auto stored = repo().find_by_key(*db, key);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->fencing_epoch, 1);
}

TEST_F(PgFixture, RecoveryAdvancesExactlyOneGeneration) {
  // F2/F3: recovery assigns previous + 1, monotonically, durably.
  REQUIRE_PG();
  const std::string key = unique_key("epoch2");
  auto db = raw_connect();
  ASSERT_EQ(repo().try_acquire(*db, key, kFp).outcome, AcquireOutcome::Created);

  const std::optional<std::int64_t> second = repo().try_recover(*db, key, kFp, 1);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 2);

  const std::optional<std::int64_t> third = repo().try_recover(*db, key, kFp, 2);
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(*third, 3);

  EXPECT_EQ(repo().find_by_key(*db, key)->fencing_epoch, 3);
  EXPECT_EQ(repo().find_by_key(*db, key)->status, RecordStatus::Processing);
}

TEST_F(PgFixture, RecoveryRefusesWrongEpochAndWrongState) {
  // try_recover is a compare-and-swap on the observed epoch: anything but
  // the current (PROCESSING, same fingerprint, exact epoch) yields nullopt
  // instead of advancing.
  REQUIRE_PG();
  const std::string key = unique_key("epochrefuse");
  auto db = raw_connect();
  ASSERT_EQ(repo().try_acquire(*db, key, kFp).outcome, AcquireOutcome::Created);

  EXPECT_FALSE(repo().try_recover(*db, key, kFp, 0).has_value()) << "epoch 0 never exists";
  EXPECT_FALSE(repo().try_recover(*db, key, kFp, 2).has_value()) << "epoch 2 not yet assigned";
  EXPECT_FALSE(repo().try_recover(*db, key, kFpOther, 1).has_value())
      << "recovery must not adopt a foreign fingerprint";
  EXPECT_FALSE(repo().try_recover(*db, "no-such-key", kFp, 1).has_value());

  // Terminal rows are not recoverable at any epoch.
  ASSERT_TRUE(repo().complete(*db, key, kFp, 1, 200, "{}", "application/json"));
  EXPECT_FALSE(repo().try_recover(*db, key, kFp, 1).has_value());
}

TEST_F(PgFixture, CurrentEpochCanCommitStaleEpochCannot) {
  // F4/F5: the FENCING INVARIANT at the repository level. Presenting the
  // current epoch transitions; presenting a superseded one affects zero
  // rows and changes nothing.
  REQUIRE_PG();
  const std::string key = unique_key("epochcommit");
  auto db = raw_connect();
  ASSERT_EQ(repo().try_acquire(*db, key, kFp).outcome, AcquireOutcome::Created);
  ASSERT_EQ(repo().try_recover(*db, key, kFp, 1), 2);

  EXPECT_FALSE(repo().complete(*db, key, kFp, 1, 200, "stale-body", "application/json"));
  EXPECT_FALSE(repo().fail(*db, key, kFp, 1, "stale", "stale"));

  const auto intact = repo().find_by_key(*db, key);
  ASSERT_TRUE(intact.has_value());
  EXPECT_EQ(intact->status, RecordStatus::Processing);
  EXPECT_EQ(intact->fencing_epoch, 2);

  EXPECT_TRUE(repo().complete(*db, key, kFp, 2, 200, "current-body", "application/json"));
  const auto done = repo().find_by_key(*db, key);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->status, RecordStatus::Completed);
  EXPECT_EQ(done->response_body, "current-body");
}

TEST_F(PgFixture, StaleEpochCannotTouchTerminalStates) {
  // F6/F7: after COMPLETED (resp. FAILED) at epoch N, writes presenting any
  // older epoch — or the current epoch, since the row is terminal — affect
  // zero rows and the stored result is byte-identical.
  REQUIRE_PG();
  for (const char* stem : {"stale-completed", "stale-failed"}) {
    const std::string key = unique_key(stem);
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, kFp).outcome, AcquireOutcome::Created);
    ASSERT_EQ(repo().try_recover(*db, key, kFp, 1), 2);
    if (std::string(stem) == "stale-completed") {
      ASSERT_TRUE(repo().complete(*db, key, kFp, 2, 200, "winner", "application/json"));
    } else {
      ASSERT_TRUE(repo().fail(*db, key, kFp, 2, "e", "m"));
    }
    EXPECT_FALSE(repo().complete(*db, key, kFp, 1, 500, "stale", "application/json"));
    EXPECT_FALSE(repo().fail(*db, key, kFp, 1, "stale", "stale"));
    EXPECT_FALSE(repo().complete(*db, key, kFp, 2, 500, "late", "application/json"));
    EXPECT_FALSE(repo().fail(*db, key, kFp, 2, "late", "late"));

    const auto record = repo().find_by_key(*db, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->fencing_epoch, 2);
    if (std::string(stem) == "stale-completed") {
      EXPECT_EQ(record->response_body, "winner");
    } else {
      EXPECT_EQ(record->error_code, "e");
    }
  }
}

TEST_F(PgFixture, ConcurrentRecoveryElectsExactlyOneWinner) {
  // F8: eight recoverers present the same observed epoch 1 at the same
  // instant (latch barrier). Exactly one advances to 2; every loser gets
  // nullopt and must re-read. Final epoch is exactly 2 — no double bump.
  REQUIRE_PG();
  const std::string key = unique_key("epochrace");
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, kFp).outcome, AcquireOutcome::Created);
  }

  constexpr int kRecoverers = 8;
  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<std::optional<std::int64_t>> outcomes(kRecoverers);
  for (int i = 0; i < kRecoverers; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      auto db = raw_connect();
      outcomes[i] = repo().try_recover(*db, key, kFp, 1);
    });
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  int winners = 0;
  for (const auto& outcome : outcomes) {
    if (outcome.has_value()) {
      ++winners;
      EXPECT_EQ(*outcome, 2);
    }
  }
  EXPECT_EQ(winners, 1) << "exactly one recovery generation may win";

  auto db = raw_connect();
  EXPECT_EQ(repo().find_by_key(*db, key)->fencing_epoch, 2);
}

TEST_F(PgFixture, RacingTerminalWritesLeaveOnlyTheCurrentResult) {
  // F9: the old generation (epoch 1) and the current one (epoch 2) fire
  // their terminal writes at the same instant. Whichever order the database
  // serializes them in, only the current epoch's result may stand — the
  // stale write affects zero rows by predicate, not by timing luck.
  REQUIRE_PG();
  const std::string key = unique_key("epochwrite-race");
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, kFp).outcome, AcquireOutcome::Created);
    ASSERT_EQ(repo().try_recover(*db, key, kFp, 1), 2);
  }

  std::latch go{1};
  bool stale_wrote = true;
  bool current_wrote = false;
  std::thread stale([&] {
    go.wait();
    auto db = raw_connect();
    stale_wrote = repo().complete(*db, key, kFp, 1, 200, "stale", "application/json");
  });
  std::thread current([&] {
    go.wait();
    auto db = raw_connect();
    current_wrote = repo().complete(*db, key, kFp, 2, 200, "current", "application/json");
  });
  go.count_down();
  stale.join();
  current.join();

  EXPECT_FALSE(stale_wrote) << "stale epoch must affect zero rows";
  EXPECT_TRUE(current_wrote) << "current epoch must commit";

  auto db = raw_connect();
  const auto record = repo().find_by_key(*db, key);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->status, RecordStatus::Completed);
  EXPECT_EQ(record->response_body, "current");
  EXPECT_EQ(record->fencing_epoch, 2);
}

}  // namespace
}  // namespace apex::persistence
