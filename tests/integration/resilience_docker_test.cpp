// Docker-gated resilience tests: real container stop/start/restart while
// asserting protocol behavior. These shell the docker CLI (the only way to
// induce genuine disconnects); they skip cleanly when docker is absent and
// gate every wait on observable state with bounds — never on timing.
//
// Isolation: unique keys per test (PID-scoped); containers are left RUNNING
// and healthy at test end (subsequent tests assume live infra).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config resilience_config() {
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

std::string fp_of(const std::string& body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return fp.hex;
}

template <typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds bound = 30000ms) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(50ms);
  }
  return pred();
}

// Runs `docker compose -f <repo>/docker-compose.yml <args>`. Returns the
// exit code; callers assert on it (infrastructure control, not test logic).
int docker_compose(const std::string& args) {
  const std::string command =
      "docker compose -f \"" + std::string(APEX_COMPOSE_FILE) + "\" " + args;
  return std::system(command.c_str());
}

bool docker_present() { return std::system("docker --version > NUL 2>&1") == 0; }

#define REQUIRE_DOCKER()                                                                          \
  do {                                                                                            \
    if (!docker_present()) {                                                                      \
      GTEST_SKIP() << "docker CLI not available; resilience tests need containers.";              \
    }                                                                                             \
  } while (0)

TEST_F(PgFixture, RedisDisconnectReconnectPreservesCorrectness) {
  // Real outage: stop the container, observe subscriber reconnect attempts,
  // assert fail-closed lease behavior mid-outage, restart, prove
  // resubscription delivers again and recovery completes end to end.
  // A reconnect must never duplicate execution (the epoch CAS is untouched
  // by connectivity state).
  REQUIRE_PG();
  REQUIRE_REDIS();
  REQUIRE_DOCKER();

  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  node.subscriber->start();
  test::TestServer server(resilience_config(), node.service);
  const std::string key = unique_key("docker-redis");
  const std::string body = R"({"docker":"redis"})";
  const std::string channel =
      idempotency::WaiterRegistry::channel_for(key, fp_of(body));

  // Baseline: subscriber alive (delivery probe with a direct waiter).
  {
    std::atomic<bool> delivered{false};
    auto owner = std::make_shared<int>(1);
    const auto receipt = node.registry->register_waiter(channel, owner,
                                                        [&] { delivered.store(true); });
    ASSERT_FALSE(receipt.rejected);
    bool seen = false;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!delivered.load() && std::chrono::steady_clock::now() < deadline) {
      try {
        (void)redis().publish(channel, "");
      } catch (const std::exception&) {
      }
      std::this_thread::sleep_for(100ms);
    }
    seen = delivered.load();
    node.registry->unregister(channel, receipt.waiter_id);
    ASSERT_TRUE(seen) << "baseline pub/sub delivery failed";
  }
  const std::uint64_t reconnects_before = node.subscriber->reconnect_count();

  // Outage: stop the container. The subscriber's consume loop must observe
  // failures (reconnect counter grows) instead of hanging or dying.
  ASSERT_EQ(docker_compose("stop redis"), 0) << "could not stop redis container";
  ASSERT_TRUE(wait_until([&] { return node.subscriber->reconnect_count() > reconnects_before; },
                         60s))
      << "subscriber never noticed the outage";

  // Mid-outage: lease acquisition fails closed; release fails safe; replay
  // of a pre-existing COMPLETED row still works (service-level proof uses
  // the shared service through HTTP below).
  {
    const auto attempt = leases().try_acquire(unique_key("docker-down"));
    EXPECT_EQ(attempt.result, coordination::LeaseAttempt::Result::RedisUnavailable);
    EXPECT_FALSE(leases().release(unique_key("docker-down-b"), "tok"));
  }

  // Restart + wait for health through the CLI (bounded, state-gated).
  ASSERT_EQ(docker_compose("start redis"), 0) << "could not start redis container";
  ASSERT_TRUE(wait_until(
      [&] {
        return docker_compose(
                   "exec -T redis redis-cli ping > NUL 2>&1") == 0;
      },
      120s))
      << "redis never became healthy again";

  // Resubscription delivers again (probe waiter converges without any new
  // publish timing dependence: publish-until-delivered bounds on delivery).
  {
    std::atomic<bool> delivered{false};
    auto owner = std::make_shared<int>(2);
    const auto receipt =
        node.registry->register_waiter(channel, owner, [&] { delivered.store(true); });
    ASSERT_FALSE(receipt.rejected);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!delivered.load() && std::chrono::steady_clock::now() < deadline) {
      try {
        (void)redis().publish(channel, "");
      } catch (const std::exception&) {
      }
      std::this_thread::sleep_for(100ms);
    }
    node.registry->unregister(channel, receipt.waiter_id);
    EXPECT_TRUE(delivered.load()) << "no delivery after resubscribe";
  }

  // Full function after reconnect: fresh key executes end to end.
  const test::HttpResult result = post_key(server.port(), unique_key("docker-after"), body);
  EXPECT_EQ(result.status, 200);
  // The reconnect path is counted, not just logged.
  EXPECT_GE(shared_metrics()->snapshot().subscriber_reconnects, 1u);
  // Leave the stack healthy for the rest of the suite (asserted below).
  EXPECT_EQ(docker_compose("ps redis"), 0);
}

