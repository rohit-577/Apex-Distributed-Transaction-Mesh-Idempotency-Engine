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
constexpr const char* kRedisHostEnv = "APEX_REDIS_HOST";
constexpr const char* kRedisPortEnv = "APEX_REDIS_PORT";
constexpr const char* kLogLevelEnv = "APEX_LOG_LEVEL";

constexpr unsigned kMaxThreads = 256;

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

bool parse_threads(const char* text, unsigned& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 1 || value > static_cast<long>(kMaxThreads)) {
    return false;
  }
  out = static_cast<unsigned>(value);
  return true;
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
  if (redis_host.empty()) {
    return "redis_host must not be empty";
  }
  if (log_level != "debug" && log_level != "info" && log_level != "warning" &&
      log_level != "error") {
    return "log_level must be one of debug|info|warning|error";
  }
  return "";
}

}  // namespace apex::config
