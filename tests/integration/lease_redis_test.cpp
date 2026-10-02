// Lease tests against REAL Redis (gated by APEX_TEST_REDIS_HOST).
// R1 connectivity, R2 first acquisition, R3 contention, R4 expiry,
// R5 safe release, R7 outage. R6 (renewal) is N/A by design — renewal is
// explicitly deferred, and the release-rejection path it would need is
// covered by R5.

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_helpers.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"

namespace apex::coordination {
namespace {

using test::PgFixture;

using namespace std::chrono_literals;

// Polls until the lease key disappears (TTL expiry) or the bound elapses.
// Expiry WILL happen (Redis guarantees it); the bound only guards the suite
// against hanging on infrastructure failure. This waits on an outcome, not
// on timing: no correctness depends on how long expiry takes.
bool wait_for_expiry(RedisClient& redis, const std::string& lease_key) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!redis.get(lease_key).has_value()) {
      return true;
    }
    std::this_thread::sleep_for(50ms);
  }
  return false;
}

TEST_F(PgFixture, RedisPingSucceeds) {
  // R1: the coordination dependency is reachable with the test endpoint.
  REQUIRE_REDIS();
  EXPECT_NO_THROW(redis().ping());
}

TEST_F(PgFixture, FirstAcquisitionWinsTheLease) {
  // R2: SET NX PX stores our token. The TTL is armed (a crashed holder can
  // never wedge the key), and the guarded release removes exactly our lease.
  REQUIRE_REDIS();
  const std::string key = unique_key("lease-first");
  const LeaseAttempt attempt = leases().try_acquire(key);
  ASSERT_EQ(attempt.result, LeaseAttempt::Result::Acquired);
  EXPECT_EQ(attempt.token.size(), 32u);

  const auto stored = redis().get(LeaseManager::lease_key_for(key));
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(*stored, attempt.token) << "stored value must be OUR token";

  EXPECT_EQ(redis().compare_and_delete(LeaseManager::lease_key_for(key), "wrong"), 0);
  EXPECT_TRUE(leases().release(key, attempt.token));
  EXPECT_FALSE(redis().get(LeaseManager::lease_key_for(key)).has_value());
}

TEST_F(PgFixture, SecondOwnerCannotAcquireActiveLease) {
  // R3: contention resolves to exactly one holder; the loser is told so
  // (no exception, no silent queue).
  REQUIRE_REDIS();
  const std::string key = unique_key("lease-contend");
  const LeaseAttempt first = leases().try_acquire(key);
  ASSERT_EQ(first.result, LeaseAttempt::Result::Acquired);

  const LeaseAttempt second = leases().try_acquire(key);
  EXPECT_EQ(second.result, LeaseAttempt::Result::HeldByOther);
  EXPECT_TRUE(second.token.empty()) << "losers get no token";

  // The stored token is still the first holder's.
  EXPECT_EQ(redis().get(LeaseManager::lease_key_for(key)), first.token);
  EXPECT_TRUE(leases().release(key, first.token));
}

TEST_F(PgFixture, ExpiredLeaseCanBeReacquired) {
  // R4: TTL expiry frees the lease; a new generation acquires with its own
  // token. Short TTL (1 s) + expiry polling: the outcome is guaranteed by
  // Redis, the wait is bounded infrastructure patience, not synchronization.
  REQUIRE_REDIS();
  const std::string key = unique_key("lease-expire");
  const std::string lease_key = LeaseManager::lease_key_for(key);

  ASSERT_TRUE(redis().set_if_absent(lease_key, "generation-1", 1000ms));
  ASSERT_TRUE(wait_for_expiry(redis(), lease_key)) << "TTL did not free the key";

  const LeaseAttempt second = leases().try_acquire(key);
  ASSERT_EQ(second.result, LeaseAttempt::Result::Acquired);
  EXPECT_EQ(redis().get(lease_key), second.token);
  EXPECT_TRUE(leases().release(key, second.token));
}

TEST_F(PgFixture, StaleOwnerCannotReleaseNewerLease) {
  // R5: the release race the Lua script exists for. A holds the lease; A's
  // key expires (explicit delete = identical observable state to TTL expiry,
  // and R4 proves real expiry frees the key); B acquires; A's guarded
  // release MUST fail and B's lease MUST survive intact.
  REQUIRE_REDIS();
  const std::string key = unique_key("lease-release");
  const std::string lease_key = LeaseManager::lease_key_for(key);

  const LeaseAttempt a = leases().try_acquire(key);
  ASSERT_EQ(a.result, LeaseAttempt::Result::Acquired);

  redis().del(lease_key);  // Simulates TTL expiry (see R4 for the real thing).

  const LeaseAttempt b = leases().try_acquire(key);
  ASSERT_EQ(b.result, LeaseAttempt::Result::Acquired);
  ASSERT_NE(a.token, b.token);

  EXPECT_FALSE(leases().release(key, a.token)) << "stale token must not delete";
  EXPECT_EQ(redis().get(lease_key), b.token) << "newer's lease must survive";

  EXPECT_TRUE(leases().release(key, b.token));
  EXPECT_FALSE(leases().release(key, b.token)) << "double release deletes nothing";
}

TEST(RedisOutageTest, LeaseOperationsFailClosedWithoutInfrastructure) {
  // R7, hermetic: no fixture, no Docker, no live anything. Every lease
  // operation against a dead Redis is a controlled failure — never an
  // escaping exception, never a phantom acquisition. Release fails safe
  // (false): the TTL owns cleanup regardless.
  RedisEndpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = test::acquire_closed_port();
  endpoint.connect_timeout = 500ms;
  endpoint.command_timeout = 500ms;
  endpoint.pool_size = 1;
  auto dead_client = std::make_shared<RedisClient>(std::move(endpoint));
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto metrics = std::make_shared<apex::observability::Metrics>();
  LeaseManager dead_leases(dead_client, 10000ms, quiet, metrics);

  EXPECT_THROW(dead_client->ping(), RedisError);

  const LeaseAttempt attempt = dead_leases.try_acquire("any-key");
  EXPECT_EQ(attempt.result, LeaseAttempt::Result::RedisUnavailable);
  EXPECT_TRUE(attempt.token.empty()) << "fail-closed mints no token";
  EXPECT_FALSE(dead_leases.release("any-key", "whatever-token"));

  const auto snapshot = metrics->snapshot();
  EXPECT_EQ(snapshot.lease_unavailable, 0u) << "manager counts redis_failures, not lease_unavailable";
  EXPECT_GE(snapshot.redis_failures, 1u);
}

}  // namespace
}  // namespace apex::coordination
