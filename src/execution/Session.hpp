#pragma once

// One HTTP connection. Owns its socket and all read/write state; destroys
// itself when the connection ends or an unrecoverable error occurs.
//
// Rules enforced here:
// - No blocking work: request handling is either a pure Router call or an
//   asynchronous DependencyChecker probe.
// - Bounded state: the request body parser rejects payloads over 1 MiB with
//   a controlled 413 instead of growing memory without limit.
// - Controlled errors: unparsable bytes get a 400 response (then the
//   connection closes, because the stream position is no longer reliable);
//   every other failure mode maps to a JSON status, never to a dropped
//   connection or an exception escaping into the I/O loop.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "api/Router.hpp"
#include "config/Config.hpp"

namespace apex::idempotency {
class IdempotencyService;
}  // namespace apex::idempotency

namespace boost::asio {
class thread_pool;
}  // namespace boost::asio

namespace apex::execution {

class Session : public std::enable_shared_from_this<Session> {
 public:
  static constexpr std::size_t kMaxBodyBytes = 1024 * 1024;  // 1 MiB

  // Takes ownership of a connected socket. `version` is reported in JSON
  // bodies; it comes from the APEX_VERSION compile definition.
  // `service`/`db_pool` wire the durable idempotency layer: both null means
  // storage is not configured and POST /v1/operations answers 503. When a
  // service is present, db_pool must outlive every session (owned by main()
  // or the test fixture, joined before destruction).
  static void launch(boost::asio::ip::tcp::socket socket, const config::Config& config,
                     std::string version,
                     std::shared_ptr<idempotency::IdempotencyService> service,
                     boost::asio::thread_pool* db_pool);

 private:
  Session(boost::asio::ip::tcp::socket socket, const config::Config& config,
          std::string version, std::shared_ptr<idempotency::IdempotencyService> service,
          boost::asio::thread_pool* db_pool);

  void do_read();
  void on_read(boost::beast::error_code ec);
  void handle_request();
  void handle_ready(boost::beast::http::verb method, unsigned version, bool keep_alive);
  void handle_operations(unsigned version, bool keep_alive);
  void send_json(int status, const std::string& body, unsigned version, bool keep_alive,
                 const std::string& allow = "");
  void do_write();
  void do_close();

  boost::beast::tcp_stream stream_;
  boost::beast::flat_buffer buffer_;
  std::optional<boost::beast::http::request_parser<boost::beast::http::string_body>> parser_;
  boost::beast::http::request<boost::beast::http::string_body> request_;
  boost::beast::http::response<boost::beast::http::string_body> response_;
  config::Config config_;
  api::Router router_;
  std::shared_ptr<idempotency::IdempotencyService> service_;
  boost::asio::thread_pool* db_pool_;
};

}  // namespace apex::execution
