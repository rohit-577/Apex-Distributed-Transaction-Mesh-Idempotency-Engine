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
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"

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
                     std::string phase,
                     std::shared_ptr<idempotency::IdempotencyService> service,
                     asio::thread_pool* db_pool, observability::Logger& logger) {
  // Private constructor: launched through shared_ptr so async handlers can
  // safely extend the session lifetime.
  auto session = std::shared_ptr<Session>(new Session(std::move(socket), config,
                                                      std::move(version), std::move(phase),
                                                      std::move(service), db_pool, logger));
  session->do_read();
}

Session::Session(tcp::socket socket, const config::Config& config, std::string version,
                 std::string phase, std::shared_ptr<idempotency::IdempotencyService> service,
                 asio::thread_pool* db_pool, observability::Logger& logger)
    : stream_(std::move(socket)),
      config_(config),
      router_(std::move(version), std::move(phase)),
      service_(std::move(service)),
      db_pool_(db_pool),
      logger_(logger),
      wait_timer_(stream_.get_executor()) {}

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

  // INV-01: the service blocks on the network, so it runs on the database
  // pool while this session suspends. The socket idle timer is paused (the
  // waiter timers below bound the suspension instead). `self` keeps the
  // session alive across both hops; if the server stops meanwhile, the
  // final post lands on a stopped executor and is dropped safely.
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
                          [self, outcome = std::move(outcome),
                           op_request = std::move(op_request), version,
                           keep_alive]() mutable {
                            self->stream_.expires_after(std::chrono::seconds(30));
                            if (outcome.kind ==
                                idempotency::OperationOutcome::Kind::Wait) {
                              // Another generation owns the operation: suspend
                              // asynchronously (no thread held) and converge
                              // on its durable result (INV-MUX-01).
                              self->enter_wait(std::move(op_request), version, keep_alive);
                              return;
                            }
                            // All responses are JSON (see OperationOutcome);
                            // content_type stays application/json.
                            self->send_json(outcome.http_status, outcome.body, version,
                                            keep_alive);
                          });
             });
}

void Session::enter_wait(idempotency::OperationRequest request, unsigned version,
                         bool keep_alive) {
  // Strand context (posted back from the pool verdict).
  auto registry = service_->registry();
  const auto options = registry->options();
  PendingWait wait;
  wait.request = std::move(request);
  wait.channel =
      idempotency::WaiterRegistry::channel_for(wait.request.key, wait.request.fingerprint);
  wait.http_version = version;
  wait.keep_alive = keep_alive;
  wait.deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(options.timeout_ms);
  wait.settled = false;
  pending_wait_ = std::move(wait);
  logger_wait_event("started");
  continue_wait();
}

void Session::continue_wait() {
  // Strand context. Every cycle: (re-)register, then ALWAYS re-check durable
  // state immediately. The unconditional re-check is what closes the §8
  // race: a terminal write landing anywhere before that re-check's database
  // read is observed, so completion-before-registration can never strand
  // this waiter. Registration is idempotent across cycles: the previous id
  // (if any) is released first.
  if (!pending_wait_ || pending_wait_->settled) {
    return;
  }
  PendingWait& wait = *pending_wait_;
  auto registry = service_->registry();
  if (registry->is_shutdown()) {
    settle_wait(202, waiter_timeout_body(true));
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= wait.deadline) {
    settle_wait_timeout();
    return;
  }
  if (wait.waiter_id != 0) {
    registry->unregister(wait.channel, wait.waiter_id);
    wait.waiter_id = 0;
  }
  auto self_weak = std::weak_ptr<Session>(shared_from_this());
  auto executor = stream_.get_executor();
  const idempotency::WaiterRegistry::Registration receipt = registry->register_waiter(
      wait.channel, self_weak,
      [self_weak, executor] {
        asio::post(executor, [self_weak] {
          if (auto self = self_weak.lock()) {
            self->on_wait_wake();
          }
        });
      });
  if (receipt.rejected) {
    // Shutdown or per-key cap: transient 202, durable state untouched.
    settle_wait(202, waiter_timeout_body(true));
    return;
  }
  wait.waiter_id = receipt.waiter_id;
  // Mandatory immediate re-check (never "register then sleep"): decides from
  // PostgreSQL truth as of NOW. A later wake or the fallback timer drives
  // the next cycle if still PROCESSING.
  recheck_now();
}

void Session::recheck_now() {
  // Strand context: run the SAME decision engine again on a pool thread.
  // Terminal rows answer immediately; a dead owner transparently promotes
  // this waiter to recoverer; a live owner yields Wait again.
  if (!pending_wait_ || pending_wait_->settled) {
    return;
  }
  auto self = shared_from_this();
  idempotency::OperationRequest request = pending_wait_->request;
  asio::post(*db_pool_, [self, request = std::move(request)]() mutable {
    idempotency::OperationOutcome outcome;
    outcome.http_status = 503;
    outcome.body = R"({"error":"storage_unavailable"})";
    try {
      outcome = self->service_->handle(request);
    } catch (const std::exception&) {
    }
    asio::post(self->stream_.get_executor(),
               [self, outcome = std::move(outcome)]() mutable {
                 self->on_recheck_result(outcome);
               });
  });
}

