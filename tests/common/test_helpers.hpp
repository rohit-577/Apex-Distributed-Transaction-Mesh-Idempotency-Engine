#pragma once

// Shared test utilities.
//
// EnvGuard: sets an environment variable for the lifetime of the object and
// restores the previous value (or absence) on destruction. Tests that mutate
// the environment must not leak state into other tests.
// TestServer: RAII wrapper that starts a real HttpServer on an OS-assigned
// port and joins its I/O thread on destruction, so fixtures stay
// exception-safe even when an assertion fails. An optional durable-service
// wiring enables POST /v1/operations against a real database; without it
// the operations route honestly answers 503.
// acquire_closed_port: binds an ephemeral port and closes it again, yielding
// a port that is (barring a pathological race) guaranteed to refuse
// connections — the deterministic way to test "dependency down".

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/http/verb.hpp>

#include "config/Config.hpp"
#include "execution/HttpServer.hpp"

namespace apex::persistence {
class ConnectionPool;
}

namespace apex::idempotency {
class IdempotencyService;
}

namespace apex::test {

class EnvGuard {
 public:
  EnvGuard(const char* name, const char* value);
  ~EnvGuard();

  EnvGuard(const EnvGuard&) = delete;
  EnvGuard& operator=(const EnvGuard&) = delete;

 private:
  std::string name_;
  std::string old_value_;
  bool had_old_{false};
};

[[nodiscard]] std::uint16_t acquire_closed_port();

// Minimal blocking HTTP client for tests. Opens a fresh connection per call,
// sends one request, reads one response. Not performance-sensitive by
// design — determinism matters here, not throughput.
struct HttpResult {
  int status{0};
  std::string body;
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] HttpResult http_send_with_headers(const std::string& host, std::uint16_t port,
                                                boost::beast::http::verb method,
                                                const std::string& target, const std::string& body,
                                                const HttpHeaders& headers);

[[nodiscard]] inline HttpResult http_send(const std::string& host, std::uint16_t port,
                                          boost::beast::http::verb method,
                                          const std::string& target,
                                          const std::string& body = "") {
  return http_send_with_headers(host, port, method, target, body, {});
}

// libpq connection string for PostgreSQL-gated tests, from
// APEX_TEST_POSTGRES_CONN. Empty when unset: gated tests must GTEST_SKIP()
// instead of failing (ctest stays hermetic without Docker).
[[nodiscard]] std::optional<std::string> pg_test_conninfo();

// Redis endpoint for Redis-gated tests, from APEX_TEST_REDIS_HOST (required)
// and APEX_TEST_REDIS_PORT (default 6379). nullopt when the host is unset:
// same skip contract as PostgreSQL.
struct RedisTestEndpoint {
  std::string host;
  std::uint16_t port{6379};
};

[[nodiscard]] std::optional<RedisTestEndpoint> redis_test_endpoint();

class TestServer {
 public:
  explicit TestServer(config::Config config);
  // Durable-service wiring for POST /v1/operations tests. The service keeps
  // its pool alive (shared ownership); the caller must still destroy the
  // TestServer before its own pool/service handles so db_pool_ drains first.
  TestServer(config::Config config, std::shared_ptr<idempotency::IdempotencyService> service);
  ~TestServer();

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return server_.port(); }

 private:
  boost::asio::io_context ioc_;
  // Database worker threads for the wired service (empty when unwired).
  // Declared before server_: destroyed after it (reverse order), so no
  // Session outlives the pool it posts to.
  std::unique_ptr<boost::asio::thread_pool> db_pool_;
  execution::HttpServer server_;
  std::thread thread_;
};

}  // namespace apex::test
