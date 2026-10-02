#include "common/pg_fixture.hpp"

#include <chrono>

#include "common/test_helpers.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "observability/Logger.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/PgConnection.hpp"
#include "persistence/Schema.hpp"

namespace apex::test {

std::optional<std::string> PgFixture::s_conninfo;
std::shared_ptr<persistence::ConnectionPool> PgFixture::s_pool;
std::unique_ptr<observability::Logger> PgFixture::s_logger;
std::shared_ptr<idempotency::IdempotencyService> PgFixture::s_service;
persistence::IdempotencyRepository PgFixture::s_repo;
std::atomic<std::uint64_t> PgFixture::s_counter{0};

std::optional<std::string> PgFixture::s_redis_host;
std::uint16_t PgFixture::s_redis_port{6379};
std::shared_ptr<coordination::RedisClient> PgFixture::s_redis;
std::shared_ptr<coordination::LeaseManager> PgFixture::s_leases;

bool PgFixture::pg_available() { return s_conninfo.has_value(); }
bool PgFixture::redis_available() { return s_redis_host.has_value(); }

void PgFixture::SetUpTestSuite() {
  s_conninfo = pg_test_conninfo();
  if (const auto redis_endpoint = redis_test_endpoint()) {
    s_redis_host = redis_endpoint->host;
    s_redis_port = redis_endpoint->port;
  }
  if (!s_conninfo) {
    return;  // Individual tests skip; the suite itself must not fail.
  }
  // Fail fast here (not per-test): a bad conninfo is an environment error,
  // and every gated test would fail identically. Migrations are idempotent,
  // so concurrent/parallel test binaries cannot corrupt them.
  persistence::PgConnection bootstrap(*s_conninfo);
  persistence::Schema::ensure(bootstrap, APEX_MIGRATIONS_DIR);
  s_logger = std::make_unique<observability::Logger>(observability::Level::Warning);
  s_pool = std::make_shared<persistence::ConnectionPool>(*s_conninfo, /*max_size=*/16);
  if (s_redis_host) {
    // Short timeouts in tests: a dead Redis must fail fast, never stall the
    // suite. Generous 10 s lease TTL mirrors production default. The pool is
    // sized for the burst tests (50–100 simultaneous contenders); this is
    // environmental headroom, not production sizing (default 8, documented
    // in docs/architecture.md).
    coordination::RedisEndpoint endpoint;
    endpoint.host = *s_redis_host;
    endpoint.port = s_redis_port;
    endpoint.connect_timeout = std::chrono::milliseconds(2000);
    endpoint.command_timeout = std::chrono::milliseconds(2000);
    endpoint.pool_size = 32;
    s_redis = std::make_shared<coordination::RedisClient>(std::move(endpoint));
    s_leases = std::make_shared<coordination::LeaseManager>(
        s_redis, std::chrono::milliseconds(10000), *s_logger);
  }
  s_service =
      std::make_shared<idempotency::IdempotencyService>(s_pool, *s_logger, s_leases);
}

void PgFixture::SetUp() {
  REQUIRE_PG();
}

void PgFixture::TearDown() {
  if (owned_keys_.empty()) {
    return;
  }
  // Best-effort cleanup of exactly our rows + our lease keys. Unique keys
  // mean a failed cleanup cannot pollute other tests.
  if (pg_available()) {
    try {
      persistence::PgConnection db(*s_conninfo);
      for (const std::string& key : owned_keys_) {
        (void)db.exec_params("DELETE FROM idempotency_records WHERE idempotency_key = $1",
                             {key});
      }
    } catch (const std::exception&) {
    }
  }
  if (redis_available() && s_redis) {
    try {
      for (const std::string& key : owned_keys_) {
        s_redis->del(coordination::LeaseManager::lease_key_for(key));
      }
    } catch (const std::exception&) {
    }
  }
  owned_keys_.clear();
}

std::string PgFixture::unique_key(const std::string& stem) {
  const std::uint64_t n = ++s_counter;
  std::string key = "pg1-" + stem + "-" + std::to_string(n);
  owned_keys_.push_back(key);
  return key;
}

std::unique_ptr<persistence::PgConnection> PgFixture::raw_connect() {
  return std::make_unique<persistence::PgConnection>(*s_conninfo);
}

persistence::ConnectionPool& PgFixture::pool() { return *s_pool; }

persistence::IdempotencyRepository& PgFixture::repo() { return s_repo; }

coordination::LeaseManager& PgFixture::leases() { return *s_leases; }

coordination::RedisClient& PgFixture::redis() { return *s_redis; }

}  // namespace apex::test