void Session::on_recheck_result(const idempotency::OperationOutcome& outcome) {
  // Strand context: the durable verdict for this cycle.
  if (!pending_wait_ || pending_wait_->settled) {
    return;
  }
  if (outcome.kind == idempotency::OperationOutcome::Kind::Wait) {
    // Still owned elsewhere. Deadline FIRST: a waiter that outlasts its
    // maximum duration answers 202 without touching durable state
    // (INV-MUX-06) — this check must live on every Wait continuation, not
    // just at registration, or the waiter would re-arm forever.
    PendingWait& wait = *pending_wait_;
    if (std::chrono::steady_clock::now() >= wait.deadline) {
      settle_wait_timeout();
      return;
    }
    // Otherwise arm the single fallback timer for min(recheck, remaining).
    // It covers missed notifications (INV-MUX-05); live wakes shortcut it
    // via on_wait_wake.
    const auto remaining = wait.deadline - std::chrono::steady_clock::now();
    auto interval =
        std::chrono::milliseconds(service_->registry()->options().recheck_ms);
    if (interval > remaining && remaining.count() > 0) {
      interval = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    }
    if (interval.count() <= 0) {
      interval = std::chrono::milliseconds(1);
    }
    wait_timer_.expires_after(interval);
    auto self = shared_from_this();
    wait_timer_.async_wait([self](boost::beast::error_code ec) { self->on_wait_timer(ec); });
    return;
  }
  logger_wait_event("completed");
  settle_wait(outcome.http_status, outcome.body);
}

void Session::on_wait_timer(boost::beast::error_code ec) {
  // Strand context.
  if (ec == boost::asio::error::operation_aborted) {
    // Timer cancelled by shutdown/ioc-stop: release the registration and
    // die silently (no response possible on a stopping loop).
    abort_wait();
    return;
  }
  if (ec) {
    abort_wait();
    return;
  }
  recheck_now();  // Fallback re-check: covers missed notifications (INV-MUX-05).
}

void Session::on_wait_wake() {
  // Strand context, from registry (local notify / pub-sub / sweep).
  // A wake is a HINT (INV-MUX-04): fall through to the durable re-read,
  // which decides. Missed wakes are covered by the timer.
  if (!pending_wait_ || pending_wait_->settled) {
    return;
  }
  logger_wait_event("woken");
  recheck_now();
}

void Session::settle_wait(int status, const std::string& body) {
  // Strand context: exactly-once response for the pending waiter.
  if (!pending_wait_ || pending_wait_->settled) {
    return;
  }
  PendingWait& wait = *pending_wait_;
  wait.settled = true;
  wait_timer_.cancel();
  if (wait.waiter_id != 0) {
    service_->registry()->unregister(wait.channel, wait.waiter_id);
    wait.waiter_id = 0;
  }
  const unsigned version = wait.http_version;
  const bool keep_alive = wait.keep_alive;
  pending_wait_.reset();
  send_json(status, body, version, keep_alive);
}

void Session::settle_wait_timeout() {
  // Waiter timeout (INV-MUX-06): THIS request stops waiting. The durable
  // record, the owner, the lease, and the epoch are all untouched — a later
  // retry re-observes or recovers normally.
  logger_wait_event("timeout");
  settle_wait(202, waiter_timeout_body(false));
}

void Session::abort_wait() {
  if (!pending_wait_) {
    return;
  }
  if (pending_wait_->waiter_id != 0 && service_ != nullptr) {
    service_->registry()->unregister(pending_wait_->channel, pending_wait_->waiter_id);
  }
  pending_wait_.reset();
}

std::string Session::waiter_timeout_body(bool shutting_down) {
  const nlohmann::json body = {
      {"status", "processing"},
      {"message",
       shutting_down
           ? "Server is shutting down; operation state is unchanged. Retry with the same "
             "Idempotency-Key to receive the result."
           : "Waiter timeout: the operation is still in progress and this request stopped "
             "waiting. Durable state is unchanged — retry with the same Idempotency-Key "
             "to observe or recover it."}};
  return body.dump();
}

void Session::logger_wait_event(const char* event) {
  // Structured waiter lifecycle events (§26): started/woken/timeout/
  // completed. Key by safe prefix + fingerprint (never bodies/secrets).
  // The logger is thread-safe; this runs on the session strand.
  if (!pending_wait_) {
    return;
  }
  const std::string line =
      std::string("idempotency.wait.") + event +
      " key=" + observability::safe_key(pending_wait_->request.key) +
      " fp=" + pending_wait_->request.fingerprint;
  if (std::string(event) == "timeout") {
    logger_.warning(line);
  } else {
    logger_.info(line);
  }
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
