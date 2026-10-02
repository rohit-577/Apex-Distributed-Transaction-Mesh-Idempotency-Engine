#pragma once

// PostgreSQL-gated test fixture. Real database, no fakes (Phase 1 proves
// durability and concurrency against the actual system of record).
//
// Gating: the suite needs APEX_TEST_POSTGRES_CONN set to a libpq conninfo
// string. When unset, every test GTEST_SKIP()s with a clear reason, so plain
// `ctest` stays hermetic without Docker (AGENTS.md §5) while
// `scripts/test.ps1 -WithPostgres` runs the full matrix.
//
// Isolation: each test mints unique keys (prefix + atomic counter) and the
// fixture deletes exactly those rows in TearDown. Tests never depend on
// execution order and never truncate shared tables.
// Schema: ensured once per process from migrations/ (idempotent script).

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

  // True when APEX_TEST_POSTGRES_CONN is set. Call REQUIRE_PG() first thing
  // in every gated test body.
  static bool pg_available();

  // Mints a unique, valid idempotency key for `stem` and registers it for
  // TearDown cleanup.
  std::string unique_key(const std::string& stem);

  // Direct (non-pooled) connection for raw-SQL failure tests.
  std::unique_ptr<persistence::PgConnection> raw_connect();

  persistence::ConnectionPool& pool();
  persistence::IdempotencyRepository& repo();

 private:
  static std::optional<std::string> s_conninfo;
  static std::shared_ptr<persistence::ConnectionPool> s_pool;
  static std::unique_ptr<observability::Logger> s_logger;
  static std::shared_ptr<idempotency::IdempotencyService> s_service;
  static persistence::IdempotencyRepository s_repo;
  static std::atomic<std::uint64_t> s_counter;

 protected:
  // Shared service for HTTP-level tests (wired into TestServer).
  static std::shared_ptr<idempotency::IdempotencyService> service() { return s_service; }
  static std::shared_ptr<persistence::ConnectionPool> shared_pool() { return s_pool; }

  std::vector<std::string> owned_keys_;
};

#define REQUIRE_PG()                                                                          \
  do {                                                                                        \
    if (!::apex::test::PgFixture::pg_available()) {                                           \
      GTEST_SKIP() << "APEX_TEST_POSTGRES_CONN is not set; run "                              \
                      "powershell ./scripts/test.ps1 -WithPostgres for the live suite.";     \
    }                                                                                         \
  } while (0)

}  // namespace apex::test
