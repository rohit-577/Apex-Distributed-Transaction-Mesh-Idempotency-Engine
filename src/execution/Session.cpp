#include "execution/Session.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "execution/DependencyChecker.hpp"

namespace apex::execution {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

// Deadline for the /ready dependency probes. Short enough that readiness
// checks fail fast; the DependencyChecker, not this constant, owns the
// timeout mechanics.
constexpr std::chrono::milliseconds kReadyTimeout{2000};

std::string_view path_only(std::string_view target) {
  const std::size_t q = target.find('?');
  return q == std::string_view::npos ? target : target.substr(0, q);
}

}  // namespace

void Session::launch(tcp::socket socket, const config::Config& config, std::string version) {
  // Private constructor: launched through shared_ptr so async handlers can
  // safely extend the session lifetime.
  auto session =
      std::shared_ptr<Session>(new Session(std::move(socket), config, std::move(version)));
  session->do_read();
}

Session::Session(tcp::socket socket, const config::Config& config, std::string version)
    : stream_(std::move(socket)), config_(config), router_(std::move(version)) {}

void Session::do_read() {
  stream_.expires_after(std::chrono::seconds(30));
  parser_.emplace();
  parser_->body_limit(kMaxBodyBytes);
  auto self = shared_from_this();
  http::async_read(stream_, buffer_, *parser_,
                   [self](beast::error_code ec, std::size_t /*bytes*/) { self->on_read(ec); });
}

void Session::on_read(beast::error_code ec) {
  if (ec == http::error::end_of_stream) {
    do_close();
    return;
  }
  if (ec == http::error::body_limit) {
    // The client sent more than kMaxBodyBytes. The request is incomplete,
    // so answer with HTTP/1.1 defaults and close the connection.
    send_json(413, R"({"error":"payload_too_large"})", 11, /*keep_alive=*/false);
    return;
  }
  if (ec) {
    // Unparsable bytes: the stream position is unreliable, so report a
    // controlled 400 and then close rather than attempting to continue.
    send_json(400, R"({"error":"bad_request"})", 11, /*keep_alive=*/false);
    return;
  }
  request_ = parser_->release();
  handle_request();
}

void Session::handle_request() {
  const std::string target{request_.target()};
  const http::verb method = request_.method();

  if (path_only(target) == "/ready") {
    handle_ready(method, request_.version(), request_.keep_alive());
    return;
  }

  const api::RouteResult result = router_.route(method, target);
  send_json(result.status, result.body, request_.version(), request_.keep_alive(), result.allow);
}

void Session::handle_ready(http::verb method, unsigned version, bool keep_alive) {
  if (method != http::verb::get) {
    send_json(405, R"({"error":"method_not_allowed"})", version, keep_alive, "GET");
    return;
  }
  // The socket would otherwise idle-timeout while the probes run.
  stream_.expires_never();
  auto self = shared_from_this();
  DependencyChecker::async_check(
      stream_.get_executor(), Endpoint{config_.postgres_host, config_.postgres_port},
      Endpoint{config_.redis_host, config_.redis_port}, kReadyTimeout,
      [self, version, keep_alive](DependencyStatus status) {
        self->stream_.expires_after(std::chrono::seconds(30));
        const nlohmann::json body = {{"status", status.ready() ? "ready" : "not_ready"},
                                     {"service", "apex"},
                                     {"postgres_reachable", status.postgres_reachable},
                                     {"redis_reachable", status.redis_reachable}};
        self->send_json(status.ready() ? 200 : 503, body.dump(), version, keep_alive);
      });
}

void Session::send_json(int status, const std::string& body, unsigned version, bool keep_alive,
                        const std::string& allow) {
  response_.version(version);
  response_.result(static_cast<http::status>(status));
  response_.set(http::field::content_type, "application/json");
  if (!allow.empty()) {
    response_.set(http::field::allow, allow);
  }
  response_.body() = body;
  response_.prepare_payload();
  response_.keep_alive(keep_alive);
  do_write();
}

void Session::do_write() {
  auto self = shared_from_this();
  http::async_write(stream_, response_, [self](beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
      self->do_close();
      return;
    }
    if (!self->response_.keep_alive()) {
      self->do_close();
      return;
    }
    self->do_read();
  });
}

void Session::do_close() {
  beast::error_code ec;
  stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
}

}  // namespace apex::execution
