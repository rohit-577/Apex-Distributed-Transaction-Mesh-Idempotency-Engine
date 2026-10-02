#pragma once

// Thin RAII wrapper over redis-plus-plus. Blocking by design: every command
// may wait on the network up to the configured command timeout, so a
// RedisClient must only ever be used on worker threads — never on a
// Boost.Asio I/O thread (INV-01, same rule as PgConnection).
//
// Per AGENTS.md §2 this is the ONLY place that opens Redis connections:
// LeaseManager, the completion publisher/subscriber, and all tests go
// through here. Connection pooling is redis-plus-plus's own (thread-safe
// `sw::redis::Redis` with a pool), sized from configuration. The blocking
// subscriber loop runs on ONE dedicated coordination thread (see
// CompletionSubscriber), never per-waiter and never on I/O threads.
//
// Fail-closed behavior: any connection/command failure surfaces as
// RedisError. Callers (LeaseManager, service) map it to controlled
// unavailability — never to "proceed without coordination".

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace sw::redis {
class Redis;
}  // namespace sw::redis

namespace apex::coordination {

class RedisError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct RedisEndpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{6379};
  std::string password;  // Empty = no AUTH. Never logged.
  std::chrono::milliseconds connect_timeout{2000};
  std::chrono::milliseconds command_timeout{2000};
  std::size_t pool_size{8};
};

class RedisClient {
 public:
  // Stores options only; redis-plus-plus connects lazily on first command,
  // so construction never touches the network (a dead Redis at boot degrades
  // to per-operation RedisError, not a refusal to boot).
  explicit RedisClient(RedisEndpoint endpoint);
  ~RedisClient();

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  // Liveness probe. Throws RedisError when unreachable (R1 exercises this).
  void ping();

  // SET key value NX PX <ttl>. Returns true iff the key was set (lease won),
  // false iff a value already exists (lease held). Atomic by Redis decree.
  [[nodiscard]] bool set_if_absent(const std::string& key, const std::string& value,
                                   std::chrono::milliseconds ttl);

  // GET key. nullopt when absent (expired or never set). A present value is
  // a liveness hint only — never proof of ownership (INV-09).
  [[nodiscard]] std::optional<std::string> get(const std::string& key);

  // Atomic compare-and-delete (Lua): deletes `key` iff its current value
  // equals `expected`, returning 1 on deletion and 0 otherwise. This is what
  // makes release safe; see LeaseManager for the race it prevents.
  [[nodiscard]] long long compare_and_delete(const std::string& key,
                                             const std::string& expected);

  // Best-effort raw delete for test cleanup (leases only, never data).
  void del(const std::string& key);

  // PUBLISH channel message. Returns the number of receiving clients.
  // Best-effort by contract (at-most-once fan-out): a return of 0 or a
  // RedisError changes nothing about correctness — the durable commit
  // already happened and waiters re-check PostgreSQL regardless.
  [[nodiscard]] long long publish(const std::string& channel, const std::string& message);

  // Blocking pattern-subscribe loop for ONE pattern. Registers `on_message`,
  // subscribes, then consumes until `stop` is set. Returns normally on stop;
  // throws RedisError on connection loss or subscribe failure (the caller —
  // CompletionSubscriber — backs off, resubscribes, and sweeps waiters to
  // re-check durable state, which is what makes missed-notification windows
  // safe). Runs on the caller's thread: dedicated coordination thread only.
  using PatternMessageHandler =
      std::function<void(const std::string& pattern, const std::string& channel,
                         const std::string& message)>;
  void psubscribe_loop(const std::string& pattern, PatternMessageHandler on_message,
                       const std::atomic<bool>& stop);

 private:
  std::unique_ptr<sw::redis::Redis> redis_;
};

}  // namespace apex::coordination
