#include "common/pg_fixture.hpp"

#include <chrono>

#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/OperationExecutor.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
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
std::shared_ptr<idempotency::WaiterRegistry> PgFixture::s_registry;
std::shared_ptr<idempotency::OperationExecutor> PgFixture::s_executor;
std::shared_ptr<coordination::CompletionSubscriber> PgFixture::s_subscriber;
std::shared_ptr<observability::Metrics> PgFixture::s_metrics;

idempotency::WaiterOptions PgFixture::test_waiter_options() {
  // Fast tests, honest semantics: short fallback interval exercises the
  // re-check path quickly; the 5 s timeout only fires in tests that
  // deliberately outlast it (P3-13 overrides it explicitly). Production
  // defaults are far more conservative (see Config).
  idempotency::WaiterOptions options;
  options.timeout_ms = 5000;
  options.recheck_ms = 100;
  options.max_waiters_per_key = 1024;
  return options;
}

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
  s_metrics = std::make_shared<observability::Metrics>();
  if (s_redis_host) {
    // Short timeouts in tests: a dead Redis must fail fast, never stall the
    // suite. Generous 10 s lease TTL mirrors production default.
    coordination::RedisEndpoint endpoint;
    endpoint.host = *s_redis_host;
    endpoint.port = s_redis_port;
    endpoint.connect_timeout = std::chrono::milliseconds(2000);
    endpoint.command_timeout = std::chrono::milliseconds(2000);
    endpoint.pool_size = 32;
    s_redis = std::make_shared<coordination::RedisClient>(std::move(endpoint));
    s_leases = std::make_shared<coordination::LeaseManager>(
        s_redis, std::chrono::milliseconds(10000), *s_logger, s_metrics);
  }
  s_registry = std::make_shared<idempotency::WaiterRegistry>(test_waiter_options());
  s_executor = std::make_shared<idempotency::SimulatedExecutor>();
  idempotency::ServiceDependencies deps;
  deps.pool = s_pool;
  deps.leases = s_leases;
  deps.executor = s_executor;
  deps.registry = s_registry;
  deps.redis = s_redis;
  deps.metrics = s_metrics;
  s_service = std::make_shared<idempotency::IdempotencyService>(std::move(deps), *s_logger);
  // Stopped until a test explicitly needs cross-node wake-up. Same-process
  // waiting works through the local registry without it.
  s_subscriber = std::make_shared<coordination::CompletionSubscriber>(s_redis, *s_registry,
                                                                      *s_logger, s_metrics);
}

PgFixture::NodeBundle::~NodeBundle() {
  if (subscriber != nullptr) {
    subscriber->stop();
  }
}

PgFixture::NodeBundle PgFixture::make_node(bool gate_open) {
  return make_node_with_options(test_waiter_options(), gate_open);
}

PgFixture::NodeBundle PgFixture::make_node_with_executor(
    std::shared_ptr<GatedExecutor> executor, const idempotency::WaiterOptions& options) {
  NodeBundle node;
  node.registry = std::make_shared<idempotency::WaiterRegistry>(options);
  node.executor = std::move(executor);
  idempotency::ServiceDependencies deps;
  deps.pool = s_pool;
  deps.leases = s_leases;
  deps.executor = node.executor;
  deps.registry = node.registry;
  deps.redis = s_redis;
  deps.metrics = s_metrics;
  node.service =
      std::make_shared<idempotency::IdempotencyService>(std::move(deps), *s_logger);
  node.subscriber = std::make_shared<coordination::CompletionSubscriber>(
      s_redis, *node.registry, *s_logger, s_metrics);
  return node;
}

PgFixture::NodeBundle PgFixture::make_node_with_options(
    const idempotency::WaiterOptions& options, bool gate_open) {
  NodeBundle node;
  node.registry = std::make_shared<idempotency::WaiterRegistry>(options);
  node.executor = std::make_shared<GatedExecutor>(gate_open);
  idempotency::ServiceDependencies deps;
  deps.pool = s_pool;
  deps.leases = s_leases;
  deps.executor = node.executor;
  deps.registry = node.registry;
  deps.redis = s_redis;
  deps.metrics = s_metrics;
  node.service =
      std::make_shared<idempotency::IdempotencyService>(std::move(deps), *s_logger);
  node.subscriber = std::make_shared<coordination::CompletionSubscriber>(
      s_redis, *node.registry, *s_logger, s_metrics);
  return node;
}

std::shared_ptr<idempotency::IdempotencyService> PgFixture::make_service(
    std::shared_ptr<persistence::ConnectionPool> pool, observability::Logger& logger,
    std::shared_ptr<coordination::LeaseManager> leases,
    std::shared_ptr<idempotency::OperationExecutor> executor,
    std::shared_ptr<idempotency::WaiterRegistry> registry,
    std::shared_ptr<coordination::RedisClient> redis) {
  idempotency::ServiceDependencies deps;
  deps.pool = std::move(pool);
  deps.leases = std::move(leases);
  deps.executor = executor != nullptr
                      ? std::move(executor)
                      : std::make_shared<idempotency::SimulatedExecutor>();
  deps.registry = registry != nullptr
                      ? std::move(registry)
                      : std::make_shared<idempotency::WaiterRegistry>(test_waiter_options());
  deps.redis = redis != nullptr ? std::move(redis) : s_redis;
  deps.metrics = s_metrics;
  return std::make_shared<idempotency::IdempotencyService>(std::move(deps), logger);
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
  // PID-scoped: keys never collide ACROSS test processes/runs, so rows
  // orphaned by a killed run (whose TearDown never executed) cannot pollute
  // a later run's assertions. Leftovers remain inert and unique.
  const std::string key =
      "pg1-" + std::to_string(test_process_id()) + "-" + stem + "-" + std::to_string(n);
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
