// Apex benchmark runner (Phase 6): measures real throughput/latency AND
// verifies logical execution counts on every scenario. NOT part of ctest:
// it prints measured numbers; it never asserts performance, only
// correctness (a correctness failure exits non-zero and stops the run).
//
// Requires APEX_TEST_POSTGRES_CONN + APEX_TEST_REDIS_HOST (same gating as
// tests). Usage: apex_bench [--scenario=A|B|C|D|E|F|G|soak|all]
// Key prefixes are unique per run (PID-scoped); rows are deleted afterwards
// on a best-effort basis to keep the dev database tidy.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "common/test_executor.hpp"
#include "common/test_helpers.hpp"
#include "config/Config.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/LeaseManager.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyService.hpp"
#include "idempotency/OperationExecutor.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "persistence/ConnectionPool.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"
#include "persistence/Schema.hpp"

namespace {

using namespace std::chrono_literals;
namespace http = boost::beast::http;

unsigned long this_pid() { return apex::test::test_process_id(); }

struct BenchEnv {
  std::string pg_conninfo;
  std::string redis_host;
  std::uint16_t redis_port{6379};
};

bool load_env(BenchEnv& env) {
  if (const auto pg = apex::test::pg_test_conninfo()) {
    env.pg_conninfo = *pg;
  } else {
    std::cerr << "APEX_TEST_POSTGRES_CONN is not set\n";
    return false;
  }
  if (const auto redis = apex::test::redis_test_endpoint()) {
    env.redis_host = redis->host;
    env.redis_port = redis->port;
  } else {
    std::cerr << "APEX_TEST_REDIS_HOST is not set\n";
    return false;
  }
  return true;
}

struct Node {
  apex::config::Config config;
  std::shared_ptr<apex::persistence::ConnectionPool> pool;
  apex::observability::Logger logger{apex::observability::Level::Error};
  std::shared_ptr<apex::coordination::RedisClient> redis;
  std::shared_ptr<apex::coordination::LeaseManager> leases;
  std::shared_ptr<apex::idempotency::WaiterRegistry> registry;
  std::shared_ptr<apex::test::GatedExecutor> executor;
  std::shared_ptr<apex::observability::Metrics> metrics;
  std::shared_ptr<apex::idempotency::IdempotencyService> service;
  std::unique_ptr<apex::test::TestServer> server;
  std::shared_ptr<apex::coordination::CompletionSubscriber> subscriber;
};

std::shared_ptr<Node> make_node(const BenchEnv& env, bool start_subscriber) {
  auto node = std::make_shared<Node>();
  node->config = apex::config::Config::defaults();
  node->config.port = 0;
  node->config.threads = 8;
  node->config.db_pool_size = 16;
  node->pool = std::make_shared<apex::persistence::ConnectionPool>(env.pg_conninfo, 16);
  apex::coordination::RedisEndpoint endpoint;
  endpoint.host = env.redis_host;
  endpoint.port = env.redis_port;
  endpoint.connect_timeout = 2000ms;
  endpoint.command_timeout = 2000ms;
  endpoint.pool_size = 32;
  node->redis = std::make_shared<apex::coordination::RedisClient>(std::move(endpoint));
  node->leases = std::make_shared<apex::coordination::LeaseManager>(
      node->redis, 10000ms, node->logger, std::make_shared<apex::observability::Metrics>());
  node->registry = std::make_shared<apex::idempotency::WaiterRegistry>();
  node->executor = std::make_shared<apex::test::GatedExecutor>(true);
  node->metrics = std::make_shared<apex::observability::Metrics>();
  apex::idempotency::ServiceDependencies deps;
  deps.pool = node->pool;
  deps.leases = node->leases;
  deps.executor = node->executor;
  deps.registry = node->registry;
  deps.redis = node->redis;
  deps.metrics = node->metrics;
  node->service =
      std::make_shared<apex::idempotency::IdempotencyService>(std::move(deps), node->logger);
  node->subscriber = std::make_shared<apex::coordination::CompletionSubscriber>(
      node->redis, *node->registry, node->logger, node->metrics);
  if (start_subscriber) {
    node->subscriber->start();
  }
  node->server = std::make_unique<apex::test::TestServer>(node->config, node->service);
  return node;
}

struct TimedResult {
  int status{0};
  std::string body;
  double ms{0.0};
};

TimedResult timed_post(std::uint16_t port, const std::string& key, const std::string& body) {
  const auto start = std::chrono::steady_clock::now();
  const auto result = apex::test::http_send_with_headers(
      "127.0.0.1", port, http::verb::post, "/v1/operations", body,
      {{"Idempotency-Key", key}, {"Content-Type", "application/json"}});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return {result.status, result.body,
          std::chrono::duration<double, std::milli>(elapsed).count()};
}

struct Stats {
  std::string name;
  std::size_t requests{0};
  int concurrency{0};
  double elapsed_ms{0.0};
  double rps{0.0};
  double mean_ms{0.0};
  double p50_ms{0.0};
  double p95_ms{0.0};
  double p99_ms{0.0};
  double max_ms{0.0};
};

Stats summarize(const std::string& name, const std::vector<TimedResult>& results, double elapsed_ms,
                int concurrency) {
  Stats stats;
  stats.name = name;
  stats.requests = results.size();
  stats.concurrency = concurrency;
  stats.elapsed_ms = elapsed_ms;
  stats.rps = results.empty() ? 0.0 : (1000.0 * results.size() / elapsed_ms);
  std::vector<double> latencies;
  for (const auto& result : results) {
    latencies.push_back(result.ms);
  }
  std::sort(latencies.begin(), latencies.end());
  const auto percentile = [&](double p) {
    if (latencies.empty()) {
      return 0.0;
    }
    const std::size_t index =
        std::min(latencies.size() - 1, static_cast<std::size_t>(p * latencies.size()));
    return latencies[index];
  };
  stats.mean_ms = latencies.empty()
                      ? 0.0
                      : std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                            latencies.size();
  stats.p50_ms = percentile(0.50);
  stats.p95_ms = percentile(0.95);
  stats.p99_ms = percentile(0.99);
  stats.max_ms = latencies.empty() ? 0.0 : latencies.back();
  return stats;
}

void print_stats(const Stats& stats) {
  std::printf("%-28s req=%5zu conc=%3d elapsed=%8.1fms rps=%9.1f mean=%7.2fms p50=%7.2fms "
              "p95=%7.2fms p99=%7.2fms max=%7.2fms\n",
              stats.name.c_str(), stats.requests, stats.concurrency, stats.elapsed_ms,
              stats.rps, stats.mean_ms, stats.p50_ms, stats.p95_ms, stats.p99_ms, stats.max_ms);
  std::fflush(stdout);
}

std::string unique_prefix(const std::string& scenario) {
  return "bench-" + std::to_string(this_pid()) + "-" + scenario + "-";
}

int count_rows(const BenchEnv& env, const std::string& like_prefix) {
  apex::persistence::PgConnection db(env.pg_conninfo);
  const auto result = db.exec_params(
      "SELECT count(*) FROM idempotency_records WHERE idempotency_key LIKE $1", {like_prefix});
  return std::stoi(result.value(0, 0));
}

void cleanup_prefix(const BenchEnv& env, const std::string& like_prefix) {
  try {
    apex::persistence::PgConnection db(env.pg_conninfo);
    (void)db.exec_params("DELETE FROM idempotency_records WHERE idempotency_key LIKE $1",
                         {like_prefix});
  } catch (const std::exception&) {
  }
}

// BENCHMARK A — unique keys at a ladder of concurrencies.
bool scenario_unique_keys(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  for (int concurrency : {1, 4, 16, 64}) {
    const int total = 400;
    const std::string prefix = unique_prefix("A" + std::to_string(concurrency));
    std::vector<TimedResult> results(total);
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    const auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < concurrency; ++t) {
      threads.emplace_back([&] {
        for (;;) {
          const int i = next.fetch_add(1);
          if (i >= total) {
            return;
          }
          results[i] =
              timed_post(node->server->port(), prefix + std::to_string(i), R"({"a":1})");
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    for (const auto& result : results) {
      if (result.status != 200) {
        std::printf("A/%d: unexpected status %d\n", concurrency, result.status);
        return false;
      }
    }
    if (count_rows(env, prefix + "%") != total) {
      std::printf("A/%d: row count mismatch\n", concurrency);
      return false;
    }
    print_stats(summarize("A-unique-keys", results, elapsed, concurrency));
    cleanup_prefix(env, prefix + "%");
  }
  return true;
}

// BENCHMARK B — duplicate fan-in: 200 identical, all converge, 1 execution.
bool scenario_fan_in(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  const int total = 200;
  const std::string prefix = unique_prefix("B");
  const std::string key = prefix + "fanin";
  const std::string body = R"({"fan":"in"})";
  std::vector<TimedResult> results(total);
  std::latch go{1};
  std::vector<std::thread> threads;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < total; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      results[i] = timed_post(node->server->port(), key, body);
    });
  }
  go.count_down();
  for (auto& thread : threads) {
    thread.join();
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  for (const auto& result : results) {
    if (result.status != 200 && result.status != 202) {
      std::printf("B: unexpected status %d\n", result.status);
      return false;
    }
  }
  // Converge stragglers, then verify one execution (epoch 1, identical bytes).
  const auto replay = timed_post(node->server->port(), key, body);
  if (replay.status != 200) {
    std::printf("B: final replay status %d\n", replay.status);
    return false;
  }
  apex::persistence::PgConnection db(env.pg_conninfo);
  const auto row = db.exec_params(
      "SELECT fencing_epoch, response_body FROM idempotency_records WHERE idempotency_key = $1",
      {key});
  if (row.rows() != 1 || row.value(0, 0) != "1") {
    std::printf("B: expected exactly one epoch-1 row\n");
    return false;
  }
  if (replay.body != row.value(0, 1)) {
    std::printf("B: replay diverged from stored body\n");
    return false;
  }
  print_stats(summarize("B-duplicate-fan-in", results, elapsed, total));
  std::printf("%-28s executions=1 waiters-converged=%zu\n", "B-correctness", results.size());
  cleanup_prefix(env, prefix + "%");
  return true;
}

// BENCHMARK C — completed replay latency (500 sequential).
bool scenario_replay(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  const std::string key = unique_prefix("C") + "replay";
  const std::string body = R"({"replay":1})";
  if (timed_post(node->server->port(), key, body).status != 200) {
    std::printf("C: seed failed\n");
    return false;
  }
  const int total = 500;
  std::vector<TimedResult> results;
  results.reserve(total);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < total; ++i) {
    results.push_back(timed_post(node->server->port(), key, body));
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  for (const auto& result : results) {
    if (result.status != 200) {
      std::printf("C: unexpected status %d\n", result.status);
      return false;
    }
  }
  print_stats(summarize("C-completed-replay", results, elapsed, 1));
  cleanup_prefix(env, key);
  return true;
}

// BENCHMARK D — conflict latency: one completed fp, then 200 foreign fps.
bool scenario_conflict(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  const std::string key = unique_prefix("D") + "conflict";
  if (timed_post(node->server->port(), key, R"({"v":0})").status != 200) {
    std::printf("D: seed failed\n");
    return false;
  }
  const int total = 200;
  std::vector<TimedResult> results(total);
  std::vector<std::string> sent_bodies(total);
  std::vector<std::thread> threads;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < total; ++i) {
    threads.emplace_back([&, i] {
      sent_bodies[i] = R"({"v":)" + std::to_string(i + 1) + "}";
      results[i] = timed_post(node->server->port(), key, sent_bodies[i]);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  for (int i = 0; i < total; ++i) {
    if (results[i].status != 409) {
      std::printf("D: thread %d sent %.20s got status %d body=%.100s\n", i,
                  sent_bodies[i].c_str(), results[i].status, results[i].body.c_str());
      return false;
    }
  }
  print_stats(summarize("D-conflict", results, elapsed, total));
  cleanup_prefix(env, key);
  return true;
}

// BENCHMARK E — many keys: 16 keys x 16 threads.
bool scenario_many_keys(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  const int keys = 16;
  const int per_key = 16;
  const std::string prefix = unique_prefix("E");
  std::vector<TimedResult> results(keys * per_key);
  std::latch go{1};
  std::vector<std::thread> threads;
  const auto start = std::chrono::steady_clock::now();
  for (int k = 0; k < keys; ++k) {
    for (int i = 0; i < per_key; ++i) {
      const int slot = k * per_key + i;
      threads.emplace_back([&, slot, k] {
        go.wait();
        results[slot] =
            timed_post(node->server->port(), prefix + std::to_string(k), R"({"k":1})");
      });
    }
  }
  go.count_down();
  for (auto& thread : threads) {
    thread.join();
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  for (const auto& result : results) {
    if (result.status != 200 && result.status != 202) {
      std::printf("E: unexpected status %d\n", result.status);
      return false;
    }
  }
  if (count_rows(env, prefix + "%") != keys) {
    std::printf("E: row count mismatch\n");
    return false;
  }
  print_stats(summarize("E-many-keys", results, elapsed, keys * per_key));
  cleanup_prefix(env, prefix + "%");
  return true;
}

// BENCHMARK F — cross-node: owner on A, 50 waiters on B.
bool scenario_cross_node(const BenchEnv& env, const std::shared_ptr<Node>& node_a,
                         const std::shared_ptr<Node>& node_b) {
  const std::string key = unique_prefix("F") + "xnode";
  const std::string body = R"({"x":1})";
  const int waiters = 50;
  std::vector<TimedResult> results(waiters);
  std::latch go{1};
  std::vector<std::thread> threads;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < waiters; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      results[i] = timed_post(node_b->server->port(), key, body);
    });
  }
  // Owner slightly delayed so waiters genuinely suspend (not replay).
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const TimedResult owner = timed_post(node_a->server->port(), key, body);
  go.count_down();
  for (auto& thread : threads) {
    thread.join();
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (owner.status != 200) {
    std::printf("F: owner status %d\n", owner.status);
    return false;
  }
  for (const auto& result : results) {
    if (result.status != 200 || result.body != owner.body) {
      std::printf("F: waiter diverged (status %d)\n", result.status);
      return false;
    }
  }
  print_stats(summarize("F-cross-node", results, elapsed, waiters));
  cleanup_prefix(env, key);
  return true;
}

// BENCHMARK G — recovery: 20 planted orphans, time service-level adoption.
bool scenario_recovery(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  const std::string prefix = unique_prefix("G");
  {
    apex::persistence::PgConnection db(env.pg_conninfo);
    apex::persistence::IdempotencyRepository repo;
    for (int i = 0; i < 20; ++i) {
      const std::string body = R"({"g":)" + std::to_string(i) + "}";
      const auto fp = apex::idempotency::fingerprint_for("POST", "/v1/operations", body);
      const auto created =
          repo.try_acquire(db, prefix + std::to_string(i), fp.hex, fp.canonical_body);
      if (created.outcome != apex::persistence::AcquireOutcome::Created) {
        std::printf("G: plant failed\n");
        return false;
      }
    }
  }
  std::vector<double> samples;
  for (int i = 0; i < 20; ++i) {
    const std::string key = prefix + std::to_string(i);
    const std::string body = R"({"g":)" + std::to_string(i) + "}";
    const auto start = std::chrono::steady_clock::now();
    const auto result = timed_post(node->server->port(), key, body);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    if (result.status != 200) {
      std::printf("G: recovery status %d\n", result.status);
      return false;
    }
    samples.push_back(ms);
  }
  std::sort(samples.begin(), samples.end());
  std::printf("%-28s n=20 mean=%7.2fms p50=%7.2fms p95=%7.2fms max=%7.2fms recovered=20/20\n",
              "G-recovery", std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size(),
              samples[10], samples[19], samples.back());
  cleanup_prefix(env, prefix + "%");
  return true;
}

// Soak: sustained mixed workload (unique + replay + small fan-in rounds)
// for a bounded duration. Asserts zero unexpected statuses throughout;
// memory/connection behavior is sampled externally (see scripts/bench.ps1).
bool scenario_soak(const BenchEnv& env, const std::shared_ptr<Node>& node, int seconds) {
  const std::string prefix = unique_prefix("soak");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::uint64_t rounds = 0;
  std::uint64_t errors = 0;
  int n = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const std::string key = prefix + std::to_string(n++);
    const std::string body = R"({"soak":1})";
    const auto first = timed_post(node->server->port(), key, body);
    const auto replay = timed_post(node->server->port(), key, body);
    if (first.status != 200 || replay.status != 200 || replay.body != first.body) {
      ++errors;
    }
    ++rounds;
  }
  std::printf("%-28s rounds=%llu errors=%llu duration=%ds\n", "soak", rounds, errors, seconds);
  cleanup_prefix(env, prefix + "%");
  return errors == 0;
}

// BENCHMARK H — fan-in across a Redis restart: gated owner + 50 waiters
// suspended, Redis restarted mid-wait, gate opens. All converge on one
// execution. The restart is guaranteed to overlap the wait: the gate opens
// only after the container is observed stopped AND healthy again.
bool scenario_restart_midwait(const BenchEnv& env, const std::shared_ptr<Node>& node) {
  const std::string key = unique_prefix("H") + "restart";
  const std::string body = R"({"h":1})";
  node->executor->close_gate();
  const int waiters = 50;
  std::vector<TimedResult> results(waiters);
  int owner_status = 0;
  std::string owner_body;
  std::thread owner([&] {
    const auto result = timed_post(node->server->port(), key, body);
    owner_status = result.status;
    owner_body = result.body;
  });
  auto executions_is = [&](std::uint64_t n) { return node->executor->executions() == n; };
  const auto start = std::chrono::steady_clock::now();
  while (!executions_is(1) &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!executions_is(1)) {
    std::printf("H: owner never reached execution\n");
    node->executor->open_gate();
    owner.join();
    return false;
  }
  std::latch go{1};
  std::vector<std::thread> threads;
  for (int i = 0; i < waiters; ++i) {
    threads.emplace_back([&, i] {
      go.wait();
      results[i] = timed_post(node->server->port(), key, body);
    });
  }
  go.count_down();
  // Wait until every waiter registered (all suspended on live PROCESSING).
  const auto reg_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  std::size_t registered = 0;
  const auto fp = apex::idempotency::fingerprint_for("POST", "/v1/operations", body);
  const std::string channel =
      apex::idempotency::WaiterRegistry::channel_for(key, fp.ok ? fp.hex : "");
  while (std::chrono::steady_clock::now() < reg_deadline) {
    registered = node->registry->waiter_count(channel);
    if (registered == static_cast<std::size_t>(waiters)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (registered != static_cast<std::size_t>(waiters)) {
    std::printf("H: only %zu/%d waiters registered\n", registered, waiters);
    node->executor->open_gate();
    owner.join();
    for (auto& thread : threads) {
      thread.join();
    }
    return false;
  }
  std::printf("H: waiters-registered, restarting redis\n");
  std::fflush(stdout);
  // Restart Redis mid-wait. The gate stays closed throughout: it opens only
  // after Redis is healthy again, so the outage necessarily overlaps the
  // suspended wait (waiters outlive it via fallback re-checks; deadline is
  // 30 s, restart takes ~10 s).
  if (std::system("docker compose -f \"" APEX_COMPOSE_FILE "\" restart redis") != 0) {
    std::printf("H: could not restart redis\n");
    node->executor->open_gate();
    owner.join();
    for (auto& thread : threads) {
      thread.join();
    }
    return false;
  }
  const auto up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  bool healthy = false;
  while (std::chrono::steady_clock::now() < up_deadline) {
    if (std::system("docker compose -f \"" APEX_COMPOSE_FILE
                    "\" exec -T redis redis-cli ping > NUL 2>&1") == 0) {
      healthy = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  node->executor->open_gate();
  std::printf("H: gate-open\n");
  std::fflush(stdout);
  owner.join();
  for (auto& thread : threads) {
    thread.join();
  }
  if (!healthy) {
    std::printf("H: redis never became healthy\n");
    return false;
  }
  if (owner_status != 200) {
    std::printf("H: owner status %d\n", owner_status);
    return false;
  }
  for (const auto& result : results) {
    if (result.status != 200 || result.body != owner_body) {
      std::printf("H: waiter diverged (status %d)\n", result.status);
      return false;
    }
  }
  if (node->executor->executions() != 1) {
    std::printf("H: executions != 1\n");
    return false;
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  print_stats(summarize("H-restart-midwait", results, elapsed, waiters));
  cleanup_prefix(env, key);
  return true;
}

void print_environment() {
  std::printf("== apex_bench environment ==\n");
#if defined(_WIN32)
  std::printf("os=Windows config=RelWithDebInfo-equivalent(Release)\n");
#else
  std::printf("os=POSIX\n");
#endif
  std::printf("note: exact CPU/RAM/compiler versions are recorded in docs/benchmarking.md\n");
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenario = "all";
  int soak_seconds = 0;
  bool no_subscriber = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--scenario=", 0) == 0) {
      scenario = arg.substr(11);
    } else if (arg.rfind("--soak-seconds=", 0) == 0) {
      soak_seconds = std::stoi(arg.substr(15));
    } else if (arg == "--no-subscriber") {
      no_subscriber = true;
    } else {
      std::printf("usage: apex_bench [--scenario=A|B|C|D|E|F|G|H|soak|all] [--soak-seconds=N] "
                  "[--no-subscriber]\n");
      return 2;
    }
  }
  BenchEnv env;
  if (!load_env(env)) {
    return 2;
  }
  print_environment();

  // Schema ensured (idempotent) so benchmarks run on any database state.
  try {
    apex::persistence::PgConnection bootstrap(env.pg_conninfo);
    apex::persistence::Schema::ensure(bootstrap, APEX_MIGRATIONS_DIR);
  } catch (const std::exception& e) {
    std::printf("schema ensure failed: %s\n", e.what());
    return 1;
  }

  auto node = make_node(env, /*start_subscriber=*/!no_subscriber);
  auto node_b = make_node(env, /*start_subscriber=*/!no_subscriber);

  bool ok = true;
  const auto run_single = [&](const std::string& name,
                              bool (*fn)(const BenchEnv&, const std::shared_ptr<Node>&)) {
    if (scenario != "all" && scenario != name) {
      return;
    }
    std::printf("--- scenario %s ---\n", name.c_str());
    ok = fn(env, node) && ok;
    if (!ok) {
      std::printf("SCENARIO %s FAILED correctness gate: STOPPING\n", name.c_str());
    }
  };
  run_single("A", scenario_unique_keys);
  run_single("B", scenario_fan_in);
  run_single("C", scenario_replay);
  run_single("D", scenario_conflict);
  run_single("E", scenario_many_keys);
  run_single("G", scenario_recovery);
  if (scenario == "all" || scenario == "F") {
    std::printf("--- scenario F ---\n");
    ok = scenario_cross_node(env, node, node_b) && ok;
    if (!ok) {
      std::printf("SCENARIO F FAILED correctness gate: STOPPING\n");
    }
  }
  if (scenario == "H") {
    std::printf("--- scenario H (docker-gated) ---\n");
    ok = scenario_restart_midwait(env, node) && ok;
    if (!ok) {
      std::printf("SCENARIO H FAILED correctness gate: STOPPING\n");
    }
  }
  if (scenario == "soak" || (scenario == "all" && soak_seconds > 0)) {
    std::printf("--- soak (%ds) ---\n", soak_seconds > 0 ? soak_seconds : 60);
    ok = scenario_soak(env, node, soak_seconds > 0 ? soak_seconds : 60) && ok;
    if (!ok) {
      std::printf("SOAK FAILED correctness gate: STOPPING\n");
    }
  }
  std::printf(ok ? "ALL SCENARIOS PASS\n" : "BENCHMARK FAILED\n");
  return ok ? 0 : 1;
}
