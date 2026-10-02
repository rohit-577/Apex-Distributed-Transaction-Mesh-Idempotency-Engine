#include "persistence/PgConnection.hpp"

#include <libpq-fe.h>

namespace apex::persistence {

namespace {

std::string sqlstate_of(const PGresult* result) {
  const char* code = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  return code != nullptr ? std::string(code) : std::string();
}

[[noreturn]] void throw_for_result(PGresult* raw, const std::string& context) {
  const char* raw_message = PQresultErrorMessage(raw);
  std::string message = context + ": " + (raw_message != nullptr ? raw_message : "unknown error");
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  const std::string state = sqlstate_of(raw);
  PQclear(raw);
  throw PgError(message, state);
}

}  // namespace

void PgResult::Releaser::operator()(PGresult* result) const noexcept { PQclear(result); }

PgResult::PgResult(PGresult* result) : result_(result) {}

PgResult::~PgResult() = default;

PgResult::PgResult(PgResult&&) noexcept = default;
PgResult& PgResult::operator=(PgResult&&) noexcept = default;

int PgResult::rows() const { return PQntuples(result_.get()); }

std::string PgResult::value(int row, int col) const {
  const char* v = PQgetvalue(result_.get(), row, col);
  return v != nullptr ? std::string(v) : std::string();
}

bool PgResult::value_is_null(int row, int col) const {
  return PQgetisnull(result_.get(), row, col) != 0;
}

std::string PgResult::command_tuples() const {
  const char* v = PQcmdTuples(result_.get());
  return v != nullptr ? std::string(v) : std::string("0");
}

PgConnection::PgConnection(const std::string& conninfo) : conn_(PQconnectdb(conninfo.c_str())) {
  if (conn_ == nullptr) {
    throw PgError("PQconnectdb failed: out of memory");
  }
  if (PQstatus(conn_) != CONNECTION_OK) {
    PGconn* doomed = conn_;
    conn_ = nullptr;
    const std::string message = PQerrorMessage(doomed);
    PQfinish(doomed);
    std::string trimmed = "PostgreSQL connection failed: " + message;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
      trimmed.pop_back();
    }
    throw PgError(trimmed);
  }
}

PgConnection::~PgConnection() {
  if (conn_ != nullptr) {
    PQfinish(conn_);
  }
}

PgConnection::PgConnection(PgConnection&& other) noexcept : conn_(other.conn_) {
  other.conn_ = nullptr;
}

PgConnection& PgConnection::operator=(PgConnection&& other) noexcept {
  if (this != &other) {
    if (conn_ != nullptr) {
      PQfinish(conn_);
    }
    conn_ = other.conn_;
    other.conn_ = nullptr;
  }
  return *this;
}

bool PgConnection::alive() const {
  return conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK;
}

PgResult PgConnection::exec(const char* sql) const {
  PGresult* raw = PQexec(conn_, sql);
  if (raw == nullptr) {
    throw PgError("PQexec failed: out of memory");
  }
  const ExecStatusType status = PQresultStatus(raw);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    throw_for_result(raw, "PostgreSQL command failed");
  }
  return PgResult(raw);
}

PgResult PgConnection::exec_params(const char* sql,
                                   const std::vector<std::string>& params) const {
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const std::string& param : params) {
    values.push_back(param.c_str());
  }
  PGresult* raw =
      PQexecParams(conn_, sql, static_cast<int>(values.size()),
                   /*paramTypes=*/nullptr, values.data(),
                   /*paramLengths=*/nullptr, /*paramFormats=*/nullptr, /*resultFormat=*/0);
  if (raw == nullptr) {
    throw PgError("PQexecParams failed: out of memory");
  }
  const ExecStatusType status = PQresultStatus(raw);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    throw_for_result(raw, "PostgreSQL command failed");
  }
  return PgResult(raw);
}

}  // namespace apex::persistence
