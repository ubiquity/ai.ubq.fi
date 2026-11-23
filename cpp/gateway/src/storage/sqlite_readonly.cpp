#include "uos/storage/sqlite_readonly.hpp"

#include "uos/rt/fs.hpp"
#include "uos/rt/url.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace uos::storage::sqlite_readonly {
  // ---- bun:sqlite over uos::rt::sqlite --------------------------------------------------

  Database::Statement::Statement(rt::sqlite::Statement statement_) : statement(std::move(statement_)) {}

  std::optional<json::object> Database::Statement::get() const {
    const std::optional<rt::sqlite::Row> row = statement.get();
    if (!row) return std::nullopt;
    json::object object;
    for (const auto& [name, value] : *row) {
      if (std::holds_alternative<rt::sqlite::Bytes>(value)) throw std::logic_error("bun:sqlite: a BLOB column value (Uint8Array) has no json::value form");
      object.emplace(name, rt::sqlite::toJson(value));
    }
    return object;
  }

  namespace {
    rt::sqlite::DatabaseOptions rtOptions(const DatabaseOptions& options) {
      rt::sqlite::DatabaseOptions converted;
      if (options.readonly) converted.readonly = true;
      if (options.create) converted.create = true;
      if (options.readwrite) converted.readwrite = true;
      if (options.strict) converted.strict = true;
      return converted;
    }
  }

  Database::Database(std::string_view filename) : database(filename) {}

  Database::Database(std::string_view filename, const DatabaseOptions& options) : database(filename, rtOptions(options)) {}

  Database::Statement Database::query(std::string_view sql) { return Statement(database.query(sql)); }

  Database::Changes Database::run(std::string_view sql) {
    const rt::sqlite::Changes changes = database.run(sql);
    return {.changes = changes.changes, .lastInsertRowid = std::get<double>(changes.lastInsertRowid)};
  }

  std::optional<int> Database::fileControl(int op, int value) { return database.fileControl(op, value); }

  void Database::close(bool throwOnError) { database.close(throwOnError); }

  // ---- src/storage/sqlite-readonly.ts ------------------------------------------------

  namespace {
    std::string fileState(const std::string& path) {
      const rt::fs::Stats info = rt::fs::lstatSync(path);
      if (!info.isFile() || info.isSymbolicLink()) throw rt::JsError("Read-only SQLite inspection requires a regular file and precise change timestamps");
      return std::to_string(info.dev) + ":" + std::to_string(info.ino) + ":" + std::to_string(info.size) + ":" + std::to_string(info.mtimeNs) + ":" + std::to_string(info.ctimeNs);
    }

    void requireNoSidecars(const std::string& path) {
      for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        try {
          rt::fs::lstatSync(path + suffix);
          throw rt::JsError("Read-only SQLite fallback refuses WAL, shared-memory or rollback-journal files");
        } catch (const rt::fs::ErrnoError& error) {
          if (error.code != "ENOENT") throw;
        }
      }
    }

    void inspect(std::string_view path, const std::function<void(Database&)>& read) {
      DatabaseOptions options;
      options.readonly = true;
      options.strict = true;
      Database database(path, options);
      std::exception_ptr failure;
      try {
        read(database);
      } catch (...) {
        failure = std::current_exception();
      }
      database.close(true);
      if (failure) std::rethrow_exception(failure);
    }
  }

  namespace detail {
    void inspectSqliteReadOnly(std::string_view path, const std::function<void(Database&)>& read) {
      try {
        inspect(path, read);
        return;
      } catch (const SQLiteError& error) {
        if (error.code != "SQLITE_CANTOPEN") throw;
      }
      const std::string physical = rt::path::resolve(path);
      requireNoSidecars(physical);
      const std::string before = fileState(physical);
      rt::URL uri = rt::pathToFileURL(physical);
      uri.searchParams().set("immutable", "1");
      inspect(uri.href(), read);
      // The immutable connection is already closed: never expose it beyond this synchronous fence.
      requireNoSidecars(physical);
      if (fileState(physical) != before) throw rt::JsError("SQLite source changed during read-only inspection");
    }
  }
}
