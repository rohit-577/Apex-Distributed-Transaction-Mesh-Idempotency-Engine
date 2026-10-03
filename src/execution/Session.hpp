#pragma once

// One HTTP connection. Owns its socket and all read/write state; destroys
// itself when the connection ends or an unrecoverable error occurs.
//
// Rules enforced here:
// - No blocking work: request handling is either a pure Router call, an
//   asynchronous DependencyChecker probe, or a worker-pool service call
//   whose completion posts back. Waiting suspends WITHOUT holding any
//   thread: the session persists as timer + registry registration only.
// - Bounded state: the request body parser rejects payloads over 1 MiB with
//   a controlled 413 instead of growing memory without limit.
// - Controlled errors: unparsable bytes get a 400 response (then the
//   connection closes, because the stream position is unreliable);
//   every other failure mode maps to a JSON status, never to a dropped
//   connection or an exception escaping into the I/O loop.
//
// Multiplexing (Phase 3): when the service verdict is Wait, the session
// registers in the shared WaiterRegistry and arms ONE Asio timer (the
// sooner of recheck-interval / remaining-deadline). Wake-up (local notify,
// Redis pub/sub, timer) always funnels into a durable re-read via
// service->handle() — the notification never decides, PostgreSQL does
// (INV-MUX-03/04). Timeout answers 202 without touching durable state
// (INV-MUX-06). All wait continuations run on the session strand.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "api/Router.hpp"
#include "config/Config.hpp"
#include "idempotency/IdempotencyService.hpp"

namespace apex::observability {
class Logger;
}  // namespace apex::observability

namespace boost::asio {
class thread_pool;
}  // namespace boost::asio

namespace apex::execution {

class Session : public std::enable_shared_from_this<Session> {
 public:
  static constexpr std::size_t kMaxBodyBytes = 1024 * 1024;  // 1 MiB

  // Takes ownership of a connected socket. `version`/`phase` are reported
  // in JSON bodies; they come from the APEX_VERSION/APEX_PHASE compile
  // definitions.
  // `service`/`db_pool` wire the durable idempotency layer: both null means
  // storage is not configured and POST /v1/operations answers 503. When a
  // service is present, db_pool must outlive every session (owned by main()
  // or the test fixture, joined before destruction). `logger` emits the
  // waiter lifecycle events (§26); it must outlive every session.
  static void launch(boost::asio::ip::tcp::socket socket, const config::Config& config,
                     std::string version, std::string phase,
                     std::shared_ptr<idempotency::IdempotencyService> service,
                     boost::asio::thread_pool* db_pool, observability::Logger& logger);

 private:
  Session(boost::asio::ip::tcp::socket socket, const config::Config& config,
          std::string version, std::string phase,
          std::shared_ptr<idempotency::IdempotencyService> service,
          boost::asio::thread_pool* db_pool, observability::Logger& logger);

 public:
  // Releases any pending waiter registration. Sessions are always
  // shared_ptr-owned (see launch); destruction only happens after the
  // response settled or the connection died, so this purely drops the
  // waiter's registry slot — owner, lease, epoch, and row are untouched
  // (client cancellation removes only the HTTP waiter).
  ~Session();

  void do_read();
  void on_read(boost::beast::error_code ec);
  void handle_request();
  void handle_ready(boost::beast::http::verb method, unsigned version, bool keep_alive);
  void handle_operations(unsigned version, bool keep_alive);
  void handle_metrics(unsigned version, bool keep_alive);
  // Phase 3 waiter suspend/resume. All run on the session strand; the pool
  // is only used for service->handle() re-reads.
  void enter_wait(idempotency::OperationRequest request, unsigned version, bool keep_alive);
  void continue_wait();
  void recheck_now();
  // Re-registers for another wait cycle (notify erases slots). False means
  // shutdown/cap: settle 202 immediately.
  bool reregister_for_wait();
  void on_recheck_result(const idempotency::OperationOutcome& outcome);
  void on_wait_timer(boost::beast::error_code ec, std::uint64_t generation);
  void on_wait_wake();
  void settle_wait(int status, const std::string& body);
  void settle_wait_timeout();
  void abort_wait();
  [[nodiscard]] static std::string waiter_timeout_body(bool shutting_down);
  void logger_wait_event(const char* event);
  void send_json(int status, const std::string& body, unsigned version, bool keep_alive,
                 const std::string& allow = "", const std::string& content_type = "application/json");
  void count_validation_failure();
  void do_write();
  void do_close();

  // Deferred waiter response state. Present only while a response is
  // pending on another generation's completion.
  struct PendingWait {
    idempotency::OperationRequest request;
    std::string channel;
    std::uint64_t waiter_id{0};
    unsigned http_version{11};
    bool keep_alive{false};
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::time_point wait_start{};
    bool settled{false};
    // True while counted in apex_waiters_active (set when the fallback
    // timer arms = truly suspended; cleared on every exit path exactly once).
    bool counts_active{false};
  };

  boost::beast::tcp_stream stream_;
  // Serialization for ALL session continuations. The I/O pool runs many
  // threads, so timer expiries, registry wakes, and pool completions would
  // otherwise execute concurrently on different threads while mutating
  // pending_wait_, the timer generation, and the Beast stream (concurrent
  // async stream operations are undefined behavior). Every handler below is
  // bound to this strand (or posted through it), so session state is
  // effectively single-threaded without ever blocking a thread.
  boost::asio::strand<boost::asio::any_io_executor> strand_;
  boost::beast::flat_buffer buffer_;
  std::optional<boost::beast::http::request_parser<boost::beast::http::string_body>> parser_;
  boost::beast::http::request<boost::beast::http::string_body> request_;
  boost::beast::http::response<boost::beast::http::string_body> response_;
  config::Config config_;
  api::Router router_;
  std::shared_ptr<idempotency::IdempotencyService> service_;
  boost::asio::thread_pool* db_pool_;
  observability::Logger& logger_;
  boost::asio::steady_timer wait_timer_;
  std::optional<PendingWait> pending_wait_;
  // Timer generation: every arm bumps it; handlers carrying a stale
  // generation no-op. Without this, arming a second wait while a first is
  // still pending would cancel it, and the cancellation handler would tear
  // down the new cycle's state (orphaning the waiter with no timer and no
  // registration). Strand-confined, plain integer.
  std::uint64_t wait_timer_generation_{0};
  // Per-request correlation ID (operations route only). Echoed back as the
  // X-Request-ID response header; never part of the fingerprint.
  std::string correlation_id_;
};

}  // namespace apex::execution
