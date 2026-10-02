#pragma once

// Minimal RAII wrapper over libpq. Blocking by design: every method may wait
// on the network, so PgConnection values must only ever be used on database
// worker threads — never on a Boost.Asio I/O thread (INV-01). The pool in
// ConnectionPool.hpp is what keeps connections off the I/O path.
//
// All statements go through exec_params with $N placeholders: no SQL string
// interpolation anywhere in the codebase. Errors surface as PgError carrying
// the server message and SQLSTATE.

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <libpq-fe.h>

namespace apex::persistence {

class PgError : public std::runtime_error {
 public:
  explicit PgError(const std::string& message, std::string sqlstate = "")
      : std::runtime_error(message), sqlstate_(std::move(sqlstate)) {}

  [[nodiscard]] const std::string& sqlstate() const noexcept { return sqlstate_; }

 private:
  std::string sqlstate_;
};

class PgResult {
 public:
  explicit PgResult(PGresult* result);
  ~PgResult();

  PgResult(const PgResult&) = delete;
  PgResult& operator=(const PgResult&) = delete;
  PgResult(PgResult&&) noexcept;
  PgResult& operator=(PgResult&&) noexcept;

  [[nodiscard]] PGresult* get() const noexcept { return result_.get(); }
  [[nodiscard]] int rows() const;
  [[nodiscard]] std::string value(int row, int col) const;
  [[nodiscard]] bool value_is_null(int row, int col) const;
  // Rows affected by a non-SELECT command ("0", "1", ...).
  [[nodiscard]] std::string command_tuples() const;

 private:
  struct Releaser {
    void operator()(PGresult* result) const noexcept;
  };
  std::unique_ptr<PGresult, Releaser> result_;
};

class PgConnection {
 public:
  // Opens a connection with PQconnectdb. Throws PgError when the server is
  // unreachable or rejects the credentials — callers map this to 503.
  explicit PgConnection(const std::string& conninfo);
  ~PgConnection();

  PgConnection(const PgConnection&) = delete;
  PgConnection& operator=(const PgConnection&) = delete;
  PgConnection(PgConnection&&) noexcept;
  PgConnection& operator=(PgConnection&&) noexcept;

  [[nodiscard]] bool alive() const;
  // Simple-protocol execution (PQexec): used for BEGIN/COMMIT/ROLLBACK and
  // for multi-statement schema scripts. Never for user-influenced SQL.
  [[nodiscard]] PgResult exec(const char* sql) const;
  // Parameterized execution (PQexecParams, text format): the ONLY path for
  // statements carrying key/fingerprint/payload values.
  [[nodiscard]] PgResult exec_params(const char* sql,
                                     const std::vector<std::string>& params) const;

 private:
  PGconn* conn_{nullptr};
};

}  // namespace apex::persistence
