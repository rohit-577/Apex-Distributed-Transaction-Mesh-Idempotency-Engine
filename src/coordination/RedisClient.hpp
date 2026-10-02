#pragma once

// Thin RAII wrapper over redis-plus-plus. Blocking by design: every command
// may wait on the network up to the configured command timeout, so a
// RedisClient must only ever be used on worker threads — never on a
// Boost.Asio I/O thread (INV-01, same rule as PgConnection).
//
// Per AGENTS.md §2 this is the ONLY place that opens Redis connections:
// LeaseManager and all tests go through here. Connection pooling is
// redis-plus-plus's own (thread-safe `sw::redis::Redis` with a pool), sized
// from configuration.
//
// Fail-closed behavior: any connection/command failure surfaces as
// RedisError. Callers (LeaseManager, service) map it to controlled
// unavailability — never to "proceed without coordination".

#include <chrono>
#include <cstdint>
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

 private:
  std::unique_ptr<sw::redis::Redis> redis_;
};

}  // namespace apex::coordination
