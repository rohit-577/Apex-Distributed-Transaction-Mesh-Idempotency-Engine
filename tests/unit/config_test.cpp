// Configuration tests: defaults are valid, every invalid field is rejected
// with a message, and environment overrides are honored. No test here
// touches the network.

#include <gtest/gtest.h>

#include "common/test_helpers.hpp"
#include "config/Config.hpp"

namespace apex::config {
namespace {

TEST(ConfigTest, DefaultsAreValid) {
  const Config cfg = Config::defaults();
  EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_EQ(cfg.postgres_port, 5432);
  EXPECT_EQ(cfg.redis_port, 6379);
  EXPECT_EQ(cfg.postgres_user, "apex");
  EXPECT_EQ(cfg.postgres_db, "apex");
  EXPECT_TRUE(cfg.postgres_password.empty());
  EXPECT_EQ(cfg.db_pool_size, 8u);
  EXPECT_EQ(cfg.migrations_dir, "migrations");
  EXPECT_GE(cfg.threads, 1u);
}

TEST(ConfigTest, ZeroThreadsIsRejected) {
  Config cfg = Config::defaults();
  cfg.threads = 0;
  EXPECT_FALSE(cfg.validate().empty());
}

TEST(ConfigTest, EmptyHostsAreRejected) {
  Config cfg = Config::defaults();
  cfg.postgres_host.clear();
  EXPECT_FALSE(cfg.validate().empty());

  cfg = Config::defaults();
  cfg.redis_host.clear();
  EXPECT_FALSE(cfg.validate().empty());

  cfg = Config::defaults();
  cfg.postgres_user.clear();
  EXPECT_FALSE(cfg.validate().empty());

  cfg = Config::defaults();
  cfg.postgres_db.clear();
  EXPECT_FALSE(cfg.validate().empty());

  cfg = Config::defaults();
  cfg.migrations_dir.clear();
  EXPECT_FALSE(cfg.validate().empty());
}

TEST(ConfigTest, DbPoolSizeBoundsAreEnforced) {
  Config cfg = Config::defaults();
  cfg.db_pool_size = 0;
  EXPECT_FALSE(cfg.validate().empty());

  cfg = Config::defaults();
  cfg.db_pool_size = 65;
  EXPECT_FALSE(cfg.validate().empty());

  cfg = Config::defaults();
  cfg.db_pool_size = 64;
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, ConninfoContainsExpectedFields) {
  Config cfg = Config::defaults();
  cfg.postgres_password = "s3cret";
  const std::string info = cfg.postgres_conninfo();
  EXPECT_NE(info.find("host=127.0.0.1"), std::string::npos);
  EXPECT_NE(info.find("port=5432"), std::string::npos);
  EXPECT_NE(info.find("dbname=apex"), std::string::npos);
  EXPECT_NE(info.find("user=apex"), std::string::npos);
  EXPECT_NE(info.find("password=s3cret"), std::string::npos);
  EXPECT_NE(info.find("connect_timeout="), std::string::npos);
}

TEST(ConfigTest, ConninfoOmitsEmptyPasswordAndQuotesSpecials) {
  Config cfg = Config::defaults();
  EXPECT_EQ(cfg.postgres_conninfo().find("password="), std::string::npos);

  cfg.postgres_password = "a b'c";
  // Value with spaces/quotes must be single-quoted per libpq rules, never
  // interpolated raw.
  EXPECT_NE(cfg.postgres_conninfo().find("password='a b\\'c'"), std::string::npos);
}

TEST(ConfigTest, UnknownLogLevelIsRejected) {
  Config cfg = Config::defaults();
  cfg.log_level = "verbose";
  EXPECT_FALSE(cfg.validate().empty());
}

TEST(ConfigTest, AllKnownLogLevelsAreAccepted) {
  for (const char* level : {"debug", "info", "warning", "error"}) {
    Config cfg = Config::defaults();
    cfg.log_level = level;
    EXPECT_TRUE(cfg.validate().empty()) << level;
  }
}

TEST(ConfigTest, EphemeralPortZeroIsAcceptedForTests) {
  Config cfg = Config::defaults();
  cfg.port = 0;
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, EnvironmentOverridesAreHonored) {
  test::EnvGuard port("APEX_PORT", "18080");
  test::EnvGuard threads("APEX_THREADS", "3");
  test::EnvGuard pg_host("APEX_POSTGRES_HOST", "db.internal");
  test::EnvGuard pg_port("APEX_POSTGRES_PORT", "5544");
  test::EnvGuard pg_user("APEX_POSTGRES_USER", "writer");
  test::EnvGuard pg_password("APEX_POSTGRES_PASSWORD", "pw");
  test::EnvGuard pg_db("APEX_POSTGRES_DB", "apex_test");
  test::EnvGuard db_pool("APEX_DB_POOL_SIZE", "5");
  test::EnvGuard migrations("APEX_MIGRATIONS_DIR", "db/migrations");
  test::EnvGuard redis_host("APEX_REDIS_HOST", "cache.internal");
  test::EnvGuard redis_port("APEX_REDIS_PORT", "6633");
  test::EnvGuard level("APEX_LOG_LEVEL", "debug");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(cfg.port, 18080);
  EXPECT_EQ(cfg.threads, 3u);
  EXPECT_EQ(cfg.postgres_host, "db.internal");
  EXPECT_EQ(cfg.postgres_port, 5544);
  EXPECT_EQ(cfg.postgres_user, "writer");
  EXPECT_EQ(cfg.postgres_password, "pw");
  EXPECT_EQ(cfg.postgres_db, "apex_test");
  EXPECT_EQ(cfg.db_pool_size, 5u);
  EXPECT_EQ(cfg.migrations_dir, "db/migrations");
  EXPECT_EQ(cfg.redis_host, "cache.internal");
  EXPECT_EQ(cfg.redis_port, 6633);
  EXPECT_EQ(cfg.log_level, "debug");
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, InvalidEnvironmentFallsBackToDefaultsWithWarnings) {
  test::EnvGuard port("APEX_PORT", "not-a-port");
  test::EnvGuard threads("APEX_THREADS", "0");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_GE(cfg.threads, 1u);
  EXPECT_EQ(warnings.size(), 2u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, OutOfRangePortsFallBackToDefaults) {
  test::EnvGuard port("APEX_PORT", "99999");
  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_EQ(warnings.size(), 1u);
}

TEST(ConfigTest, CoordinationDefaultsAreValid) {
  const Config cfg = Config::defaults();
  EXPECT_TRUE(cfg.redis_password.empty());
  EXPECT_EQ(cfg.redis_pool_size, 8u);
  EXPECT_EQ(cfg.lease_ttl_ms, 10000u);
  EXPECT_EQ(cfg.redis_op_timeout_ms, 2000u);
  EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();
}

TEST(ConfigTest, CoordinationBoundsAreEnforced) {
  for (unsigned bad_pool : {0u, 65u}) {
    Config cfg = Config::defaults();
    cfg.redis_pool_size = bad_pool;
    EXPECT_FALSE(cfg.validate().empty()) << bad_pool;
  }
  for (unsigned bad_ttl : {0u, 999u, 300001u}) {
    Config cfg = Config::defaults();
    cfg.lease_ttl_ms = bad_ttl;
    EXPECT_FALSE(cfg.validate().empty()) << bad_ttl;
  }
  for (unsigned bad_timeout : {0u, 99u, 60001u}) {
    Config cfg = Config::defaults();
    cfg.redis_op_timeout_ms = bad_timeout;
    EXPECT_FALSE(cfg.validate().empty()) << bad_timeout;
  }
  Config ok = Config::defaults();
  ok.lease_ttl_ms = 1000;
  ok.redis_op_timeout_ms = 100;
  EXPECT_TRUE(ok.validate().empty());
}

TEST(ConfigTest, CoordinationEnvironmentOverridesAreHonored) {
  test::EnvGuard password("APEX_REDIS_PASSWORD", "s3cret");
  test::EnvGuard pool("APEX_REDIS_POOL_SIZE", "5");
  test::EnvGuard ttl("APEX_LEASE_TTL_MS", "15000");
  test::EnvGuard timeout("APEX_REDIS_OP_TIMEOUT_MS", "500");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(cfg.redis_password, "s3cret");
  EXPECT_EQ(cfg.redis_pool_size, 5u);
  EXPECT_EQ(cfg.lease_ttl_ms, 15000u);
  EXPECT_EQ(cfg.redis_op_timeout_ms, 500u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, InvalidCoordinationEnvironmentFallsBackWithWarnings) {
  test::EnvGuard pool("APEX_REDIS_POOL_SIZE", "0");
  test::EnvGuard ttl("APEX_LEASE_TTL_MS", "forever");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_EQ(cfg.redis_pool_size, 8u);
  EXPECT_EQ(cfg.lease_ttl_ms, 10000u);
  EXPECT_EQ(warnings.size(), 2u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, WaiterDefaultsAreValid) {
  const Config cfg = Config::defaults();
  EXPECT_EQ(cfg.waiter_timeout_ms, 30000u);
  EXPECT_EQ(cfg.waiter_recheck_ms, 1000u);
  EXPECT_EQ(cfg.max_waiters_per_key, 1024u);
  EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();
}

TEST(ConfigTest, WaiterBoundsAreEnforced) {
  for (unsigned bad_timeout : {0u, 999u, 300001u}) {
    Config cfg = Config::defaults();
    cfg.waiter_timeout_ms = bad_timeout;
    EXPECT_FALSE(cfg.validate().empty()) << bad_timeout;
  }
  for (unsigned bad_recheck : {0u, 99u, 30001u}) {
    Config cfg = Config::defaults();
    cfg.waiter_recheck_ms = bad_recheck;
    EXPECT_FALSE(cfg.validate().empty()) << bad_recheck;
  }
  for (unsigned bad_cap : {0u, 100001u}) {
    Config cfg = Config::defaults();
    cfg.max_waiters_per_key = bad_cap;
    EXPECT_FALSE(cfg.validate().empty()) << bad_cap;
  }
}

TEST(ConfigTest, WaiterEnvironmentOverridesAreHonored) {
  test::EnvGuard timeout("APEX_WAITER_TIMEOUT_MS", "15000");
  test::EnvGuard recheck("APEX_WAITER_RECHECK_MS", "250");
  test::EnvGuard cap("APEX_MAX_WAITERS_PER_KEY", "64");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(cfg.waiter_timeout_ms, 15000u);
  EXPECT_EQ(cfg.waiter_recheck_ms, 250u);
  EXPECT_EQ(cfg.max_waiters_per_key, 64u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, InvalidWaiterEnvironmentFallsBackWithWarnings) {
  test::EnvGuard timeout("APEX_WAITER_TIMEOUT_MS", "forever");
  test::EnvGuard cap("APEX_MAX_WAITERS_PER_KEY", "0");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_EQ(cfg.waiter_timeout_ms, 30000u);
  EXPECT_EQ(cfg.max_waiters_per_key, 1024u);
  EXPECT_EQ(warnings.size(), 2u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, ReaperDefaultsAreValid) {
  const Config cfg = Config::defaults();
  EXPECT_EQ(cfg.reaper_interval_ms, 30000u);
  EXPECT_EQ(cfg.reaper_batch_size, 10u);
  EXPECT_EQ(cfg.reaper_eligible_after_ms, 30000u);
  EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();
}

TEST(ConfigTest, ReaperBoundsAreEnforced) {
  for (unsigned bad : {0u, 999u, 600001u}) {
    Config cfg = Config::defaults();
    cfg.reaper_interval_ms = bad;
    EXPECT_FALSE(cfg.validate().empty()) << bad;
  }
  for (unsigned bad : {0u, 1001u}) {
    Config cfg = Config::defaults();
    cfg.reaper_batch_size = bad;
    EXPECT_FALSE(cfg.validate().empty()) << bad;
  }
  for (unsigned bad : {0u, 999u, 3600001u}) {
    Config cfg = Config::defaults();
    cfg.reaper_eligible_after_ms = bad;
    EXPECT_FALSE(cfg.validate().empty()) << bad;
  }
}

TEST(ConfigTest, ReaperEnvironmentOverridesAreHonored) {
  test::EnvGuard interval("APEX_REAPER_INTERVAL_MS", "5000");
  test::EnvGuard batch("APEX_REAPER_BATCH_SIZE", "25");
  test::EnvGuard eligible("APEX_REAPER_ELIGIBLE_AFTER_MS", "60000");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(cfg.reaper_interval_ms, 5000u);
  EXPECT_EQ(cfg.reaper_batch_size, 25u);
  EXPECT_EQ(cfg.reaper_eligible_after_ms, 60000u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, InvalidReaperEnvironmentFallsBackWithWarnings) {
  test::EnvGuard interval("APEX_REAPER_INTERVAL_MS", "soon");
  test::EnvGuard batch("APEX_REAPER_BATCH_SIZE", "0");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_EQ(cfg.reaper_interval_ms, 30000u);
  EXPECT_EQ(cfg.reaper_batch_size, 10u);
  EXPECT_EQ(warnings.size(), 2u);
  EXPECT_TRUE(cfg.validate().empty());
}

TEST(ConfigTest, NodeIdDefaultsEmptyAndValidatesLength) {
  const Config cfg = Config::defaults();
  EXPECT_TRUE(cfg.node_id.empty());
  EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();

  Config long_id = Config::defaults();
  long_id.node_id = std::string(65, 'n');
  EXPECT_FALSE(long_id.validate().empty());

  Config ok_id = Config::defaults();
  ok_id.node_id = "gateway-eu-1";
  EXPECT_TRUE(ok_id.validate().empty());
}

TEST(ConfigTest, NodeIdEnvironmentOverrideIsHonored) {
  test::EnvGuard node("APEX_NODE_ID", "gateway-eu-1");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(cfg.node_id, "gateway-eu-1");
  EXPECT_TRUE(cfg.validate().empty());
}

}  // namespace
}  // namespace apex::config
