#pragma once

// Asynchronous HTTP accept loop. RAII over the acceptor socket:
//
// - start() binds, listens, and posts the first async_accept. Bind failures
//   throw boost::system::system_error so main() can report them and exit.
// - stop() cancels the acceptor; in-flight sessions drain on their own and
//   destroy themselves. stop() is idempotent and safe to call from a signal
//   handler dispatch (it only touches the acceptor and a flag).
// - The io_context and its threads are owned by main(), not by this class,
//   so shutdown sequencing (stop acceptor -> stop context -> join threads)
//   is explicit and testable.

#include <cstdint>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "config/Config.hpp"
#include "observability/Logger.hpp"

namespace apex::idempotency {
class IdempotencyService;
}  // namespace apex::idempotency

namespace boost::asio {
class thread_pool;
}  // namespace boost::asio

namespace apex::execution {

class HttpServer {
 public:
  // `service`/`db_pool` wire POST /v1/operations to durable storage (both
  // null = storage unwired, operations answer 503). `logger` emits waiter
  // lifecycle events from sessions. Lifetimes: pool, threads, service, and
  // logger must outlive the server (see main() shutdown sequence).
  HttpServer(boost::asio::io_context& ioc, const config::Config& config,
             std::string version, std::string phase,
             std::shared_ptr<idempotency::IdempotencyService> service,
             boost::asio::thread_pool* db_pool, observability::Logger& logger);
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void start();
  void stop();

  // Bound port. Useful with port 0 (OS-assigned, used by tests).
  // Only valid after start().
  [[nodiscard]] std::uint16_t port() const;

 private:
  void do_accept();

  boost::asio::io_context& ioc_;
  config::Config config_;
  std::string version_;
  std::string phase_;
  std::shared_ptr<idempotency::IdempotencyService> service_;
  boost::asio::thread_pool* db_pool_;
  observability::Logger& logger_;
  boost::asio::ip::tcp::acceptor acceptor_;
  bool running_{false};
};

}  // namespace apex::execution
