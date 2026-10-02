#pragma once

// Application configuration. Every value has a compiled-in default, can be
// overridden with an APEX_* environment variable, and is validated before
// the server is allowed to start. No credentials are hard-coded: anything
// secret must arrive via the environment.
//
// Supported variables:
//   APEX_PORT            TCP port to listen on.            Default: 8080
//                        0 is accepted and means "let the OS pick" — this
//                        exists for tests, which need ephemeral ports.
//   APEX_THREADS         I/O thread-pool size (>= 1).      Default: hardware
//                                                      concurrency, min 2.
//   APEX_POSTGRES_HOST   Durable-state host (TCP-checked). Default: 127.0.0.1
//   APEX_POSTGRES_PORT   Durable-state port.               Default: 5432
//   APEX_POSTGRES_USER   Durable-state user.               Default: apex
//   APEX_POSTGRES_PASSWORD Durable-state password.        Default: "" (empty:
//                        no password sent; suitable for trust auth. Set this
//                        in real environments — it is never logged.)
//   APEX_POSTGRES_DB     Durable-state database name.      Default: apex
//   APEX_DB_POOL_SIZE    Blocking PG connection pool size. Default: 8 (1..64)
//   APEX_MIGRATIONS_DIR  Directory holding V00N__*.sql.    Default: migrations
//                        (relative to the server working directory)
//   APEX_REDIS_HOST      Coordination host (TCP-checked).  Default: 127.0.0.1
//   APEX_REDIS_PORT      Coordination port.                Default: 6379
//   APEX_REDIS_PASSWORD  Coordination password.            Default: "" (empty:
//                        no password sent; never logged)
//   APEX_REDIS_POOL_SIZE Redis connection pool size.       Default: 8 (1..64)
//   APEX_LEASE_TTL_MS    Lease ownership window in ms.     Default: 10000
//                        (1000..300000). NOT a correctness mechanism: TTL
//                        expiry is a liveness hint; fencing epochs decide
//                        ownership (INV-09, INV-18).
//   APEX_REDIS_OP_TIMEOUT_MS Per-command Redis deadline.   Default: 2000
//                        (100..60000). Bounds every lease operation so a dead
//                        Redis delays but never hangs a worker.
//   APEX_WAITER_TIMEOUT_MS Max time one request waits for another      Default: 30000
//                        generation's completion (1000..300000). Expiry answers
//                        202 WITHOUT touching durable state (INV-MUX-06).
//   APEX_WAITER_RECHECK_MS Fallback durable re-check interval.        Default: 1000
//                        (100..30000). Pub/sub wakes arrive sooner when delivery
//                        works; this bounds missed-notification recovery.
//   APEX_MAX_WAITERS_PER_KEY Per-operation waiter cap (DoS bound).    Default: 1024
//                        (1..100000). Excess waiters answer 202 immediately.
//   APEX_LOG_LEVEL       debug|info|warning|error.          Default: info

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace apex::config {

struct Config {
  std::uint16_t port{8080};
  unsigned threads{2};
  std::string postgres_host{"127.0.0.1"};
  std::uint16_t postgres_port{5432};
  std::string postgres_user{"apex"};
  std::string postgres_password;
  std::string postgres_db{"apex"};
  unsigned db_pool_size{8};
  std::string migrations_dir{"migrations"};
  std::string redis_host{"127.0.0.1"};
  std::uint16_t redis_port{6379};
  std::string redis_password;
  unsigned redis_pool_size{8};
  unsigned lease_ttl_ms{10000};
  unsigned redis_op_timeout_ms{2000};
  unsigned waiter_timeout_ms{30000};
  unsigned waiter_recheck_ms{1000};
  unsigned max_waiters_per_key{1024};
  std::string log_level{"info"};

  // Baseline defaults (threads are fixed up to the hardware default).
  static Config defaults();

  // Best-effort load: unparseable values fall back to defaults and produce
  // one warning string each. The server logs warnings but still starts;
  // hard failures are reported by validate().
  static std::pair<Config, std::vector<std::string>> load_from_environment();

  // Returns "" when the configuration is usable, otherwise a
  // human-readable reason. The server refuses to start when invalid.
  [[nodiscard]] std::string validate() const;

  // libpq keyword/value connection string for the idempotency database.
  // The password is included only when non-empty. Never logged: callers
  // must treat the result as a secret.
  [[nodiscard]] std::string postgres_conninfo() const;
};

}  // namespace apex::config
