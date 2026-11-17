#pragma once
#include "uos/rt/error.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/sqlite.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uos::storage::sqlite_readonly {
  // ---- `import { Database } from "bun:sqlite"` ------------------------------------
  // The part of bun:sqlite that this module, its caller (src/storage/kv.ts verifyCodec)
  // and its tests use, over uos::rt::sqlite (docs/rt/sqlite.md): rows read as
  // json::object, the options object with booleans.

  // bun:sqlite's SQLiteError (name "SQLiteError", `code`, `errno`, `byteOffset`).
  using SQLiteError = rt::sqlite::SQLiteError;

  // The `options` object of `new Database(filename, options)`; a field that is
  // false is a property the object does not have.
  struct DatabaseOptions {
    bool readonly = false;
    bool create = false;
    bool readwrite = false;
    bool strict = false;
  };

  // `import { constants } from "bun:sqlite"`
  namespace constants {
    using rt::sqlite::constants::SQLITE_FCNTL_PERSIST_WAL;
  }

  class Database {
  public:
    // `db.query(sql)`: the statement, prepared once per SQL text and cached on
    // the database (copies are the same statement).
    class Statement {
    public:
      explicit Statement(rt::sqlite::Statement statement_);
      // `statement.get()`: the first row as an object keyed by column name (TEXT
      // a string, INTEGER and REAL a number, NULL null), or std::nullopt for
      // `null` when there is none. A BLOB column throws std::logic_error: a
      // Uint8Array has no json::value form.
      std::optional<json::object> get() const;

    private:
      rt::sqlite::Statement statement;
    };

    // `db.run(sql)`: `{ changes, lastInsertRowid }`.
    struct Changes {
      double changes = 0;
      double lastInsertRowid = 0;
    };

    // `new Database(filename)`: read-write, created when missing.
    explicit Database(std::string_view filename);
    // `new Database(filename, options)`: SQLITE_OPEN_READONLY for `readonly`,
    // read-write-create for `create`, read-write added by `readwrite`; with only
    // `strict`, read-write-create. Error "Cannot open an anonymous database in
    // read-only mode." for "" or ":memory:" with `readonly`; SQLiteError when
    // SQLite cannot open the file. The filename is trimmed, as Bun does.
    Database(std::string_view filename, const DatabaseOptions& options);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // `db.query(sql)`: Error "SQL query cannot be empty." for ""; RangeError
    // "Cannot use a closed database" after close(); SQLiteError for invalid SQL.
    // Only the first statement of the text is prepared.
    Statement query(std::string_view sql);
    // `db.run(sql)`: runs every statement of the text; Error "Database has
    // closed" after close().
    Changes run(std::string_view sql);
    // `db.fileControl(op, value)`: SQLite's result code; std::nullopt (undefined)
    // after close().
    std::optional<int> fileControl(int op, int value);
    // `db.close(throwOnError)`: finalizes the cached statements and closes; a
    // second close does nothing.
    void close(bool throwOnError = false);

  private:
    rt::sqlite::Database database;
  };

  // ---- src/storage/sqlite-readonly.ts ------------------------------------------------

  namespace detail {
    // The module's inspection with the value-typed read callback erased.
    void inspectSqliteReadOnly(std::string_view path, const std::function<void(Database&)>& read);
  }

  /** Read live WAL normally; immutable retry is limited to a stable, sidecar-free stopped file. */
  // `T extends Scalar` (string | number | bigint | boolean | null | undefined): the
  // value `read` returns from the inspection that succeeded.
  template <class Read>
    requires std::is_invocable_v<Read&, Database&>
  std::invoke_result_t<Read&, Database&> inspectSqliteReadOnly(std::string_view path, Read&& read) {
    using T = std::invoke_result_t<Read&, Database&>;
    if constexpr (std::is_void_v<T>) {
      detail::inspectSqliteReadOnly(path, [&read](Database& database) { read(database); });
    } else {
      std::optional<T> result;
      detail::inspectSqliteReadOnly(path, [&read, &result](Database& database) { result.emplace(read(database)); });
      return std::move(*result);
    }
  }
}
