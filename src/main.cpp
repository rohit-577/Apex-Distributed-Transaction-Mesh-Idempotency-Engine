// Apex baseline gateway entry point.
//
// Startup sequence: load config -> log warnings -> validate (exit 2 on
// failure) -> bind/listen (exit 1 with a message on failure) -> run the I/O
// thread pool -> graceful shutdown on SIGINT/SIGTERM.
//
// Shutdown sequence: stop the acceptor first (no new connections), then stop
// the io_context and join every worker so in-flight handlers complete before
// any destructor runs. Exit codes: 0 clean, 1 runtime failure, 2 invalid
// configuration.

#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "config/Config.hpp"
#include "execution/HttpServer.hpp"
#include "observability/Logger.hpp"

namespace {

int run() {
  using apex::config::Config;
  using apex::execution::HttpServer;
  using apex::observability::Level;
  using apex::observability::Logger;

  auto [config, warnings] = Config::load_from_environment();
  Logger logger(Logger::parse_or(Level::Info, config.log_level));
  for (const std::string& warning : warnings) {
    logger.warning(warning);
  }

  if (const std::string error = config.validate(); !error.empty()) {
    logger.error("Invalid configuration: " + error);
    return 2;
  }

  boost::asio::io_context ioc{static_cast<int>(config.threads)};
  HttpServer server(ioc, config, APEX_VERSION);
  server.start();

  logger.info("apex " + std::string(APEX_VERSION) + " listening on 0.0.0.0:" +
              std::to_string(server.port()) + " with " + std::to_string(config.threads) +
              " io threads");
  logger.info("dependencies: postgres=" + config.postgres_host + ":" +
              std::to_string(config.postgres_port) + " redis=" + config.redis_host + ":" +
              std::to_string(config.redis_port));

  boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code&, int /*signal*/) {
    logger.info("shutdown signal received; draining connections");
    server.stop();
    ioc.stop();
  });

  std::vector<std::thread> workers;
  workers.reserve(config.threads > 0 ? config.threads - 1 : 0);
  for (unsigned i = 1; i < config.threads; ++i) {
    workers.emplace_back([&ioc] { ioc.run(); });
  }
  ioc.run();
  for (std::thread& worker : workers) {
    worker.join();
  }

  logger.info("shutdown complete");
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& e) {
    std::cerr << "[ERROR] apex failed to start: " << e.what() << "\n";
    return 1;
  }
}
