// Concurrency baseline: many clients hitting the gateway at once must all
// get correct answers and the process must stay healthy. This is the Phase 0
// seed of tests/concurrency — the multiplexing/dedup races arrive with the
// Phase 1 idempotency engine and its invariants.

#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/test_helpers.hpp"
#include "config/Config.hpp"

namespace apex {
namespace {

namespace http = boost::beast::http;

TEST(ParallelHealthTest, ThirtyTwoConcurrentClientsAllGet200) {
  constexpr int kClients = 32;

  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 4;
  cfg.postgres_port = test::acquire_closed_port();
  cfg.redis_port = test::acquire_closed_port();
  test::TestServer server(cfg);

  std::vector<std::thread> clients;
  std::vector<int> statuses(kClients, 0);
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&, i] {
      statuses[i] =
          test::http_send("127.0.0.1", server.port(), http::verb::get, "/health").status;
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }

  for (int i = 0; i < kClients; ++i) {
    EXPECT_EQ(statuses[i], 200) << "client " << i << " got a wrong status";
  }

  // The server is still fully functional afterwards.
  EXPECT_EQ(test::http_send("127.0.0.1", server.port(), http::verb::get, "/health").status,
            200);
}

TEST(ParallelHealthTest, MixedRoutesUnderConcurrencyStayCorrect) {
  constexpr int kClients = 16;

  config::Config cfg = config::Config::defaults();
  cfg.port = 0;
  cfg.threads = 4;
  cfg.postgres_port = test::acquire_closed_port();
  cfg.redis_port = test::acquire_closed_port();
  test::TestServer server(cfg);

  std::vector<std::thread> clients;
  std::vector<int> statuses(kClients, 0);
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&, i] {
      const std::string target = (i % 2 == 0) ? "/health" : "/no-such-route";
      statuses[i] =
          test::http_send("127.0.0.1", server.port(), http::verb::get, target).status;
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }

  for (int i = 0; i < kClients; ++i) {
    EXPECT_EQ(statuses[i], (i % 2 == 0) ? 200 : 404) << "client " << i;
  }
}

}  // namespace
}  // namespace apex
