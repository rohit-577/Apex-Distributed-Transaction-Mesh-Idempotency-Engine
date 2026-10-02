#include "observability/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace apex::observability {

Logger::Logger(Level min_level) : min_level_(min_level) {}

void Logger::debug(const std::string& message) { log(Level::Debug, message); }
void Logger::info(const std::string& message) { log(Level::Info, message); }
void Logger::warning(const std::string& message) { log(Level::Warning, message); }
void Logger::error(const std::string& message) { log(Level::Error, message); }

void Logger::log(Level level, const std::string& message) {
  if (level < min_level_) {
    return;
  }
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char stamp[32]{};
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);

  std::lock_guard<std::mutex> lock(mutex_);
  std::cerr << "[" << stamp << "] [" << to_string(level) << "] " << message << "\n";
}

Level Logger::parse_or(Level fallback, const std::string& text) {
  if (text == "debug") {
    return Level::Debug;
  }
  if (text == "info") {
    return Level::Info;
  }
  if (text == "warning") {
    return Level::Warning;
  }
  if (text == "error") {
    return Level::Error;
  }
  return fallback;
}

const char* Logger::to_string(Level level) {
  switch (level) {
    case Level::Debug:
      return "DEBUG";
    case Level::Info:
      return "INFO";
    case Level::Warning:
      return "WARN";
    case Level::Error:
      return "ERROR";
  }
  return "UNKNOWN";
}

std::string safe_key(const std::string& key) {
  constexpr std::size_t kLoggedKeyPrefix = 16;
  if (key.size() <= kLoggedKeyPrefix) {
    return key;
  }
  return key.substr(0, kLoggedKeyPrefix) + "...(len=" + std::to_string(key.size()) + ")";
}

}  // namespace apex::observability
