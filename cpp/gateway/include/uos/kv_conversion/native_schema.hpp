#pragma once
#include "uos/rt/ordered.hpp"
#include "uos/rt/sqlite.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::kv_conversion::native_schema {
  // The part of bun:sqlite (`Database`, `Statement` and the row values) this module
  // reads, as an interface: adapt() puts the uos::rt::sqlite binding behind it, test fakes
  // implement it directly.
  namespace bun_sqlite {
    // One column value as bun:sqlite returns it (rt::sqlite::Value): NULL -> null; INTEGER ->
    // bigint under safeIntegers(true) (std::int64_t holds SQLite's whole range), number
    // otherwise; REAL -> number; TEXT -> string; BLOB -> Uint8Array.
    using Value = rt::sqlite::Value;
    // One result row (rt::sqlite::Row): column name -> value, in result column order. A name
    // the row lacks reads as undefined (nullptr from get()).
    using Row = rt::sqlite::Row;
    // `statement.iterate()`: the next row on each call, std::nullopt after the last.
    using RowIterator = std::function<std::optional<Row>()>;

    // The members of bun:sqlite's `Statement` this module uses.
    class Statement {
    public:
      virtual ~Statement() = default;
      // `statement.paramsCount`
      virtual std::int64_t paramsCount() const = 0;
      // `"safeIntegers" in statement && typeof statement.safeIntegers === "function"`
      virtual bool hasSafeIntegers() const = 0;
      // `statement.safeIntegers(enabled)`
      virtual void safeIntegers(bool enabled) = 0;
      // `statement.get()`: the first row, std::nullopt for `null` (no row).
      virtual std::optional<Row> get() = 0;
      virtual std::vector<Row> all() = 0;
      virtual RowIterator iterate() = 0;
    };

    // The members of bun:sqlite's `Database` this module uses.
    class Database {
    public:
      virtual ~Database() = default;
      // `database.query(sql)` (bun:sqlite returns one cached statement per SQL text).
      virtual std::shared_ptr<Statement> query(const std::string& sql) = 0;
      // `database.close(throwOnError)`
      virtual void close(bool throwOnError) = 0;
    };

    // A real bun:sqlite `Database` behind this interface (`new Database(uri, flags)` in
    // convert.ts is `adapt(rt::sqlite::Database(uri, flags))`).
    std::shared_ptr<Database> adapt(rt::sqlite::Database database);
  }

  // Pick<Statement<TRow, []>, "get" | "all" | "iterate">. Rows are untyped: each call
  // site reads the columns it selected and checks their types where the TS does.
  struct NativeReadStatement {
    std::function<std::optional<bun_sqlite::Row>()> get{};
    std::function<std::vector<bun_sqlite::Row>()> all{};
    std::function<bun_sqlite::RowIterator()> iterate{};
  };

  // { query<TRow>(sql: string): NativeReadStatement<TRow> }
  struct NativeReader {
    std::function<NativeReadStatement(const std::string& sql)> query{};
  };

  // NativeReader & { close(): void }
  struct NativeSnapshotReader : NativeReader {
    std::function<void()> close{};
  };

  /** Numeric SQLite URI flags cannot also set Bun's database-level binding options. */
  NativeSnapshotReader createNativeSnapshotReader(std::shared_ptr<bun_sqlite::Database> database);

  // { k: Uint8Array; v: Uint8Array; v_encoding: bigint; version: bigint; seq: bigint; expiration_ms: bigint }
  // as SQLite returned it: validateNativeRow checks those column types.
  using NativeRow = bun_sqlite::Row;

  // { k: bigint; version: bigint; seq: bigint }
  struct NativeRevision {
    std::int64_t k = 0;
    std::int64_t version = 0;
    std::int64_t seq = 0;
    friend bool operator==(const NativeRevision&, const NativeRevision&) = default;
  };

  inline constexpr std::array<std::string_view, 6> NATIVE_DDL{
    "CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL, v_encoding INTEGER NOT NULL, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0, expiration_ms INTEGER NOT NULL DEFAULT -1) WITHOUT ROWID",
    "CREATE INDEX kv_expiration_ms_idx ON kv (expiration_ms)",
    "CREATE TABLE migration_state (k INTEGER NOT NULL PRIMARY KEY, version INTEGER NOT NULL)",
    "CREATE TABLE queue (ts INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (ts, id))",
    "CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (deadline, id))",
  };

  // `codec: "deno-v8" | "bun-jsc-v1"`. Throws rt::JsError with the TS messages (and the
  // TypeErrors JavaScript raises for column values SQLite itself never produces).
  NativeRevision verifyNativeSchema(const NativeReader& db, std::string_view codec);

  void validateNativeRow(const NativeRow& row, const NativeRevision& revision);
}
