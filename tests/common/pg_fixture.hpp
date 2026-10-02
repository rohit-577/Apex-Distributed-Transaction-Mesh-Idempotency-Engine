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
}  // namespace apex::coordination

namespace apex::observability {
class Logger;
}

namespace apex::idempotency {
class IdempotencyService;
}

namespace apex::test {

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

 protected:
  // Shared service for HTTP-level tests (wired into TestServer).
  static std::shared_ptr<idempotency::IdempotencyService> service() { return s_service; }
  static std::shared_ptr<persistence::ConnectionPool> shared_pool() { return s_pool; }
  static std::shared_ptr<coordination::LeaseManager> shared_leases() { return s_leases; }

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
