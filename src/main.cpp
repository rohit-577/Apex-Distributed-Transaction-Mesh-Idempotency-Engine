// Apex gateway entry point.
//
// Startup sequence: load config -> log warnings -> validate (exit 2 on
// failure) -> build the database pool + idempotency service (lazy: a dead
// database degrades to 503s, not a refusal to boot) -> best-effort schema
// ensure -> bind/listen (exit 1 with a message on failure) -> run the I/O
// thread pool -> graceful shutdown on SIGINT/SIGTERM.
//
// Shutdown sequence: stop the acceptor (no new connections) -> stop the
// io_context -> join I/O workers -> join database workers -> close the pool.
// Every stage outlives its users: sessions finish before the ioc dies, and
// pool guards finish before the pool closes. Exit codes: 0 clean,
// 1 runtime failure, 2 invalid configuration.

#include <csignal>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>

#include "config/Config.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "execution/HttpServer.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/OperationExecutor.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/PgConnection.hpp"
#include "persistence/Schema.hpp"

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

  // Durable idempotency wiring. The pool opens connections lazily (see
  // ConnectionPool), so constructing it never touches the network: a dead
  // database at boot means 503s on POST /v1/operations, not exit(1).
  // /health and /ready keep their Phase 0 behavior regardless.
  auto pool = std::make_shared<apex::persistence::ConnectionPool>(config.postgres_conninfo(),
                                                                  config.db_pool_size);

  // Lease coordination (single Redis instance — NOT Redlock, documented in
  // docs/lease-and-fencing.md). redis-plus-plus connects lazily, so this
  // never touches the network either: a dead Redis degrades to per-request
  // 503s on ownership paths, while COMPLETED replays keep working.
  apex::coordination::RedisEndpoint redis_endpoint;
  redis_endpoint.host = config.redis_host;
  redis_endpoint.port = config.redis_port;
  redis_endpoint.password = config.redis_password;
  redis_endpoint.connect_timeout = std::chrono::milliseconds(config.redis_op_timeout_ms);
  redis_endpoint.command_timeout = std::chrono::milliseconds(config.redis_op_timeout_ms);
  redis_endpoint.pool_size = config.redis_pool_size;
  auto redis =
      std::make_shared<apex::coordination::RedisClient>(std::move(redis_endpoint));
  auto leases = std::make_shared<apex::coordination::LeaseManager>(
      redis, std::chrono::milliseconds(config.lease_ttl_ms), logger);

  auto service_deps = apex::idempotency::ServiceDependencies{};
  service_deps.pool = pool;
  service_deps.leases = leases;
  service_deps.executor = std::make_shared<apex::idempotency::SimulatedExecutor>();
  service_deps.registry = std::make_shared<apex::idempotency::WaiterRegistry>(
      apex::idempotency::WaiterOptions{
          static_cast<long long>(config.waiter_timeout_ms),
          static_cast<long long>(config.waiter_recheck_ms), config.max_waiters_per_key});
  service_deps.redis = redis;
  auto service =
      std::make_shared<apex::idempotency::IdempotencyService>(std::move(service_deps), logger);
  boost::asio::thread_pool db_pool(config.db_pool_size);

  // Best-effort schema ensure on the MAIN thread (blocking is fine here —
  // no I/O loop exists yet). Uses one short-lived connection, not the
  // pool, so a dead database costs at most connect_timeout seconds.
  // Migrations are idempotent; the single source of truth is migrations/.
  try {
    apex::persistence::PgConnection bootstrap(config.postgres_conninfo());
    apex::persistence::Schema::ensure(bootstrap, config.migrations_dir);
    logger.info("idempotency schema ensured (" +
                std::string(apex::persistence::Schema::kMigrationFile) + ")");
  } catch (const std::exception& e) {
    logger.warning(std::string("schema ensure skipped (database unreachable?): ") + e.what());
  }

  // Cross-node waiter wake-ups (best-effort pub/sub; correctness never
  // depends on it). Started after the schema ensure so its thread never
  // outlives the registry it dispatches to (stopped explicitly below).
  apex::coordination::CompletionSubscriber subscriber(redis, *service->registry(), logger);
  subscriber.start();

  HttpServer server(ioc, config, APEX_VERSION, APEX_PHASE, service, &db_pool, logger);
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
    // Wake waiters with 202 while the loop still runs (their deferred
    // responses need a live io_context), then stop the subscriber thread
    // before anything it touches can die.
    service->registry()->shutdown();
    subscriber.stop();
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
  // Drain order matters: the I/O loop is stopped, so no new database work
  // can be posted; joining the pool waits out the work already posted (its
  // completions post back to the stopped ioc and are dropped safely); only
  // then is the pool closed, so no Guard outlives it.
  db_pool.join();
  pool->close();

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
