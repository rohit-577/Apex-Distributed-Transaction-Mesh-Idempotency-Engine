// Multiplexing fan-in tests (P3-01/02/03): N identical concurrent requests
// converge on ONE logical execution with identical responses. Deterministic
// by construction: the owner is gated mid-execution while duplicates arrive
// (all observe PROCESSING + held lease => all wait), waiter registration is
// asserted via the registry, then the gate opens and everyone converges.
// No sleeps: barriers (latch), state polling with deadlines (executions,
// waiter counts), and scope-guarded gates.

#include <atomic>
#include <chrono>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Metrics.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex {
namespace {

using test::GateOpener;
using test::GatedExecutor;
using test::PgFixture;
using namespace std::chrono_literals;

config::Config mux_config() {
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

// State-gated waits (bounded by infrastructure health, never by timing):
// the polled condition WILL become true on a correct implementation.
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

TEST_F(PgFixture, TwoWayDuplicateConvergesOnOneExecution) {
  // P3-01: 1 owner + 1 waiter => 1 execution, 2 identical 200s.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);
  const std::string key = unique_key("mux2");
  const std::string body = R"({"mux":2})";
  const std::string channel = channel_of(key, body);

  std::string owner_body;
  int owner_status = 0;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  std::string waiter_body;
  int waiter_status = 0;
  std::thread waiter([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    waiter_status = result.status;
    waiter_body = result.body;
  });
  ASSERT_TRUE(wait_until(
      [&] { return node.registry->waiter_count(channel) == 1; }, 15000ms))
      << "waiter never registered (replay/202 instead of waiting?)";

  node.executor->open_gate();
  owner.join();
  waiter.join();

  EXPECT_EQ(owner_status, 200);
  EXPECT_EQ(waiter_status, 200);
  EXPECT_EQ(waiter_body, owner_body);
  EXPECT_EQ(node.executor->executions(), 1u) << "INV-MUX-01 violated";
  EXPECT_EQ(node.registry->waiter_count(channel), 0u) << "waiter leaked";
}

TEST_F(PgFixture, FiftyWayDuplicateConvergesOnOneExecution) {
  // P3-02: 1 owner + 49 waiters. All 50 converge on the single execution's
  // byte-identical result (INV-MUX-08).
  REQUIRE_PG();
  REQUIRE_REDIS();
  constexpr int kTotal = 50;
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);
  const std::string key = unique_key("mux50");
  const std::string body = R"({"mux":50})";
  const std::string channel = channel_of(key, body);

  int owner_status = 0;
  std::string owner_body;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  std::vector<int> statuses(kTotal - 1, 0);
  std::vector<std::string> bodies(kTotal - 1);
  std::latch go{1};
  std::vector<std::thread> threads;
  for (int i = 0; i < kTotal - 1; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      const test::HttpResult result = post_key(server.port(), key, body);
      statuses[i] = result.status;
      bodies[i] = result.body;
    });
  }
  go.count_down();
  ASSERT_TRUE(wait_until([&] {
    return node.registry->waiter_count(channel) == static_cast<std::size_t>(kTotal - 1);
  })) << "not all duplicates became waiters";

  node.executor->open_gate();
  owner.join();
  for (std::thread& t : threads) {
    t.join();
  }

  EXPECT_EQ(owner_status, 200);
  for (int i = 0; i < kTotal - 1; ++i) {
    EXPECT_EQ(statuses[i], 200) << "waiter " << i;
    EXPECT_EQ(bodies[i], owner_body) << "waiter " << i << " diverged (INV-MUX-08)";
  }
  EXPECT_EQ(node.executor->executions(), 1u) << "INV-MUX-01 violated";
  EXPECT_EQ(node.registry->waiter_count(channel), 0u) << "waiters leaked";
}

