// Observability tests (Phase 5): correlation-ID echo/validation, metrics
// funnel deltas through real traffic, /metrics endpoint shape, and bounded
// cardinality (unique keys never appear in rendered output). Deterministic:
// deltas on the fixture-shared counters (fresh per test process), no timing.

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/LeaseManager.hpp"
#include "idempotency/CorrelationId.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"

namespace apex {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config obs_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 8;
  cfg.db_pool_size = 16;
  return cfg;
}

test::HttpResult post_ops(std::uint16_t port, const std::string& key, const std::string& body,
                          const std::string& correlation = "") {
  test::HttpHeaders headers{{"Idempotency-Key", key}, {"Content-Type", "application/json"}};
  if (!correlation.empty()) {
    headers.emplace_back("X-Request-ID", correlation);
  }
  return test::http_send_with_headers("127.0.0.1", port, boost::beast::http::verb::post,
                                      "/v1/operations", body, headers);
}

TEST(CorrelationIdTest, ValidationAcceptsWellFormedIds) {
  EXPECT_TRUE(idempotency::validate_correlation_id("abc-123_XYZ").has_value());
  EXPECT_EQ(*idempotency::validate_correlation_id("r-1"), "r-1");
}

TEST(CorrelationIdTest, ValidationRejectsGarbage) {
  EXPECT_FALSE(idempotency::validate_correlation_id("").has_value());
  EXPECT_FALSE(idempotency::validate_correlation_id("has space").has_value());
  EXPECT_FALSE(idempotency::validate_correlation_id("semi;colon").has_value());
  EXPECT_FALSE(idempotency::validate_correlation_id(std::string(65, 'k')).has_value());
  EXPECT_FALSE(idempotency::validate_correlation_id("uni→code").has_value());
}

TEST(CorrelationIdTest, MintedIdsAreValidAndUnique) {
  const std::string first = idempotency::new_correlation_id();
  EXPECT_EQ(first.size(), 16u);
  EXPECT_TRUE(idempotency::validate_correlation_id(first).has_value());
  EXPECT_NE(idempotency::new_correlation_id(), first);
}

