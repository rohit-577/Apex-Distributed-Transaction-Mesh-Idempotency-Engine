#pragma once

// Bounded blocking connection pool for PostgreSQL.
//
// Why a pool exists at all: libpq connections are blocking and NOT safe to
// share between threads, while the gateway serves many concurrent requests.
// The pool gives each database worker thread exclusive ownership of one
// connection for the duration of an operation, then takes it back.
//
// What the pool is NOT (INV-17): it holds no idempotency state and makes no
// correctness decisions. Every acquire/create/complete goes to PostgreSQL;
// the pool is pure concurrency plumbing. Losing the whole pool (close())
// only fails fast with PoolError — committed results stay in the database.
//
// Threading contract:
// - acquire() may block (on connect_timeout or waiting for a free slot).
//   Call it on database worker threads only, never on Asio I/O threads.
// - Guard destruction never blocks and never throws.
// - close() is idempotent; it unblocks current and future acquirers.

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::persistence {

class PgConnection;

class PoolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
 public:
  // `max_size` bounds total open connections (checked-out + idle). No
  // connections are opened here: creation is lazy so a dead database at
  // startup degrades to per-request 503s instead of a refusal to boot.
  ConnectionPool(std::string conninfo, std::size_t max_size);
  ~ConnectionPool();

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  class Guard {
   public:
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&&) noexcept;
    Guard& operator=(Guard&&) noexcept;
    ~Guard();

    [[nodiscard]] PgConnection& connection();

   private:
    friend class ConnectionPool;
    Guard(std::shared_ptr<class PoolState> state, std::unique_ptr<PgConnection> conn);

    std::shared_ptr<class PoolState> state_;
    std::unique_ptr<PgConnection> conn_;
  };

  // Checks out one live connection, creating it when under budget. Throws
  // PgError when connecting fails, PoolError after close().
  [[nodiscard]] Guard acquire();

  // Rejects further checkouts and wakes blocked acquirers with PoolError.
  // Idle connections are closed; checked-out ones return to a closed pool
  // and are simply destroyed. Must be called after all worker threads that
  // might call acquire() have joined (see main() shutdown sequence).
  void close();

 private:
  std::shared_ptr<class PoolState> state_;
  std::string conninfo_;
  std::size_t max_size_;
};

}  // namespace apex::persistence
