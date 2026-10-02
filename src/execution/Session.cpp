#include "execution/Session.hpp"

#include <chrono>
#include <memory>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <nlohmann/json.hpp>

#include "execution/DependencyChecker.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyKey.hpp"
#include "idempotency/IdempotencyService.hpp"

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

void Session::launch(tcp::socket socket, const config::Config& config, std::string version,
                     std::shared_ptr<idempotency::IdempotencyService> service,
                     asio::thread_pool* db_pool) {
  // Private constructor: launched through shared_ptr so async handlers can
  // safely extend the session lifetime.
  auto session = std::shared_ptr<Session>(
      new Session(std::move(socket), config, std::move(version), std::move(service), db_pool));
  session->do_read();
}

Session::Session(tcp::socket socket, const config::Config& config, std::string version,
                 std::shared_ptr<idempotency::IdempotencyService> service,
                 asio::thread_pool* db_pool)
    : stream_(std::move(socket)),
      config_(config),
      router_(std::move(version)),
      service_(std::move(service)),
      db_pool_(db_pool) {}

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

  // POST /v1/operations needs the durable service and therefore cannot be
  // answered by the pure Router (which sees no I/O and no state). It is
  // dispatched here, exactly like /ready.
  if (path_only(target) == "/v1/operations") {
    handle_operations(request_.version(), request_.keep_alive());
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

void Session::handle_operations(unsigned version, bool keep_alive) {
  if (request_.method() != http::verb::post) {
    send_json(405, R"({"error":"method_not_allowed"})", version, keep_alive, "POST");
    return;
  }

  // Idempotency-Key is required and validated before anything touches the
  // database. Beast header lookup is case-insensitive, as HTTP requires.
  const auto key_field = request_.find("Idempotency-Key");
  if (key_field == request_.end()) {
    send_json(400, R"({"error":"missing_idempotency_key"})", version, keep_alive);
    return;
  }
  const std::string key{key_field->value()};
  const idempotency::KeyCheck key_check = idempotency::validate_key(key);
  if (!key_check.ok) {
    const nlohmann::json body = {
        {"error", "invalid_idempotency_key"},
        {"reason", idempotency::key_problem_reason(key_check.problem)}};
    send_json(400, body.dump(), version, keep_alive);
    return;
  }

  const idempotency::Fingerprint fingerprint =
      idempotency::fingerprint_for("POST", "/v1/operations", request_.body());
  if (!fingerprint.ok) {
    send_json(400, R"({"error":"invalid_json_body"})", version, keep_alive);
    return;
  }

  if (service_ == nullptr || db_pool_ == nullptr) {
    // Storage not wired (DB-less run or bare test fixture): honest 503, not
    // a silent success. Operators wire APEX_POSTGRES_* to enable this route.
    send_json(503, R"({"error":"storage_unavailable"})", version, keep_alive);
    return;
  }

  // INV-01: the repository blocks on the network, so it runs on the database
  // pool while this session suspends. The socket idle timer is paused for the
  // same reason as in handle_ready. `self` keeps the session alive across
  // both hops; if the server stops meanwhile, the final post lands on a
  // stopped executor and is dropped safely.
  stream_.expires_never();
  auto self = shared_from_this();
  idempotency::OperationRequest op_request{key, fingerprint.hex, fingerprint.canonical_body};
  asio::post(*db_pool_,
             [self, op_request = std::move(op_request), version, keep_alive]() mutable {
               idempotency::OperationOutcome outcome;
               outcome.http_status = 503;
               outcome.body = R"({"error":"storage_unavailable"})";
               try {
                 outcome = self->service_->handle(op_request);
               } catch (const std::exception&) {
                 // Unreachable by contract (handle() is no-throw), kept so a
                 // pool thread can never die from an escaping exception.
               }
               asio::post(self->stream_.get_executor(),
                          [self, outcome = std::move(outcome), version, keep_alive]() mutable {
                            self->stream_.expires_after(std::chrono::seconds(30));
                            // Phase 1 responses are always JSON (see
                            // OperationOutcome); content_type stays
                            // application/json.
                            self->send_json(outcome.http_status, outcome.body, version,
                                            keep_alive);
                          });
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
