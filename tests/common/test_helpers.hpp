#pragma once

// Shared test utilities. Header-only by design so every test executable can
// use them without an extra library target.
//
// EnvGuard: sets an environment variable for the lifetime of the object and
// restores the previous value (or absence) on destruction. Tests that mutate
// the environment must not leak state into other tests.
// TestServer: RAII wrapper that starts a real HttpServer on an OS-assigned
// port and joins its I/O thread on destruction, so fixtures stay
// exception-safe even when an assertion fails.
// acquire_closed_port: binds an ephemeral port and closes it again, yielding
// a port that is (barring a pathological race) guaranteed to refuse
// connections — the deterministic way to test "dependency down".

#include <cstdint>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/beast/http/verb.hpp>

#include "config/Config.hpp"
#include "execution/HttpServer.hpp"

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

[[nodiscard]] HttpResult http_send(const std::string& host, std::uint16_t port,
                                   boost::beast::http::verb method, const std::string& target,
                                   const std::string& body = "");

class TestServer {
 public:
  explicit TestServer(config::Config config);
  ~TestServer();

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return server_.port(); }

 private:
  boost::asio::io_context ioc_;
  execution::HttpServer server_;
  std::thread thread_;
};

}  // namespace apex::test
