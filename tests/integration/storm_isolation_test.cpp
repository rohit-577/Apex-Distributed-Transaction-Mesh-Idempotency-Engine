// Storm-isolation tests: redis-plus-plus under connection churn (commands +
// subscriber + registered waiter slots) across a real container restart.
// These bisect failure domains: a crash here would implicate the client
// library or our thin wrapper under reconnect storms, independent of HTTP
// session machinery (which the H benchmark scenario covers end to end).

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"

namespace apex::coordination {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config probe_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 8;
  cfg.db_pool_size = 16;
  return cfg;
}

test::HttpResult probe_post(std::uint16_t port, const std::string& key,
                            const std::string& body) {
  return test::http_send_with_headers("127.0.0.1", port,
                                      boost::beast::http::verb::post, "/v1/operations", body,
                                      {{"Idempotency-Key", key},
                                       {"Content-Type", "application/json"}});
}

template <typename Predicate>
bool probe_wait_until(Predicate pred, std::chrono::milliseconds bound = 15000ms) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

int probe_docker(const std::string& args) {
  return std::system(("docker compose -f \"" + std::string(APEX_COMPOSE_FILE) + "\" " + args)
                         .c_str());
}

TEST_F(PgFixture, StormIsolationSubscriberRestartWithGatedOwner) {
  // Subscriber + restart + gated owner, but NO waiter sessions: isolates
  // the subscriber/restart path from waiter machinery.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  node.subscriber->start();
  test::TestServer server(probe_config(), node.service);
  const std::string key = unique_key("probe-h3");
  const std::string body = R"({"h":3})";

  std::thread owner([&] {
    try {
      (void)probe_post(server.port(), key, body);
    } catch (const std::exception&) {
    }
  });
  EXPECT_TRUE(probe_wait_until([&] { return node.executor->executions() == 1; }));
  EXPECT_EQ(probe_docker("restart redis"), 0);
  // Ride out the restart with the owner gated, then release.
  std::this_thread::sleep_for(15000ms);
  node.executor->open_gate();
  owner.join();
  SUCCEED();
}

TEST_F(PgFixture, StormIsolationRedisStormDuringRestart) {
  REQUIRE_REDIS();
  RedisEndpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = test::redis_test_endpoint()->port;
  endpoint.connect_timeout = 2000ms;
  endpoint.command_timeout = 2000ms;
  endpoint.pool_size = 32;

  // Subscriber running throughout (reconnects during the restart below),
  // plus one registered waiter slot so sweeps have live targets — the H
  // combination minus HTTP sessions. One shared client like production.
  auto shared_client = std::make_shared<RedisClient>(std::move(endpoint));
  RedisClient& client = *shared_client;
  apex::idempotency::WaiterRegistry registry;
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto metrics = std::make_shared<apex::observability::Metrics>();
  apex::coordination::CompletionSubscriber sub(shared_client, registry, quiet, metrics);
  sub.start();
  auto waiter = std::make_shared<int>(1);
  std::weak_ptr<void> weak = waiter;
  const auto receipt = registry.register_waiter("probe-channel", weak, [] {});
  (void)receipt;

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> ops{0};
  std::atomic<std::uint64_t> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 32; ++i) {
    threads.emplace_back([&] {
      while (!stop.load()) {
        try {
          (void)client.get("probe-key");
          ++ops;
        } catch (const RedisError&) {
          ++failures;
        }
      }
    });
  }
  // Storm across a real container restart (bounded 60 s).
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  bool restarted = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5s);
    if (!restarted) {
      restarted = true;
      const int rc = std::system("docker compose -f \"" APEX_COMPOSE_FILE
                                "\" restart redis > NUL 2>&1");
      EXPECT_EQ(rc, 0) << "could not restart redis for the probe";
    }
    if (ops.load() > 2000 && failures.load() > 10) {
      break;  // Seen both traffic and failure handling: enough signal.
    }
  }
  stop.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_GT(ops.load(), 0u);
  // No crash = pass. Failure counts are environmental (restart timing).
  SUCCEED();
}

}  // namespace
}  // namespace apex::coordination
