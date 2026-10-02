#include "config/Config.hpp"

#include <cstdlib>
#include <optional>
#include <thread>

namespace apex::config {
namespace {

// MSVC deprecates std::getenv (C4996); _dupenv_s is the safe replacement
// because it hands ownership of the buffer to the caller instead of exposing
// a shared static. Returns nullopt when the variable is absent.
std::optional<std::string> get_env(const char* name) {
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
    return std::nullopt;
  }
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  if (const char* value = std::getenv(name); value != nullptr) {
    return std::string(value);
  }
  return std::nullopt;
#endif
}

constexpr const char* kPortEnv = "APEX_PORT";
constexpr const char* kThreadsEnv = "APEX_THREADS";
constexpr const char* kPgHostEnv = "APEX_POSTGRES_HOST";
constexpr const char* kPgPortEnv = "APEX_POSTGRES_PORT";
constexpr const char* kPgUserEnv = "APEX_POSTGRES_USER";
constexpr const char* kPgPasswordEnv = "APEX_POSTGRES_PASSWORD";
constexpr const char* kPgDbEnv = "APEX_POSTGRES_DB";
constexpr const char* kDbPoolEnv = "APEX_DB_POOL_SIZE";
constexpr const char* kMigrationsEnv = "APEX_MIGRATIONS_DIR";
constexpr const char* kRedisHostEnv = "APEX_REDIS_HOST";
constexpr const char* kRedisPortEnv = "APEX_REDIS_PORT";
constexpr const char* kRedisPasswordEnv = "APEX_REDIS_PASSWORD";
constexpr const char* kRedisPoolEnv = "APEX_REDIS_POOL_SIZE";
constexpr const char* kLeaseTtlEnv = "APEX_LEASE_TTL_MS";
constexpr const char* kRedisTimeoutEnv = "APEX_REDIS_OP_TIMEOUT_MS";
constexpr const char* kWaiterTimeoutEnv = "APEX_WAITER_TIMEOUT_MS";
constexpr const char* kWaiterRecheckEnv = "APEX_WAITER_RECHECK_MS";
constexpr const char* kMaxWaitersEnv = "APEX_MAX_WAITERS_PER_KEY";
constexpr const char* kLogLevelEnv = "APEX_LOG_LEVEL";

constexpr unsigned kMaxThreads = 256;
constexpr unsigned kMaxDbPool = 64;
constexpr unsigned kMaxRedisPool = 64;
// Lease window bounds (ms). Floor: below 1 s, expiry races the operation it
// is meant to protect. Ceiling: beyond 5 min the lease is effectively
// immortal and orphan recovery stops working.
constexpr unsigned kMinLeaseTtlMs = 1000;
constexpr unsigned kMaxLeaseTtlMs = 300000;
constexpr unsigned kMinRedisTimeoutMs = 100;
constexpr unsigned kMaxRedisTimeoutMs = 60000;
// Waiter bounds (ms, ms, count). Timeout floor keeps "wait" meaningful
// (below 1 s the waiter would almost always 202 spuriously); recheck floor
// keeps fallback reads off the hot path; the per-key cap bounds registry
// memory against abusive duplicate fan-in (§29).
constexpr unsigned kMinWaiterTimeoutMs = 1000;
constexpr unsigned kMaxWaiterTimeoutMs = 300000;
constexpr unsigned kMinWaiterRecheckMs = 100;
constexpr unsigned kMaxWaiterRecheckMs = 30000;
constexpr unsigned kMaxWaitersPerKey = 100000;

bool parse_port(const char* text, std::uint16_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 0 || value > 65535) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

bool parse_bounded_count(const char* text, unsigned& out, unsigned max) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 1 || value > static_cast<long>(max)) {
    return false;
  }
  out = static_cast<unsigned>(value);
  return true;
}

bool parse_ranged_ms(const char* text, unsigned& out, unsigned min_ms, unsigned max_ms) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < static_cast<long>(min_ms) ||
      value > static_cast<long>(max_ms)) {
    return false;
  }
  out = static_cast<unsigned>(value);
  return true;
}

bool parse_threads(const char* text, unsigned& out) {
  return parse_bounded_count(text, out, kMaxThreads);
}

unsigned default_threads() {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw <= 1) {
    return 2;  // Always keep at least 2 so one slow handler cannot stall accept.
  }
  return hw;
}

}  // namespace

