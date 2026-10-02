#include "execution/HttpServer.hpp"

#include <utility>

#include "execution/Session.hpp"

namespace apex::execution {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

HttpServer::HttpServer(asio::io_context& ioc, const config::Config& config, std::string version,
                       std::shared_ptr<idempotency::IdempotencyService> service,
                       asio::thread_pool* db_pool)
    : ioc_(ioc),
      config_(config),
      version_(std::move(version)),
      service_(std::move(service)),
      db_pool_(db_pool),
      acceptor_(ioc) {}

void HttpServer::start() {
  const tcp::endpoint endpoint{tcp::v4(), config_.port};
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::socket_base::reuse_address(true));
  // Throws boost::system::system_error when the port is unavailable; main()
  // reports it and exits non-zero. No half-started state escapes: either the
  // listen succeeds and running_ is set, or an exception propagates.
  acceptor_.bind(endpoint);
  acceptor_.listen(asio::socket_base::max_listen_connections);
  running_ = true;
  do_accept();
}

void HttpServer::stop() {
  running_ = false;
  boost::system::error_code ec;
  acceptor_.close(ec);
}

std::uint16_t HttpServer::port() const {
  if (!acceptor_.is_open()) {
    return 0;
  }
  return acceptor_.local_endpoint().port();
}

void HttpServer::do_accept() {
  acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
    if (!running_) {
      return;
    }
    if (!ec) {
      Session::launch(std::move(socket), config_, version_, service_, db_pool_);
    }
    // On transient accept errors the loop continues; on stop() running_ is
    // false and the chain ends here. The server object must outlive pending
    // accepts — main() and the test fixture both guarantee this by joining
    // the I/O threads before destroying the server.
    if (running_) {
      do_accept();
    }
  });
}

}  // namespace apex::execution
