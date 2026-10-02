// HTTP integration tests: a real HttpServer on an OS-assigned port, driven
// by a real Beast client. These prove the gateway accepts connections,
// routes correctly, and survives malformed input.

#include <string>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/test_helpers.hpp"
#include "config/Config.hpp"

namespace apex {
namespace {

namespace http = boost::beast::http;

config::Config test_config() {
  config::Config cfg = config::Config::defaults();
  cfg.port = 0;  // OS-assigned; the test reads back the bound port.
  cfg.threads = 2;
  // Point dependencies at ports nothing listens on so /ready is
  // deterministically 503 without needing Docker.
  cfg.postgres_port = test::acquire_closed_port();
  cfg.redis_port = test::acquire_closed_port();
  return cfg;
}

TEST(HttpIntegrationTest, HealthReturns200WithJsonBody) {
  test::TestServer server(test_config());
  const test::HttpResult result =
      test::http_send("127.0.0.1", server.port(), http::verb::get, "/health");
  EXPECT_EQ(result.status, 200);
  const nlohmann::json body = nlohmann::json::parse(result.body);
  EXPECT_EQ(body.at("status").get<std::string>(), "ok");
  EXPECT_EQ(body.at("service").get<std::string>(), "apex");
}

TEST(HttpIntegrationTest, UnknownPathReturns404Json) {
  test::TestServer server(test_config());
  const test::HttpResult result =
      test::http_send("127.0.0.1", server.port(), http::verb::get, "/does-not-exist");
  EXPECT_EQ(result.status, 404);
  EXPECT_NE(result.body.find("not_found"), std::string::npos);
}

TEST(HttpIntegrationTest, OperationsEndpointIsExplicitlyUnimplemented) {
  test::TestServer server(test_config());
  const test::HttpResult result = test::http_send("127.0.0.1", server.port(), http::verb::post,
                                                  "/v1/operations", R"({"op":"write"})");
  EXPECT_EQ(result.status, 501);
  EXPECT_NE(result.body.find("not_implemented"), std::string::npos);
}

TEST(HttpIntegrationTest, ReadyReports503WhenDependenciesAreDown) {
  test::TestServer server(test_config());
  const test::HttpResult result =
      test::http_send("127.0.0.1", server.port(), http::verb::get, "/ready");
  EXPECT_EQ(result.status, 503);
  const nlohmann::json body = nlohmann::json::parse(result.body);
  EXPECT_EQ(body.at("status").get<std::string>(), "not_ready");
  EXPECT_FALSE(body.at("postgres_reachable").get<bool>());
  EXPECT_FALSE(body.at("redis_reachable").get<bool>());
}

TEST(HttpIntegrationTest, ReadyReports200WhenDependenciesListen) {
  // Two bare listeners stand in for PostgreSQL and Redis. The gateway only
  // checks TCP reachability in Phase 0, so a listening socket is a faithful
  // double — the kernel completes the handshake from the backlog queue.
  namespace asio = boost::asio;
  asio::io_context ioc;
  asio::ip::tcp::acceptor fake_pg(ioc);
  fake_pg.open(asio::ip::tcp::v4());
  fake_pg.bind({asio::ip::tcp::v4(), 0});
  fake_pg.listen();
  asio::ip::tcp::acceptor fake_redis(ioc);
  fake_redis.open(asio::ip::tcp::v4());
  fake_redis.bind({asio::ip::tcp::v4(), 0});
  fake_redis.listen();

  config::Config cfg = test_config();
  cfg.postgres_port = fake_pg.local_endpoint().port();
  cfg.redis_port = fake_redis.local_endpoint().port();
  test::TestServer server(cfg);

  const test::HttpResult result =
      test::http_send("127.0.0.1", server.port(), http::verb::get, "/ready");
  EXPECT_EQ(result.status, 200);
  const nlohmann::json body = nlohmann::json::parse(result.body);
  EXPECT_EQ(body.at("status").get<std::string>(), "ready");
  EXPECT_TRUE(body.at("postgres_reachable").get<bool>());
  EXPECT_TRUE(body.at("redis_reachable").get<bool>());
}

TEST(HttpIntegrationTest, MalformedBytesGetControlled400AndServerSurvives) {
  test::TestServer server(test_config());

  namespace asio = boost::asio;
  asio::io_context ioc;
  asio::ip::tcp::socket raw(ioc);
  raw.connect({asio::ip::make_address("127.0.0.1"), server.port()});
  const std::string garbage = "THIS IS NOT HTTP\r\n\r\n";
  asio::write(raw, asio::buffer(garbage));

  // The server answers 400 and then closes; either way the connection must
  // terminate instead of hanging or crashing the process.
  std::string reply;
  boost::system::error_code ec;
  for (;;) {
    char chunk[512]{};
    const std::size_t n = raw.read_some(asio::buffer(chunk), ec);
    if (n > 0) {
      reply.append(chunk, n);
    }
    if (ec) {
      break;
    }
  }
  EXPECT_NE(reply.find("400"), std::string::npos) << reply;

  // The server is still alive for the next connection.
  const test::HttpResult after =
      test::http_send("127.0.0.1", server.port(), http::verb::get, "/health");
  EXPECT_EQ(after.status, 200);
}

TEST(HttpIntegrationTest, KeepAliveServesMultipleRequestsPerConnection) {
  test::TestServer server(test_config());

  namespace asio = boost::asio;
  namespace beast = boost::beast;
  asio::io_context ioc;
  beast::tcp_stream stream(ioc);
  stream.connect(asio::ip::tcp::resolver(ioc).resolve("127.0.0.1",
                                                      std::to_string(server.port())));

  for (int i = 0; i < 3; ++i) {
    http::request<http::empty_body> req{http::verb::get, "/health", 11};
    req.set(http::field::host, "127.0.0.1");
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    EXPECT_EQ(res.result(), http::status::ok);
  }
}

}  // namespace
}  // namespace apex
