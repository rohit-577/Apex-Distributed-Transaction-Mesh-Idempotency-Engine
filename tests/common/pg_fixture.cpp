#include "common/pg_fixture.hpp"

#include "common/test_helpers.hpp"
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

bool PgFixture::pg_available() { return s_conninfo.has_value(); }

void PgFixture::SetUpTestSuite() {
  s_conninfo = pg_test_conninfo();
  if (!s_conninfo) {
    return;  // Individual tests skip; the suite itself must not fail.
  }
  // Fail fast here (not per-test): a bad conninfo is an environment error,
  // and every gated test would fail identically. The migration script is
  // idempotent, so concurrent/parallel test binaries cannot corrupt it.
  persistence::PgConnection bootstrap(*s_conninfo);
  persistence::Schema::apply(bootstrap, persistence::Schema::read_migration_file(
                                             APEX_MIGRATIONS_DIR));
  s_logger = std::make_unique<observability::Logger>(observability::Level::Warning);
  s_pool = std::make_shared<persistence::ConnectionPool>(*s_conninfo, /*max_size=*/16);
  s_service = std::make_shared<idempotency::IdempotencyService>(s_pool, *s_logger);
}

void PgFixture::SetUp() {
  REQUIRE_PG();
}

void PgFixture::TearDown() {
  if (!pg_available() || owned_keys_.empty()) {
    return;
  }
  try {
    persistence::PgConnection db(*s_conninfo);
    for (const std::string& key : owned_keys_) {
      (void)db.exec_params("DELETE FROM idempotency_records WHERE idempotency_key = $1", {key});
    }
  } catch (const std::exception&) {
    // Cleanup is best-effort: keys are unique per test run, so a failed
    // delete cannot pollute other tests — it only leaves inert rows behind.
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

}  // namespace apex::test
