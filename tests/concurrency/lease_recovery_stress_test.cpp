// Phase 2 concurrency stress: real gateway + real PostgreSQL + real Redis.
// 100-way same-key fan-in, concurrent recovery after expiry, multiple keys,
// mixed fingerprints, owner-completion racing recovery. Barrier-coordinated
// (std::latch) where ordering matters; outcome-asserted (never timing).

#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/LeaseManager.hpp"
#include "idempotency/Fingerprint.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

using test::PgFixture;

config::Config stress_config() {
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

int row_count(const std::string& key) {
  persistence::PgConnection db(*test::pg_test_conninfo());
  const persistence::PgResult count = db.exec_params(
      "SELECT count(*) FROM idempotency_records WHERE idempotency_key = $1", {key});
  return std::stoi(count.value(0, 0));
}

std::int64_t row_epoch(const std::string& key) {
  persistence::PgConnection db(*test::pg_test_conninfo());
  const persistence::PgResult row = db.exec_params(
      "SELECT fencing_epoch FROM idempotency_records WHERE idempotency_key = $1", {key});
  return row.rows() == 1 ? std::stoll(row.value(0, 0)) : -1;
}

TEST_F(PgFixture, HundredContendersForOneKeyStayConsistent) {
  // 100-way fan-in on one key: every contender observes a consistent record
  // (200 executed/replayed or 202 active-owner), exactly one durable row
  // exists, and the final replay is stable. No uniqueness or lease error may
  // escape as an uncontrolled status.
  REQUIRE_PG();
  REQUIRE_REDIS();
  constexpr int kContenders = 100;
  test::TestServer server(stress_config(), service());
  const std::string key = unique_key("stress100");
  const std::string body = R"({"stress":100})";

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
  for (int status : statuses) {
    EXPECT_TRUE(status == 200 || status == 202) << "status " << status << " escaped";
    executed += (status == 200) ? 1 : 0;
  }
  EXPECT_GE(executed, 1);
  EXPECT_EQ(row_count(key), 1) << "INV-12 under 100-way fan-in";
  EXPECT_EQ(row_epoch(key), 1) << "no recovery should have been needed";

  const test::HttpResult replay = post_key(server.port(), key, body);
  EXPECT_EQ(replay.status, 200);
}

TEST_F(PgFixture, ConcurrentRecoveryElectsOneOwner) {
  // Orphaned PROCESSING (no lease) + 16 same-fingerprint contenders: exactly
  // one recovery generation (epoch 2) executes; losers see 202/200; the
  // final state is one COMPLETED row at epoch 2 with a stable replay.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("stress-recover");
  const std::string body = R"({"orphan":true})";
  {
    auto db = raw_connect();
    const idempotency::Fingerprint fp =
        idempotency::fingerprint_for("POST", "/v1/operations", body);
    ASSERT_TRUE(fp.ok);
    ASSERT_EQ(repo().try_acquire(*db, key, fp.hex).outcome,
              persistence::AcquireOutcome::Created);
  }

  constexpr int kContenders = 16;
  test::TestServer server(stress_config(), service());
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

  for (int status : statuses) {
    EXPECT_TRUE(status == 200 || status == 202) << "status " << status << " escaped";
  }
  EXPECT_EQ(row_count(key), 1);
  EXPECT_EQ(row_epoch(key), 2) << "exactly one recovery generation";

  const test::HttpResult replay = post_key(server.port(), key, body);
  EXPECT_EQ(replay.status, 200);
  EXPECT_NE(replay.body.find("\"result\":\"ok\""), std::string::npos);
}

TEST_F(PgFixture, ManyDifferentKeysProceedInParallel) {
  // 8 keys x 8 contenders: independent keys never interfere (no cross-key
  // lease or epoch coupling). Every key ends COMPLETED at epoch 1.
  REQUIRE_PG();
  REQUIRE_REDIS();
  constexpr int kKeys = 8;
  constexpr int kPerKey = 8;
  test::TestServer server(stress_config(), service());

  std::vector<std::string> keys;
  for (int k = 0; k < kKeys; ++k) {
    keys.push_back(unique_key("stress-multi-" + std::to_string(k)));
  }

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(kKeys * kPerKey, 0);
  for (int k = 0; k < kKeys; ++k) {
    for (int i = 0; i < kPerKey; ++i) {
      const int slot = k * kPerKey + i;
      threads.emplace_back([&, slot, k] {
        go.wait();
        statuses[slot] =
            post_key(server.port(), keys[k], R"({"k":1})").status;
      });
    }
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  for (int status : statuses) {
    EXPECT_TRUE(status == 200 || status == 202) << "status " << status << " escaped";
  }
  for (const std::string& key : keys) {
    EXPECT_EQ(row_count(key), 1) << key;
    EXPECT_EQ(row_epoch(key), 1) << key;
  }
}

TEST_F(PgFixture, MixedFingerprintsUnderOneKeyResolveCleanly) {
  // 3 fingerprints x 5 contenders on one key: exactly one logical operation
  // wins (its contenders see 200/202); every other fingerprint sees 409;
  // one row holds the winner's fingerprint.
  REQUIRE_PG();
  REQUIRE_REDIS();
  test::TestServer server(stress_config(), service());
  const std::string key = unique_key("stress-mixed");
  const std::vector<std::string> bodies{R"({"side":"a"})", R"({"side":"b"})",
                                        R"({"side":"c"})"};
  constexpr int kPerBody = 5;

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(bodies.size() * kPerBody, 0);
  for (std::size_t b = 0; b < bodies.size(); ++b) {
    for (int i = 0; i < kPerBody; ++i) {
      const int slot = static_cast<int>(b * kPerBody + i);
      threads.emplace_back([&, slot, b] {
        go.wait();
        statuses[slot] = post_key(server.port(), key, bodies[b]).status;
      });
    }
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  // Ownership decides, then durability decides — per contender:
  // - 200: this contender's body won the lease and executed.
  // - 202: arrived before the row committed and lost the lease race
  //   (cannot know fingerprints yet => honest defer, NOT a missed
  //   conflict — the retry converges to 200/409).
  // - 409: arrived after the row committed with a foreign fingerprint.
  // Exactly one body may contain 200s (single executor); every body is
  // otherwise 202/409. The post-hoc probe below (winner replays 200,
  // losers stay 409) is the deterministic verdict.
  int bodies_with_200 = 0;
  for (std::size_t b = 0; b < bodies.size(); ++b) {
    bool any_200 = false;
    for (int i = 0; i < kPerBody; ++i) {
      const int status = statuses[b * kPerBody + i];
      EXPECT_TRUE(status == 200 || status == 202 || status == 409)
          << "status " << status << " escaped";
      any_200 = any_200 || status == 200;
    }
    bodies_with_200 += any_200 ? 1 : 0;
  }
  EXPECT_EQ(bodies_with_200, 1) << "exactly one fingerprint may execute";
  EXPECT_EQ(row_count(key), 1);

  // The stored fingerprint replays 200; the others stay 409.
  persistence::PgConnection db(*test::pg_test_conninfo());
  const persistence::PgResult fp_row = db.exec_params(
      "SELECT fingerprint FROM idempotency_records WHERE idempotency_key = $1", {key});
  ASSERT_EQ(fp_row.rows(), 1);
  const std::string stored_fp = fp_row.value(0, 0);
  int replayed = 0;
  for (const std::string& body : bodies) {
    const idempotency::Fingerprint fp =
        idempotency::fingerprint_for("POST", "/v1/operations", body);
    ASSERT_TRUE(fp.ok);
    const test::HttpResult probe = post_key(server.port(), key, body);
    if (fp.hex == stored_fp) {
      EXPECT_EQ(probe.status, 200);
      ++replayed;
    } else {
      EXPECT_EQ(probe.status, 409);
    }
  }
  EXPECT_EQ(replayed, 1);
}

TEST_F(PgFixture, OwnerCompletionRacingRecoveryKeepsOneAuthoritativeResult) {
  // Owner A holds the lease on a PROCESSING row while BOTH A's completion
  // (epoch 1, direct repository call) and B's recovery attempt (epoch 2,
  // direct repository call) fire at once. Whichever terminal write the
  // database serializes, the predicate admits only the current epoch — the
  // test asserts the end state is exactly one authoritative result and that
  // a subsequent service-level duplicate converges on it.
  REQUIRE_PG();
  REQUIRE_REDIS();
  const std::string key = unique_key("stress-owner-race");
  const std::string body = R"({"owner":"race"})";
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", body);
  ASSERT_TRUE(fp.ok);
  {
    auto db = raw_connect();
    ASSERT_EQ(repo().try_acquire(*db, key, fp.hex).outcome,
              persistence::AcquireOutcome::Created);
  }
  // A holds the lease (active owner); B will race recovery anyway to prove
  // the predicate decides, not the lease.
  const coordination::LeaseAttempt lease_a = leases().try_acquire(key);
  ASSERT_EQ(lease_a.result, coordination::LeaseAttempt::Result::Acquired);

  std::latch go{1};
  bool a_wrote = true;
  std::optional<std::int64_t> b_epoch;
  std::thread owner([&] {
    go.wait();
    auto db = raw_connect();
    a_wrote = repo().complete(*db, key, fp.hex, 1, 200, R"({"by":"owner"})",
                              "application/json");
  });
  std::thread recoverer([&] {
    go.wait();
    auto db = raw_connect();
    b_epoch = repo().try_recover(*db, key, fp.hex, 1);
  });
  go.count_down();
  owner.join();
  recoverer.join();

  // Exactly one of them changed durable state: either A completed at epoch
  // 1 (B's CAS then fails and B must not write), or B advanced to epoch 2
  // first (A's epoch-1 write then affects zero rows).
  auto db = raw_connect();
  const auto record = repo().find_by_key(*db, key);
  ASSERT_TRUE(record.has_value());
  if (record->status == persistence::RecordStatus::Completed) {
    EXPECT_TRUE(a_wrote);
    EXPECT_EQ(record->response_body, R"({"by":"owner"})");
    EXPECT_FALSE(b_epoch.has_value()) << "no recovery past a terminal row";
  } else {
    EXPECT_FALSE(a_wrote) << "owner write must lose once epoch advanced";
    ASSERT_TRUE(b_epoch.has_value());
    EXPECT_EQ(record->fencing_epoch, *b_epoch);
    // Finish B's generation through the real service path (lease still held
    // by A out-of-band... release it first: the test owns both sides).
    EXPECT_TRUE(leases().release(key, lease_a.token));
    test::TestServer server(stress_config(), service());
    EXPECT_EQ(post_key(server.port(), key, body).status, 200);
    const auto final_row = repo().find_by_key(*db, key);
    ASSERT_TRUE(final_row.has_value());
    EXPECT_EQ(final_row->status, persistence::RecordStatus::Completed);
  }
}

}  // namespace
}  // namespace apex
