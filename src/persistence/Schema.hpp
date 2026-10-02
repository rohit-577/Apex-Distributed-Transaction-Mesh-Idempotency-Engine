#pragma once

// Schema management for Phase 1.
//
// Single source of truth: migrations/V001__idempotency_records.sql.
// apply_file() reads that file and executes it; every statement in the file
// is IF NOT EXISTS-safe, so applying twice (server startup + test fixtures)
// is a harmless no-op. There is deliberately no version-tracking table yet:
// with exactly one migration, "does the table exist" (via the file itself)
// is the whole state. A versioned runner arrives when V002 does.

#include <string>

namespace apex::persistence {

class PgConnection;

class Schema {
 public:
  static constexpr const char* kMigrationFile = "V001__idempotency_records.sql";

  // Reads <dir>/V001__idempotency_records.sql. Throws PgError when the file
  // is missing or unreadable (fail fast: running without the schema would
  // turn every request into a confusing 503).
  [[nodiscard]] static std::string read_migration_file(const std::string& dir);

  // Executes the migration script (multi-statement simple-protocol exec).
  static void apply(PgConnection& db, const std::string& sql);
};

}  // namespace apex::persistence
