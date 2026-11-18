#include <cstdio>
#include <sqlite3.h>

static_assert(SQLITE_VERSION_NUMBER == 3054000, "The gateway CI suite requires SQLite 3.54.0 headers");

int main() {
  std::printf("sqlite3_libversion(): %s\n", sqlite3_libversion());
  return sqlite3_libversion_number() == SQLITE_VERSION_NUMBER ? 0 : 1;
}
