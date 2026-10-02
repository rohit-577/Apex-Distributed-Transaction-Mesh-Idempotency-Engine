#pragma once

// Minimal thread-safe stderr logger. Deliberately tiny: structured logging
// and log sinks are observability work for a later phase. The only contract
// that matters now is that logging from any I/O thread never interleaves
// bytes and never throws into networking code.

#include <mutex>
#include <string>

namespace apex::observability {

enum class Level { Debug = 0, Info = 1, Warning = 2, Error = 3 };

class Logger {
 public:
  explicit Logger(Level min_level = Level::Info);

  void debug(const std::string& message);
  void info(const std::string& message);
  void warning(const std::string& message);
  void error(const std::string& message);
  void log(Level level, const std::string& message);

  [[nodiscard]] static Level parse_or(Level fallback, const std::string& text);

 private:
  static const char* to_string(Level level);

  Level min_level_;
  std::mutex mutex_;
};

}  // namespace apex::observability
