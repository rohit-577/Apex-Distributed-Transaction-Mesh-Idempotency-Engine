// Cross-node multiplexing tests (P3-14/15): two logical server instances
// (separate io_context, server, service, registry — sharing only
// PostgreSQL + Redis, like two processes) converge on one execution.
// P3-14 proves the pub/sub wake path carries convergence; P3-15 proves the
// durable fallback carries it when the subscriber is deaf.

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/WaiterRegistry.hpp"

namespace apex {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config node_config() {
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

std::string channel_of(const std::string& key, const std::string& body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return idempotency::WaiterRegistry::channel_for(key, fp.hex);
}

template <typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds bound = 15000ms) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

idempotency::WaiterOptions slow_fallback_options() {
  // Fallback interval far longer than the test budget: convergence proves
  // the pub/sub wake carried it (the fallback could not have fired).
  idempotency::WaiterOptions options;
  options.timeout_ms = 40000;
  options.recheck_ms = 20000;
  options.max_waiters_per_key = 1024;
  return options;
}

TEST_F(PgFixture, CrossNodeDuplicatesConvergeViaPubSub) {
  // P3-14: owner executes on node A (gated), waiter suspends on node B
  // whose subscriber is live. Convergence in ~milliseconds with a 20 s
  // fallback interval proves Redis notification delivery (INV-MUX-04: the
  // message only woke; PostgreSQL decided).
  REQUIRE_PG();
  REQUIRE_REDIS();
  auto shared_executor = std::make_shared<GatedExecutor>(/*start_open=*/false);
  GateOpener guard(*shared_executor);

  NodeBundle node_a = make_node_with_executor(shared_executor, slow_fallback_options());
  test::TestServer server_a(node_config(), node_a.service);

  NodeBundle node_b = make_node_with_executor(shared_executor, slow_fallback_options());
  node_b.subscriber->start();
  test::TestServer server_b(node_config(), node_b.service);

  const std::string key = unique_key("xnode");
  const std::string body = R"({"xnode":14})";
  const std::string channel = channel_of(key, body);

  int owner_status = 0;
  std::string owner_body;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server_a.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return shared_executor->executions() == 1; }));

  int waiter_status = 0;
  std::string waiter_body;
  std::thread waiter([&] {
    const test::HttpResult result = post_key(server_b.port(), key, body);
    waiter_status = result.status;
    waiter_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node_b.registry->waiter_count(channel) == 1; }))
      << "node-B waiter never registered";

  const auto start = std::chrono::steady_clock::now();
  shared_executor->open_gate();
  owner.join();
  waiter.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(owner_status, 200);
  EXPECT_EQ(waiter_status, 200);
  EXPECT_EQ(waiter_body, owner_body) << "cross-node divergence (INV-MUX-08)";
  EXPECT_EQ(shared_executor->executions(), 1u) << "executed more than once";
  // B's fallback (20 s) cannot have fired: the wake-up came from pub/sub.
  EXPECT_LT(elapsed, 15s) << "converged too slowly — fallback, not notification?";
  EXPECT_EQ(node_b.registry->waiter_count(channel), 0u);
}

TEST_F(PgFixture, CrossNodeMissedNotificationStillConverges) {
  // P3-15: node B's subscriber stays STOPPED (every notification lost, by
  // construction). The waiter converges purely through the durable fallback
  // re-check — slower, but byte-identical and single-execution (INV-MUX-05).
  REQUIRE_PG();
  REQUIRE_REDIS();
  auto shared_executor = std::make_shared<GatedExecutor>(/*start_open=*/false);
  GateOpener guard(*shared_executor);

  NodeBundle node_a = make_node_with_executor(shared_executor, slow_fallback_options());
  test::TestServer server_a(node_config(), node_a.service);

  // Node B: subscriber deliberately never started; short fallback so the
  // test converges promptly via re-checks alone.
  NodeBundle node_b = make_node_with_executor(shared_executor, PgFixture::test_waiter_options());
  test::TestServer server_b(node_config(), node_b.service);

  const std::string key = unique_key("xnode-missed");
  const std::string body = R"({"xnode":15})";
  const std::string channel = channel_of(key, body);

  int owner_status = 0;
  std::string owner_body;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server_a.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return shared_executor->executions() == 1; }));

  int waiter_status = 0;
  std::string waiter_body;
  std::thread waiter([&] {
    const test::HttpResult result = post_key(server_b.port(), key, body);
    waiter_status = result.status;
    waiter_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node_b.registry->waiter_count(channel) == 1; }));

  shared_executor->open_gate();
  owner.join();
  waiter.join();

  EXPECT_EQ(owner_status, 200);
  EXPECT_EQ(waiter_status, 200);
  EXPECT_EQ(waiter_body, owner_body);
  EXPECT_EQ(shared_executor->executions(), 1u);
}

}  // namespace
}  // namespace apex