TEST_F(PgFixture, RequestIdEchoedBackToClient) {
  // Valid client IDs echo exactly; invalid/absent ones are replaced with a
  // fresh minted ID (never a 400 — correlation is metadata, not contract).
  REQUIRE_PG();
  REQUIRE_REDIS();
  test::TestServer server(obs_config(), service());
  const std::string key = unique_key("obs-cid");

  {
    // Raw exchange: the shared helper hides response headers, so read them.
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    asio::io_context ioc;
    beast::tcp_stream stream(ioc);
    stream.connect(asio::ip::tcp::resolver(ioc).resolve("127.0.0.1",
                                                         std::to_string(server.port())));
    http::request<http::string_body> req{http::verb::post, "/v1/operations", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set("Idempotency-Key", key);
    req.set("X-Request-ID", "client-trace-9");
    req.body() = R"({"cid":1})";
    req.prepare_payload();
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    EXPECT_EQ(res.result(), http::status::ok);
    const auto echoed = res.find("X-Request-ID");
    ASSERT_TRUE(echoed != res.end());
    EXPECT_EQ(std::string(echoed->value()), "client-trace-9");
  }
  {
    // Invalid client ID => server mints a fresh valid one and echoes that.
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    asio::io_context ioc;
    beast::tcp_stream stream(ioc);
    stream.connect(asio::ip::tcp::resolver(ioc).resolve("127.0.0.1",
                                                         std::to_string(server.port())));
    http::request<http::string_body> req{http::verb::post, "/v1/operations", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set("Idempotency-Key", unique_key("obs-cid-bad"));
    req.set("X-Request-ID", "not valid!!");
    req.body() = R"({"cid":2})";
    req.prepare_payload();
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    EXPECT_EQ(res.result(), http::status::ok);
    const auto echoed = res.find("X-Request-ID");
    ASSERT_TRUE(echoed != res.end());
    const std::string value(echoed->value());
    EXPECT_NE(value, "not valid!!");
    EXPECT_TRUE(idempotency::validate_correlation_id(value).has_value());
  }
}

TEST_F(PgFixture, CorrelationIdNeverEntersTheFingerprint) {
  // Same key + same body with different correlation IDs is ONE logical
  // operation: single execution, identical replay. Confusing the two IDs
  // would break dedup; this test pins the distinction.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(obs_config(), node.service);
  const std::string key = unique_key("obs-cidfp");

  const test::HttpResult first = post_ops(server.port(), key, R"({"c":1})", "attempt-one");
  ASSERT_EQ(first.status, 200);
  const test::HttpResult second = post_ops(server.port(), key, R"({"c":1})", "attempt-two");
  EXPECT_EQ(second.status, 200);
  EXPECT_EQ(second.body, first.body);
  EXPECT_EQ(node.executor->executions(), 1u) << "correlation must not fork execution";
}

TEST_F(PgFixture, MetricsFunnelCountsRealTraffic) {
  // End-to-end counter deltas across the documented funnel: one execution,
  // one replay, one conflict, one validation failure, one failed terminal.
  REQUIRE_PG();
  REQUIRE_REDIS();
  test::TestServer server(obs_config(), service());
  const auto before = shared_metrics()->snapshot();

  const std::string exec_key = unique_key("m-exec");
  EXPECT_EQ(post_ops(server.port(), exec_key, R"({"m":1})").status, 200);
  EXPECT_EQ(post_ops(server.port(), exec_key, R"({"m":1})").status, 200);  // replay
  const std::string conflict_key = unique_key("m-conflict");
  EXPECT_EQ(post_ops(server.port(), conflict_key, R"({"m":1})").status, 200);
  EXPECT_EQ(post_ops(server.port(), conflict_key, R"({"m":2})").status, 409);  // conflict
  const std::string fail_key = unique_key("m-fail");
  EXPECT_EQ(post_ops(server.port(), fail_key, R"({"fail":true})").status, 500);
  EXPECT_EQ(post_ops(server.port(), fail_key, R"({"fail":true})").status, 409);  // failed
  EXPECT_EQ(post_ops(server.port(), unique_key("m-bad"), "nope-json").status, 400);

  const auto after = shared_metrics()->snapshot();
  EXPECT_EQ(after.requests_total - before.requests_total, 7u);
  EXPECT_EQ(after.executions - before.executions, 3u);
  EXPECT_EQ(after.executions_completed - before.executions_completed, 2u);
  EXPECT_EQ(after.executions_failed - before.executions_failed, 1u);
  EXPECT_EQ(after.completed_replays - before.completed_replays, 1u);
  EXPECT_EQ(after.fingerprint_conflicts - before.fingerprint_conflicts, 1u);
  EXPECT_EQ(after.failed_terminal_answers - before.failed_terminal_answers, 1u);
  EXPECT_EQ(after.validation_failures - before.validation_failures, 1u);
}

TEST_F(PgFixture, MetricsEndpointRendersWithoutUserData) {
  // GET /metrics: 200, Prometheus text shape, fixed counters only. The
  // unique keys exercised in THIS process must not appear anywhere in the
  // output (bounded cardinality — a malicious client cannot grow it).
  REQUIRE_PG();
  REQUIRE_REDIS();
  test::TestServer server(obs_config(), service());
  const std::string key = unique_key("m-card");
  EXPECT_EQ(post_ops(server.port(), key, R"({"m":1})").status, 200);

  const test::HttpResult metrics =
      test::http_send("127.0.0.1", server.port(), boost::beast::http::verb::get, "/metrics");
  EXPECT_EQ(metrics.status, 200);
  EXPECT_NE(metrics.body.find("apex_requests_total"), std::string::npos);
  EXPECT_NE(metrics.body.find("apex_executions"), std::string::npos);
  EXPECT_NE(metrics.body.find("apex_lease_acquired"), std::string::npos);
  EXPECT_EQ(metrics.body.find(key), std::string::npos) << "user key leaked into metrics";
  EXPECT_EQ(metrics.body.find("apex-dev-only"), std::string::npos) << "secret leaked";

  const test::HttpResult wrong_method = test::http_send(
      "127.0.0.1", server.port(), boost::beast::http::verb::post, "/metrics");
  EXPECT_EQ(wrong_method.status, 405);
}

std::string fp_of(const std::string& body) {
  const idempotency::Fingerprint fp =
      idempotency::fingerprint_for("POST", "/v1/operations", body);
  if (!fp.ok) {
    throw std::runtime_error("test body is not valid JSON");
  }
  return fp.hex;
}

TEST_F(PgFixture, WaiterConvergenceIsCountedOnce) {
  // One gated owner + one waiter: waiter_completions and wait_time advance
  // exactly once for the convergence, and the active gauge returns to zero.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(obs_config(), node.service);
  const std::string key = unique_key("m-waitconv");
  const std::string body = R"({"m":"wait"})";
  const std::string channel = idempotency::WaiterRegistry::channel_for(key, fp_of(body));

  const auto before = shared_metrics()->snapshot();
  int owner_status = 0;
  std::thread owner([&] {
    owner_status = post_ops(server.port(), key, body).status;
  });
  auto executions_is = [&](std::uint64_t n) { return node.executor->executions() == n; };
  const auto deadline = std::chrono::steady_clock::now() + 15000ms;
  while (!executions_is(1) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(executions_is(1));

  int waiter_status = 0;
  std::string waiter_body;
  std::thread waiter([&] {
    const test::HttpResult result = post_ops(server.port(), key, body);
    waiter_status = result.status;
    waiter_body = result.body;
  });
  const auto reg_deadline = std::chrono::steady_clock::now() + 15000ms;
  while (node.registry->waiter_count(channel) != 1 &&
         std::chrono::steady_clock::now() < reg_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(node.registry->waiter_count(channel), 1u);
  // Active gauge observes the suspended waiter.
  EXPECT_GE(shared_metrics()->snapshot().waiters_active, 1u);

  node.executor->open_gate();
  owner.join();
  waiter.join();
  EXPECT_EQ(owner_status, 200);
  EXPECT_EQ(waiter_status, 200);

  const auto after = shared_metrics()->snapshot();
  EXPECT_EQ(after.waiter_completions - before.waiter_completions, 1u);
  EXPECT_GT(after.wait_time_ms_total - before.wait_time_ms_total, 0u);
  EXPECT_EQ(after.waiters_active, before.waiters_active) << "gauge must return to baseline";
}

TEST_F(PgFixture, WindowADeferralIsCountedSeparately) {
  // Lease held with no durable row (crash window A): immediate 202 counted
  // as a deferral, NOT as a waiter start (nothing suspended).
  REQUIRE_PG();
  REQUIRE_REDIS();
  test::TestServer server(obs_config(), service());
  const std::string key = unique_key("m-window-a");
  const auto held = leases().try_acquire(key);
  ASSERT_EQ(held.result, coordination::LeaseAttempt::Result::Acquired);

  const auto before = shared_metrics()->snapshot();
  const test::HttpResult result = post_ops(server.port(), key, R"({"m":"wina"})");
  EXPECT_EQ(result.status, 202);
  const auto after = shared_metrics()->snapshot();
  EXPECT_EQ(after.deferred_processing_answers - before.deferred_processing_answers, 1u);
  EXPECT_EQ(after.waiters_started - before.waiters_started, 0u);
  EXPECT_TRUE(leases().release(key, held.token));
}

}  // namespace
}  // namespace apex