Config Config::defaults() {
  Config cfg;
  cfg.threads = default_threads();
  return cfg;
}

std::pair<Config, std::vector<std::string>> Config::load_from_environment() {
  Config cfg = Config::defaults();
  std::vector<std::string> warnings;

  if (const auto v = get_env(kPortEnv); v && !v->empty()) {
    if (!parse_port(v->c_str(), cfg.port)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kPortEnv + "='" + *v +
                            "'; using default 8080.");
      cfg.port = 8080;
    }
  }
  if (const auto v = get_env(kThreadsEnv); v && !v->empty()) {
    if (!parse_threads(v->c_str(), cfg.threads)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kThreadsEnv + "='" + *v +
                            "'; using default.");
      cfg.threads = default_threads();
    }
  }
  if (const auto v = get_env(kPgHostEnv); v && !v->empty()) {
    cfg.postgres_host = *v;
  }
  if (const auto v = get_env(kPgPortEnv); v && !v->empty()) {
    if (!parse_port(v->c_str(), cfg.postgres_port)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kPgPortEnv + "='" + *v +
                            "'; using default 5432.");
      cfg.postgres_port = 5432;
    }
  }
  if (const auto v = get_env(kPgUserEnv); v && !v->empty()) {
    cfg.postgres_user = *v;
  }
  // The password intentionally has no warning text: values must never appear
  // in logs, even as "invalid '***'". An absent variable simply means "send
  // no password" (trust/peer auth).
  if (const auto v = get_env(kPgPasswordEnv); v) {
    cfg.postgres_password = *v;
  }
  if (const auto v = get_env(kPgDbEnv); v && !v->empty()) {
    cfg.postgres_db = *v;
  }
  if (const auto v = get_env(kDbPoolEnv); v && !v->empty()) {
    if (!parse_bounded_count(v->c_str(), cfg.db_pool_size, kMaxDbPool)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kDbPoolEnv +
                            "; using default 8.");
      cfg.db_pool_size = 8;
    }
  }
  if (const auto v = get_env(kMigrationsEnv); v && !v->empty()) {
    cfg.migrations_dir = *v;
  }
  if (const auto v = get_env(kRedisHostEnv); v && !v->empty()) {
    cfg.redis_host = *v;
  }
  if (const auto v = get_env(kRedisPortEnv); v && !v->empty()) {
    if (!parse_port(v->c_str(), cfg.redis_port)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kRedisPortEnv + "='" + *v +
                            "'; using default 6379.");
      cfg.redis_port = 6379;
    }
  }
  // Same no-echo policy as the PostgreSQL password: secrets never appear in
  // warnings or logs.
  if (const auto v = get_env(kRedisPasswordEnv); v) {
    cfg.redis_password = *v;
  }
  if (const auto v = get_env(kRedisPoolEnv); v && !v->empty()) {
    if (!parse_bounded_count(v->c_str(), cfg.redis_pool_size, kMaxRedisPool)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kRedisPoolEnv +
                            "; using default 8.");
      cfg.redis_pool_size = 8;
    }
  }
  if (const auto v = get_env(kLeaseTtlEnv); v && !v->empty()) {
    if (!parse_ranged_ms(v->c_str(), cfg.lease_ttl_ms, kMinLeaseTtlMs, kMaxLeaseTtlMs)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kLeaseTtlEnv +
                            "; using default 10000.");
      cfg.lease_ttl_ms = 10000;
    }
  }
  if (const auto v = get_env(kRedisTimeoutEnv); v && !v->empty()) {
    if (!parse_ranged_ms(v->c_str(), cfg.redis_op_timeout_ms, kMinRedisTimeoutMs,
                         kMaxRedisTimeoutMs)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kRedisTimeoutEnv +
                            "; using default 2000.");
      cfg.redis_op_timeout_ms = 2000;
    }
  }
  if (const auto v = get_env(kWaiterTimeoutEnv); v && !v->empty()) {
    if (!parse_ranged_ms(v->c_str(), cfg.waiter_timeout_ms, kMinWaiterTimeoutMs,
                         kMaxWaiterTimeoutMs)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kWaiterTimeoutEnv +
                            "; using default 30000.");
      cfg.waiter_timeout_ms = 30000;
    }
  }
  if (const auto v = get_env(kWaiterRecheckEnv); v && !v->empty()) {
    if (!parse_ranged_ms(v->c_str(), cfg.waiter_recheck_ms, kMinWaiterRecheckMs,
                         kMaxWaiterRecheckMs)) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kWaiterRecheckEnv +
                            "; using default 1000.");
      cfg.waiter_recheck_ms = 1000;
    }
  }
  if (const auto v = get_env(kMaxWaitersEnv); v && !v->empty()) {
    if (!parse_bounded_count(v->c_str(), cfg.max_waiters_per_key, kMaxWaitersPerKey) ||
        cfg.max_waiters_per_key < 1) {
      warnings.emplace_back(std::string("Ignoring invalid ") + kMaxWaitersEnv +
                            "; using default 1024.");
      cfg.max_waiters_per_key = 1024;
    }
  }
  if (const auto v = get_env(kLogLevelEnv); v && !v->empty()) {
    cfg.log_level = *v;
  }

  return {cfg, warnings};
}

