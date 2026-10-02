#pragma once

// Live-infrastructure test fixture: real PostgreSQL AND real Redis, no fakes.
// Phase 2 proves lease ownership, fencing, and recovery against the actual
// systems — a mock would assert nothing about the real failure modes.
//
// Gating: PostgreSQL needs APEX_TEST_POSTGRES_CONN (libpq conninfo); Redis
// needs APEX_TEST_REDIS_HOST (+ optional APEX_TEST_REDIS_PORT). When either
// is unset, gated tests GTEST_SKIP() with a clear reason, so plain `ctest`
// stays hermetic without Docker (AGENTS.md §5) while
// `scripts/test.ps1 -WithPostgres` runs the full matrix.
//
// Isolation: each test mints unique keys (prefix + atomic counter); TearDown
// deletes exactly those PG rows and their lease keys. Tests never depend on
// execution order and never truncate shared tables or flush the database.
// Schema: ensured once per process from migrations/ (idempotent scripts).

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "persistence/IdempotencyRepository.hpp"

namespace apex::persistence {
class ConnectionPool;
class PgConnection;
}  // namespace apex::persistence

namespace apex::coordination {
class LeaseManager;
class RedisClient;
class CompletionSubscriber;
}  // namespace apex::coordination

namespace apex::observability {
class Logger;
class Metrics;
}

namespace apex::idempotency {
class IdempotencyService;
class OperationExecutor;
class WaiterRegistry;
struct WaiterOptions;
}  // namespace apex::idempotency

namespace apex::test {

class GatedExecutor;

class PgFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite();
  void SetUp() override;
  void TearDown() override;

  // True when the corresponding env is set. Call REQUIRE_PG() /
  // REQUIRE_REDIS() first thing in every gated test body.
  static bool pg_available();
  static bool redis_available();

  // Mints a unique, valid idempotency key for `stem` and registers it for
  // TearDown cleanup (PG row + lease key).
  std::string unique_key(const std::string& stem);

  // Direct (non-pooled) connection for raw-SQL failure tests.
  std::unique_ptr<persistence::PgConnection> raw_connect();

  persistence::ConnectionPool& pool();
  persistence::IdempotencyRepository& repo();
  coordination::LeaseManager& leases();
  coordination::RedisClient& redis();

  // Isolated node bundle for cross-node / subscriber-control tests: a fresh
  // service with its own registry (own waiter slots) sharing the process
  // pool/Redis/leases. The bundled subscriber starts STOPPED; start it
  // explicitly when the test needs cross-node wake-up, leave it stopped to
  // exercise the fallback path. Destruction stops the subscriber first.
  struct NodeBundle {
    std::shared_ptr<idempotency::WaiterRegistry> registry;
    std::shared_ptr<GatedExecutor> executor;
    std::shared_ptr<idempotency::IdempotencyService> service;
    std::shared_ptr<coordination::CompletionSubscriber> subscriber;
    ~NodeBundle();
  };

  [[nodiscard]] static NodeBundle make_node(bool gate_open = true);
  [[nodiscard]] static NodeBundle make_node_with_options(
      const idempotency::WaiterOptions& options, bool gate_open = true);
  // Node sharing an externally owned executor (cross-node execution
  // counting: one counter across both nodes proves single execution).
  [[nodiscard]] static NodeBundle make_node_with_executor(
      std::shared_ptr<GatedExecutor> executor, const idempotency::WaiterOptions& options);

  // Default waiter options for tests (short fallback, generous timeout).
  // Production defaults live in Config and are far more conservative.
  [[nodiscard]] static idempotency::WaiterOptions test_waiter_options();

  // Ad-hoc service builder for fail-closed tests (dead pool / dead Redis /
  // null leases). Null executor/registry/redis fall back to a simulated
  // executor, a fresh registry, and the shared Redis client.
  [[nodiscard]] static std::shared_ptr<idempotency::IdempotencyService> make_service(
      std::shared_ptr<persistence::ConnectionPool> pool, observability::Logger& logger,
      std::shared_ptr<coordination::LeaseManager> leases,
      std::shared_ptr<idempotency::OperationExecutor> executor = nullptr,
      std::shared_ptr<idempotency::WaiterRegistry> registry = nullptr,
      std::shared_ptr<coordination::RedisClient> redis = nullptr);

 private:
  static std::optional<std::string> s_conninfo;
  static std::shared_ptr<persistence::ConnectionPool> s_pool;
  static std::unique_ptr<observability::Logger> s_logger;
  static std::shared_ptr<idempotency::IdempotencyService> s_service;
  static persistence::IdempotencyRepository s_repo;
  static std::atomic<std::uint64_t> s_counter;

  static std::optional<std::string> s_redis_host;
  static std::uint16_t s_redis_port;
  static std::shared_ptr<coordination::RedisClient> s_redis;
  static std::shared_ptr<coordination::LeaseManager> s_leases;
  static std::shared_ptr<idempotency::WaiterRegistry> s_registry;
  static std::shared_ptr<idempotency::OperationExecutor> s_executor;
  static std::shared_ptr<coordination::CompletionSubscriber> s_subscriber;
  static std::shared_ptr<observability::Metrics> s_metrics;

 protected:
  // Shared service for HTTP-level tests (wired into TestServer).
  static std::shared_ptr<idempotency::IdempotencyService> service() { return s_service; }
  static std::shared_ptr<persistence::ConnectionPool> shared_pool() { return s_pool; }
  static std::shared_ptr<coordination::LeaseManager> shared_leases() { return s_leases; }
  static std::shared_ptr<coordination::RedisClient> shared_redis_client() { return s_redis; }
  static std::shared_ptr<observability::Metrics> shared_metrics() { return s_metrics; }
  static std::shared_ptr<idempotency::WaiterRegistry> shared_registry() { return s_registry; }
  // Shared (stopped) subscriber for tests that need cross-node wake-up on
  // the shared registry. Start explicitly; most tests leave it stopped
  // (local notify + fallback cover same-process waiting).
  static std::shared_ptr<coordination::CompletionSubscriber> shared_subscriber() {
    return s_subscriber;
  }

  std::vector<std::string> owned_keys_;
};

#define REQUIRE_PG()                                                                          \
  do {                                                                                        \
    if (!::apex::test::PgFixture::pg_available()) {                                           \
      GTEST_SKIP() << "APEX_TEST_POSTGRES_CONN is not set; run "                              \
                      "powershell ./scripts/test.ps1 -WithPostgres for the live suite.";     \
    }                                                                                         \
  } while (0)

#define REQUIRE_REDIS()                                                                       \
  do {                                                                                        \
    if (!::apex::test::PgFixture::redis_available()) {                                        \
      GTEST_SKIP() << "APEX_TEST_REDIS_HOST is not set; run "                                 \
                      "powershell ./scripts/test.ps1 -WithPostgres for the live suite.";     \
    }                                                                                         \
  } while (0)

}  // namespace apex::test
