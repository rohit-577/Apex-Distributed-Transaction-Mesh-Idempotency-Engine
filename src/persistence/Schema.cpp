#include "persistence/Schema.hpp"

#include <array>
#include <fstream>
#include <sstream>

#include "persistence/PgConnection.hpp"

namespace apex::persistence {

std::string Schema::read_migration_file(const std::string& dir, const char* file) {
  const std::string path = dir + "/" + file;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw PgError("Migration file not found or unreadable: " + path +
                  " (APEX_MIGRATIONS_DIR must point at migrations/)");
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  if (contents.str().empty()) {
    throw PgError("Migration file is empty: " + path);
  }
  return contents.str();
}

void Schema::apply(PgConnection& db, const std::string& sql) {
  // The script is trusted project source (not user input), so plain exec is
  // correct here; parameterized exec cannot run multi-statement scripts.
  // (void): schema scripts return command-ok by contract.
  (void)db.exec(sql.c_str());
}

void Schema::ensure(PgConnection& db, const std::string& dir) {
  // Ordered, append-only. Each file is individually idempotent; the sequence
  // is therefore idempotent too.
  constexpr std::array<const char*, 3> kOrdered = {kMigrationV001, kMigrationV002,
                                                   kMigrationV003};
  for (const char* file : kOrdered) {
    apply(db, read_migration_file(dir, file));
  }
}

}  // namespace apex::persistence