std::string Config::validate() const {
  // Port 0 is intentionally valid: it means "OS-assigned" and exists for
  // tests. There is no way to express "above 65535" in a uint16_t, so no
  // upper-bound check is needed here — string parsing enforces the range.
  if (threads < 1 || threads > kMaxThreads) {
    return "threads must be in [1, 256]";
  }
  if (postgres_host.empty()) {
    return "postgres_host must not be empty";
  }
  if (postgres_user.empty()) {
    return "postgres_user must not be empty";
  }
  if (postgres_db.empty()) {
    return "postgres_db must not be empty";
  }
  if (db_pool_size < 1 || db_pool_size > kMaxDbPool) {
    return "db_pool_size must be in [1, 64]";
  }
  if (migrations_dir.empty()) {
    return "migrations_dir must not be empty";
  }
  if (redis_host.empty()) {
    return "redis_host must not be empty";
  }
  if (redis_pool_size < 1 || redis_pool_size > kMaxRedisPool) {
    return "redis_pool_size must be in [1, 64]";
  }
  if (lease_ttl_ms < kMinLeaseTtlMs || lease_ttl_ms > kMaxLeaseTtlMs) {
    return "lease_ttl_ms must be in [1000, 300000]";
  }
  if (redis_op_timeout_ms < kMinRedisTimeoutMs || redis_op_timeout_ms > kMaxRedisTimeoutMs) {
    return "redis_op_timeout_ms must be in [100, 60000]";
  }
  if (waiter_timeout_ms < kMinWaiterTimeoutMs || waiter_timeout_ms > kMaxWaiterTimeoutMs) {
    return "waiter_timeout_ms must be in [1000, 300000]";
  }
  if (waiter_recheck_ms < kMinWaiterRecheckMs || waiter_recheck_ms > kMaxWaiterRecheckMs) {
    return "waiter_recheck_ms must be in [100, 30000]";
  }
  if (max_waiters_per_key < 1 || max_waiters_per_key > kMaxWaitersPerKey) {
    return "max_waiters_per_key must be in [1, 100000]";
  }
  if (log_level != "debug" && log_level != "info" && log_level != "warning" &&
      log_level != "error") {
    return "log_level must be one of debug|info|warning|error";
  }
  return "";
}

std::string Config::postgres_conninfo() const {
  // libpq keyword/value format. connect_timeout bounds every new connection
  // (including startup schema checks) so a dead database delays but never
  // hangs the gateway. application_name identifies us in pg_stat_activity.
  // NOTE: the result contains the password when one is configured — secret.
  const auto quote = [](const std::string& value) {
    if (value.find_first_of(" \t'\\") == std::string::npos) {
      return value;
    }
    std::string out = "'";
    for (char c : value) {
      if (c == '\'' || c == '\\') {
        out += '\\';
      }
      out += c;
    }
    out += "'";
    return out;
  };
  std::string info = "host=" + quote(postgres_host) + " port=" + std::to_string(postgres_port) +
                     " dbname=" + quote(postgres_db) + " user=" + quote(postgres_user) +
                     " connect_timeout=5 application_name=apex";
  if (!postgres_password.empty()) {
    info += " password=" + quote(postgres_password);
  }
  return info;
}

}  // namespace apex::config
