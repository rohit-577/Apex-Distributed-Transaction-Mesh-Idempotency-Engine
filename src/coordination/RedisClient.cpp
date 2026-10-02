#include "coordination/RedisClient.hpp"

#include <utility>

#include <sw/redis++/redis++.h>

namespace apex::coordination {

namespace {

[[noreturn]] void throw_redis(const std::string& context, const sw::redis::Error& e) {
  throw RedisError(context + ": " + e.what());
}

}  // namespace

RedisClient::RedisClient(RedisEndpoint endpoint) {
  sw::redis::ConnectionOptions opts;
  opts.host = std::move(endpoint.host);
  opts.port = static_cast<int>(endpoint.port);
  if (!endpoint.password.empty()) {
    opts.password = std::move(endpoint.password);
  }
  opts.connect_timeout = endpoint.connect_timeout;
  opts.socket_timeout = endpoint.command_timeout;

  sw::redis::ConnectionPoolOptions pool_opts;
  pool_opts.size = static_cast<int>(endpoint.pool_size);
  // Wait for a pooled connection rather than failing under burst: bounding
  // happens via socket_timeout, and pool exhaustion just queues workers.
  pool_opts.wait_timeout = endpoint.command_timeout;

  try {
    redis_ = std::make_unique<sw::redis::Redis>(std::move(opts), pool_opts);
  } catch (const sw::redis::Error& e) {
    throw_redis("Redis client construction failed", e);
  }
}

RedisClient::~RedisClient() = default;

void RedisClient::ping() {
  try {
    (void)redis_->ping();
  } catch (const sw::redis::Error& e) {
    throw_redis("Redis ping failed", e);
  }
}

bool RedisClient::set_if_absent(const std::string& key, const std::string& value,
                                 std::chrono::milliseconds ttl) {
  try {
    // NX (only when absent) + PX (expiry) in ONE atomic command. This is the
    // entire lease-acquisition primitive: either we hold the lease with our
    // token, or someone else does — there is no intermediate state.
    return redis_->set(key, value, ttl, sw::redis::UpdateType::NOT_EXIST);
  } catch (const sw::redis::Error& e) {
    throw_redis("Redis SET NX PX failed", e);
  }
}

std::optional<std::string> RedisClient::get(const std::string& key) {
  try {
    // sw::redis::Optional is a custom optional (explicit bool, value()/ *
    // accessors) — convert to std::optional at the boundary so callers only
    // ever see standard types.
    const sw::redis::OptionalString result = redis_->get(key);
    if (!result) {
      return std::nullopt;
    }
    return *result;
  } catch (const sw::redis::Error& e) {
    throw_redis("Redis GET failed", e);
  }
}

long long RedisClient::compare_and_delete(const std::string& key, const std::string& expected) {
  // The check (GET == expected) and the DEL execute inside one Lua script,
  // i.e. atomically from every client's point of view. A separate GET-then-
  // DEL would allow: A reads token (matches) → A's lease expires → B
  // acquires (B's token stored) → A DELs → B's lease destroyed with A never
  // owning it. The script closes exactly that interleaving.
  static constexpr const char* kScript =
      "if redis.call('GET', KEYS[1]) == ARGV[1] then "
      "return redis.call('DEL', KEYS[1]) "
      "else return 0 end";
  try {
    const std::vector<sw::redis::StringView> keys{sw::redis::StringView(key)};
    const std::vector<sw::redis::StringView> args{sw::redis::StringView(expected)};
    return redis_->eval<long long>(kScript, keys.begin(), keys.end(), args.begin(), args.end());
  } catch (const sw::redis::Error& e) {
    throw_redis("Redis release script failed", e);
  }
}

void RedisClient::del(const std::string& key) {
  try {
    (void)redis_->del(key);
  } catch (const sw::redis::Error& e) {
    throw_redis("Redis DEL failed", e);
  }
}

}  // namespace apex::coordination
