#pragma once

// Schema management: ordered, idempotent migration files.
//
// Single source of truth: migrations/V00N__*.sql in filename order
// (kMigrations). ensure() reads and applies each in turn; every statement in
// every file is IF NOT EXISTS-safe, so applying twice (server startup + test
// fixtures, or concurrent test binaries) is a harmless no-op. There is
// deliberately no version-tracking table: with a short ordered list,
// "apply in order, each idempotent" is the whole runner. A versioned runner
// arrives when the migration count justifies it.

#include <string>

namespace apex::persistence {

class PgConnection;

class Schema {
 public:
  // Ordered migration list. Append-only: never reorder, never edit an
  // applied migration in place — new changes get a new file.
  static constexpr const char* kMigrationV001 = "V001__idempotency_records.sql";
  static constexpr const char* kMigrationV002 = "V002__fencing_epoch.sql";
  static constexpr const char* kMigrationV003 = "V003__recovery_support.sql";
  // Kept for log lines that name the latest migration.
  static constexpr const char* kMigrationFile = kMigrationV003;

  // Reads <dir>/<file>. Throws PgError when missing/unreadable/empty (fail
  // fast: running without the schema turns every request into a 503).
  [[nodiscard]] static std::string read_migration_file(const std::string& dir,
                                                       const char* file);

  // Executes one migration script (multi-statement simple-protocol exec).
  static void apply(PgConnection& db, const std::string& sql);

  // Applies every migration in order. Idempotent: safe to call on every
  // boot and from every test fixture.
  static void ensure(PgConnection& db, const std::string& dir);
};

}  // namespace apex::persistence