TEST_F(PgFixture, HundredWayDuplicateConvergesOnOneExecution) {
  // P3-03: 1 owner + 99 waiters. Same convergence proof at the phase's
  // headline scale.
  REQUIRE_PG();
  REQUIRE_REDIS();
  constexpr int kTotal = 100;
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);
  const std::string key = unique_key("mux100");
  const std::string body = R"({"mux":100})";
  const std::string channel = channel_of(key, body);

  int owner_status = 0;
  std::string owner_body;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  std::vector<int> statuses(kTotal - 1, 0);
  std::vector<std::string> bodies(kTotal - 1);
  std::latch go{1};
  std::vector<std::thread> threads;
  for (int i = 0; i < kTotal - 1; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      const test::HttpResult result = post_key(server.port(), key, body);
      statuses[i] = result.status;
      bodies[i] = result.body;
    });
  }
  go.count_down();
  ASSERT_TRUE(wait_until([&] {
    return node.registry->waiter_count(channel) == static_cast<std::size_t>(kTotal - 1);
  })) << "not all duplicates became waiters";

  node.executor->open_gate();
  owner.join();
  for (std::thread& t : threads) {
    t.join();
  }

  EXPECT_EQ(owner_status, 200);
  for (int i = 0; i < kTotal - 1; ++i) {
    EXPECT_EQ(statuses[i], 200) << "waiter " << i;
    EXPECT_EQ(bodies[i], owner_body) << "waiter " << i << " diverged (INV-MUX-08)";
  }
  EXPECT_EQ(node.executor->executions(), 1u) << "INV-MUX-01 violated";
  EXPECT_EQ(node.registry->waiter_count(channel), 0u) << "waiters leaked";
}