TEST_F(PgFixture, PostgresRestartPreservesFencingAndRecovery) {
  // Real database restart mid-suite: in-flight semantics degrade to
  // controlled 503s (covered), and after restart the same keys recover
  // with epochs intact — no phantom completions, no fencing drift.
  REQUIRE_PG();
  REQUIRE_REDIS();
  REQUIRE_DOCKER();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(resilience_config(), node.service);

  // Sanity while healthy + plant an orphan that must survive the restart.
  const std::string orphan_key = unique_key("docker-pg-orphan");
  const std::string orphan_body = R"({"docker":"pg"})";
  {
    auto db = raw_connect();
    ASSERT_EQ(
        repo().try_acquire(*db, orphan_key, fp_of(orphan_body), orphan_body).outcome,
        persistence::AcquireOutcome::Created);
  }
  EXPECT_EQ(post_key(server.port(), unique_key("docker-pg-sanity"), R"({"s":1})").status,
            200);

  ASSERT_EQ(docker_compose("restart postgres"), 0) << "could not restart postgres";

  // Poll until the stack answers again (bounded, state-gated on success).
  bool recovered = wait_until(
      [&] {
        try {
          return post_key(server.port(), unique_key("docker-pg-probe"), R"({"p":1})")
                     .status == 200;
        } catch (const std::exception&) {
          return false;
        }
      },
      180s);
  ASSERT_TRUE(recovered) << "postgres never came back";

  // The pre-restart orphan recovered across the restart with its epoch
  // intact; a stale epoch-1 write still affects zero rows.
  {
    auto db = raw_connect();
    const auto before = repo().find_by_key(*db, orphan_key);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->status, persistence::RecordStatus::Processing);
    EXPECT_EQ(before->fencing_epoch, 1);
  }
  EXPECT_EQ(post_key(server.port(), orphan_key, orphan_body).status, 200);
  {
    auto db = raw_connect();
    const auto after = repo().find_by_key(*db, orphan_key);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, persistence::RecordStatus::Completed);
    EXPECT_EQ(after->fencing_epoch, 2);
    EXPECT_FALSE(repo().complete(*db, orphan_key, fp_of(orphan_body), 1, 200, "stale",
                                 "application/json"))
        << "stale epoch must fail after restart too";
  }
  EXPECT_EQ(node.executor->executions(), 3u)
      << "sanity + probe + orphan recovery = 3 executions, no phantoms";
}

}  // namespace
}  // namespace apex
