#include "persistence/Schema.hpp"

#include <fstream>
#include <sstream>

#include "persistence/PgConnection.hpp"

namespace apex::persistence {

std::string Schema::read_migration_file(const std::string& dir) {
  const std::string path = dir + "/" + kMigrationFile;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw PgError("Migration file not found or unreadable: " + path +
                  " (APEX_MIGRATIONS_DIR must point at migrations/)");
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  if (contents.str().empty()) {
    throw PgError("Migration file is empty: " + path);
  }
  return contents.str();
}

void Schema::apply(PgConnection& db, const std::string& sql) {
  // The script is trusted project source (not user input), so plain exec is
  // correct here; parameterized exec cannot run multi-statement scripts.
  db.exec(sql.c_str());
}

}  // namespace apex::persistence