TEST_F(PgFixture, WaiterContinuationsStaySerializedUnderTimerChurn) {
  // Session-serialization regression: every continuation of one connection
  // (timer expiry, registry wake, recheck completion) must be mutually
  // exclusive — concurrent continuations could double-settle a response,
  // corrupt the Beast stream, and unbalance the active gauge. 32 waiters
  // churn through repeated 100 ms fallback-timer cycles (expiry -> durable
  // re-check -> re-register -> re-arm) while the owner is gated, then all
  // converge at once. Exactly-once completions, identical bodies, zero
  // leaked slots, and a balanced gauge prove serialization held throughout
  // the churn. State-gated throughout (fallback-wakeup count, waiter count),
  // never time-based.
  REQUIRE_PG();
  REQUIRE_REDIS();
  constexpr int kWaiters = 32;
  idempotency::WaiterOptions options = test_waiter_options();
  options.timeout_ms = 30000;  // Churn phase must never hit the waiter deadline.
  NodeBundle node = make_node_with_options(options, /*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);
  const std::string key = unique_key("mux-churn");
  const std::string body = R"({"mux":"churn"})";
  const std::string channel = channel_of(key, body);

  std::string owner_body;
  int owner_status = 0;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  // True metric baseline: snapshot before any waiter exists (nothing of this
  // test suspended yet), so later deltas isolate exactly these waiters.
  const auto before = node.service->metrics()->snapshot();

  std::vector<int> statuses(kWaiters, 0);
  std::vector<std::string> bodies(kWaiters);
  std::latch go{1};
  std::vector<std::thread> threads;
  for (int i = 0; i < kWaiters; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      const test::HttpResult result = post_key(server.port(), key, body);
      statuses[i] = result.status;
      bodies[i] = result.body;
    });
  }
  go.count_down();
  ASSERT_TRUE(wait_until([&] {
    return node.registry->waiter_count(channel) == static_cast<std::size_t>(kWaiters);
  })) << "not all duplicates became waiters";

  // Require every waiter to be observably suspended (gauge fully armed)
  // before churning. Registration alone does not arm the gauge — the first
  // durable re-check must return on the session strand first.
  ASSERT_TRUE(wait_until([&] {
    return node.service->metrics()->snapshot().waiters_active ==
           before.waiters_active + static_cast<std::uint64_t>(kWaiters);
  })) << "not all waiters reached suspended state";
  // Churn: every waiter fires multiple fallback timers before the owner
  // completes. The count WILL advance on a live system; the deadline fails
  // loudly if waiters are stuck.
  ASSERT_TRUE(wait_until(
      [&] {
        return node.service->metrics()->snapshot().fallback_wakeups - before.fallback_wakeups >=
               static_cast<std::uint64_t>(2 * kWaiters);
      },
      30000ms))
      << "fallback timers never cycled (waiters stuck?)";

  node.executor->open_gate();
  owner.join();
  for (std::thread& t : threads) {
    t.join();
  }

  EXPECT_EQ(owner_status, 200);
  for (int i = 0; i < kWaiters; ++i) {
    EXPECT_EQ(statuses[i], 200) << "waiter " << i;
    EXPECT_EQ(bodies[i], owner_body) << "waiter " << i << " diverged";
  }
  EXPECT_EQ(node.executor->executions(), 1u) << "INV-MUX-01 violated";
  EXPECT_EQ(node.registry->waiter_count(channel), 0u) << "waiters leaked";
  const auto after = node.service->metrics()->snapshot();
  EXPECT_EQ(after.waiter_completions - before.waiter_completions,
            static_cast<std::uint64_t>(kWaiters))
      << "each waiter must converge exactly once (double-settle?)";
  EXPECT_EQ(after.waiters_active, before.waiters_active) << "active gauge unbalanced";
}

TEST_F(PgFixture, ManyKeysExecuteOnceEachWithoutInterference) {
  // P3-16: 8 keys x 8 simultaneous duplicates. Each key executes exactly
  // once (8 total), keys never block each other, every contender converges.
  REQUIRE_PG();
  REQUIRE_REDIS();
  constexpr int kKeys = 8;
  constexpr int kPerKey = 8;
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);

  std::vector<std::string> keys;
  for (int k = 0; k < kKeys; ++k) {
    keys.push_back(unique_key("mux-multi-" + std::to_string(k)));
  }

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(kKeys * kPerKey, 0);
  for (int k = 0; k < kKeys; ++k) {
    for (int i = 0; i < kPerKey; ++i) {
      const int slot = k * kPerKey + i;
      threads.emplace_back([&, slot, k] {
        go.wait();
        statuses[slot] = post_key(server.port(), keys[k], R"({"k":1})").status;
      });
    }
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  for (int status : statuses) {
    EXPECT_TRUE(status == 200 || status == 202) << "status " << status << " escaped";
  }
  EXPECT_EQ(node.executor->executions(), static_cast<std::uint64_t>(kKeys))
      << "one execution per key, no more (INV-MUX-01)";
  for (const std::string& key : keys) {
    EXPECT_EQ(post_key(server.port(), key, R"({"k":1})").status, 200) << key;
  }
}

TEST_F(PgFixture, MixedFingerprintsExecuteOnlyTheWinner) {
  // P3-17 (INV-MUX-09 with counts): 3 fingerprints x 5 contenders. Exactly
  // one fingerprint executes (total count 1); every other contender sees
  // 409 or a pre-row 202 — never an execution.
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/true);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);
  const std::string key = unique_key("mux-mixed");
  const std::vector<std::string> bodies{R"({"side":"a"})", R"({"side":"b"})",
                                        R"({"side":"c"})"};
  constexpr int kPerBody = 5;

  std::latch go{1};
  std::vector<std::thread> threads;
  std::vector<int> statuses(bodies.size() * kPerBody, 0);
  for (std::size_t b = 0; b < bodies.size(); ++b) {
    for (int i = 0; i < kPerBody; ++i) {
      const int slot = static_cast<int>(b * kPerBody + i);
      threads.emplace_back([&, slot, b] {
        go.wait();
        statuses[slot] = post_key(server.port(), key, bodies[b]).status;
      });
    }
  }
  go.count_down();
  for (std::thread& t : threads) {
    t.join();
  }

  int bodies_with_200 = 0;
  for (std::size_t b = 0; b < bodies.size(); ++b) {
    bool any_200 = false;
    for (int i = 0; i < kPerBody; ++i) {
      const int status = statuses[b * kPerBody + i];
      EXPECT_TRUE(status == 200 || status == 202 || status == 409)
          << "status " << status << " escaped";
      any_200 = any_200 || status == 200;
    }
    bodies_with_200 += any_200 ? 1 : 0;
  }
  EXPECT_EQ(bodies_with_200, 1) << "exactly one fingerprint may execute";
  EXPECT_EQ(node.executor->executions(), 1u) << "conflicts must not execute (INV-MUX-09)";
}

TEST_F(PgFixture, DisconnectedWaitersChangeNothingDurable) {
  // Cancellation: 1 gated owner + 50 waiting clients + 50 clients that
  // connect, send, and vanish mid-wait. Disconnecting removes only the HTTP
  // waiter — never the owner, lease, epoch, or row. The surviving 50 get the
  // identical final result from exactly one execution, and the registry ends
  // empty (sessions release their slots on death).
  REQUIRE_PG();
  REQUIRE_REDIS();
  NodeBundle node = make_node(/*gate_open=*/false);
  GateOpener guard(*node.executor);
  test::TestServer server(mux_config(), node.service);
  const std::string key = unique_key("mux-cancel");
  const std::string body = R"({"mux":"cancel"})";
  const std::string channel = channel_of(key, body);

  int owner_status = 0;
  std::string owner_body;
  std::thread owner([&] {
    const test::HttpResult result = post_key(server.port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  ASSERT_TRUE(wait_until([&] { return node.executor->executions() == 1; }));

  // 50 well-behaved waiters, all suspended before the chaos half starts
  // (count==50 exactly: nothing else has been sent yet).
  constexpr int kKeepers = 50;
  std::vector<int> keeper_statuses(kKeepers, 0);
  std::vector<std::string> keeper_bodies(kKeepers);
  std::vector<std::thread> keepers;
  for (int i = 0; i < kKeepers; ++i) {
    keepers.emplace_back([&, i] {
      const test::HttpResult result = post_key(server.port(), key, body);
      keeper_statuses[i] = result.status;
      keeper_bodies[i] = result.body;
    });
  }
  ASSERT_TRUE(wait_until([&] {
    return node.registry->waiter_count(channel) == static_cast<std::size_t>(kKeepers);
  })) << "keepers never all suspended";

  // 50 abrupt disconnects: full request bytes, then close without reading.
  // Raw sockets (not the shared helper) because the helper always reads.
  constexpr int kDroppers = 50;
  std::vector<std::thread> droppers;
  for (int i = 0; i < kDroppers; ++i) {
    droppers.emplace_back([&] {
      try {
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        asio::io_context ioc;
        asio::ip::tcp::socket socket(ioc);
        socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
        http::request<http::string_body> req{http::verb::post, "/v1/operations", 11};
        req.set(http::field::host, "127.0.0.1");
        req.set("Idempotency-Key", key);
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
        http::write(socket, req);
        boost::system::error_code ec;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);
      } catch (const std::exception&) {
      }
    });
  }
  for (std::thread& t : droppers) {
    t.join();
  }

  node.executor->open_gate();
  owner.join();
  for (std::thread& t : keepers) {
    t.join();
  }

  EXPECT_EQ(owner_status, 200);
  for (int i = 0; i < kKeepers; ++i) {
    EXPECT_EQ(keeper_statuses[i], 200) << "keeper " << i;
    EXPECT_EQ(keeper_bodies[i], owner_body) << "keeper " << i << " diverged";
  }
  EXPECT_EQ(node.executor->executions(), 1u) << "one execution despite 101 contenders";
  {
    auto db = raw_connect();
    const auto record = repo().find_by_key(*db, key);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, persistence::RecordStatus::Completed);
    EXPECT_EQ(record->fencing_epoch, 1);
  }
  EXPECT_TRUE(wait_until([&] { return node.registry->waiter_count(channel) == 0; }))
      << "dead-client slots leaked";
}

}  // namespace
}  // namespace apex
