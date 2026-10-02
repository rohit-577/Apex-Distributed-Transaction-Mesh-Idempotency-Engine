#pragma once

// Non-blocking dependency probing for GET /ready.
//
// Phase 0 deliberately checks only TCP reachability of PostgreSQL and Redis:
// opening a real SQL/RESP session belongs to Phase 1 (persistence and
// coordination layers). The check is fully asynchronous — no I/O thread
// ever blocks — and every probe carries a deadline so /ready always
// terminates even when the network drops packets silently.
//
// Result mapping (see Session):
//   both reachable -> 200 {"status":"ready", ...}
//   otherwise      -> 503 {"status":"not_ready", ...}

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include <boost/asio/any_io_executor.hpp>

namespace apex::execution {

struct Endpoint {
  std::string host;
  std::uint16_t port;
};

struct DependencyStatus {
  bool postgres_reachable{false};
  bool redis_reachable{false};

  [[nodiscard]] bool ready() const noexcept { return postgres_reachable && redis_reachable; }
};

class DependencyChecker {
 public:
  using Handler = std::function<void(DependencyStatus)>;

  // Resolves both endpoints, attempts a TCP connect to each with the given
  // deadline, and invokes handler exactly once with the outcome. Safe to
  // call from any thread; the handler runs on `executor`.
  static void async_check(boost::asio::any_io_executor executor, Endpoint postgres,
                          Endpoint redis, std::chrono::milliseconds timeout, Handler handler);
};

}  // namespace apex::execution
