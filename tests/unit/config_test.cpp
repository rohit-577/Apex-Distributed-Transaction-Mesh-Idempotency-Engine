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
  test::EnvGuard redis_host("APEX_REDIS_HOST", "cache.internal");
  test::EnvGuard redis_port("APEX_REDIS_PORT", "6633");
  test::EnvGuard level("APEX_LOG_LEVEL", "debug");

  const auto [cfg, warnings] = Config::load_from_environment();
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(cfg.port, 18080);
  EXPECT_EQ(cfg.threads, 3u);
  EXPECT_EQ(cfg.postgres_host, "db.internal");
  EXPECT_EQ(cfg.postgres_port, 5544);
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

}  // namespace
}  // namespace apex::config
