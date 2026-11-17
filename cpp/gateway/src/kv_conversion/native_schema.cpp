#include "uos/kv_conversion/native_schema.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace uos::kv_conversion::native_schema {
  using bun_sqlite::Row;
  using bun_sqlite::Value;

  namespace {
    class AdaptedStatement final : public bun_sqlite::Statement {
    public:
      explicit AdaptedStatement(rt::sqlite::Statement statement_) : statement(std::move(statement_)) {}
      std::int64_t paramsCount() const override { return statement.paramsCount(); }
      bool hasSafeIntegers() const override { return true; }
      void safeIntegers(bool enabled) override { statement.safeIntegers(enabled); }
      std::optional<Row> get() override { return statement.get(); }
      std::vector<Row> all() override { return statement.all(); }
      bun_sqlite::RowIterator iterate() override {
        auto rows = std::make_shared<rt::sqlite::RowIterator>(statement.iterate());
        return [rows] { return rows->next(); };
      }

    private:
      rt::sqlite::Statement statement;
    };

    class AdaptedDatabase final : public bun_sqlite::Database {
    public:
      explicit AdaptedDatabase(rt::sqlite::Database database_) : database(std::move(database_)) {}
      std::shared_ptr<bun_sqlite::Statement> query(const std::string& sql) override { return std::make_shared<AdaptedStatement>(database.query(sql)); }
      void close(bool throwOnError) override { database.close(throwOnError); }

    private:
      rt::sqlite::Database database;
    };
  }

  std::shared_ptr<bun_sqlite::Database> bun_sqlite::adapt(rt::sqlite::Database database) { return std::make_shared<AdaptedDatabase>(std::move(database)); }

  NativeSnapshotReader createNativeSnapshotReader(std::shared_ptr<bun_sqlite::Database> database) {
    NativeSnapshotReader reader;
    reader.query = [database](const std::string& sql) {
      const std::shared_ptr<bun_sqlite::Statement> statement = database->query(sql);
      if (statement->paramsCount() != 0) throw rt::JsError("Conversion snapshot queries must not have bound parameters");
      // Pinned Bun exposes this method, although its installed type declaration omits it.
      if (!statement->hasSafeIntegers()) throw rt::JsError("Bun statement safe integers are unavailable");
      statement->safeIntegers(true);
      NativeReadStatement read;
      read.get = [statement] { return statement->get(); };
      read.all = [statement] { return statement->all(); };
      read.iterate = [statement] { return statement->iterate(); };
      return read;
    };
    reader.close = [database] { database->close(true); };
    return reader;
  }

  namespace {
    struct ExpectedColumns {
      std::string_view table;
      std::vector<std::string> columns;
    };
    const std::vector<ExpectedColumns> expectedColumns{
      {"data_version", {"k:INTEGER:1", "version:INTEGER:0", "seq:INTEGER:0"}},
      {"kv", {"k:BLOB:1", "v:BLOB:0", "v_encoding:INTEGER:0", "version:INTEGER:0", "seq:INTEGER:0", "expiration_ms:INTEGER:0"}},
      {"migration_state", {"k:INTEGER:1", "version:INTEGER:0"}},
      {"queue", {"ts:INTEGER:1", "id:TEXT:2", "data:BLOB:0", "backoff_schedule:TEXT:0", "keys_if_undelivered:BLOB:0"}},
      {"queue_running", {"deadline:INTEGER:1", "id:TEXT:2", "data:BLOB:0", "backoff_schedule:TEXT:0", "keys_if_undelivered:BLOB:0"}},
      {"uos_kv_metadata", {"key:TEXT:1", "value:TEXT:0"}},
    };

    const std::string* textOf(const Value* value) { return value ? std::get_if<std::string>(value) : nullptr; }

    // `value === text` for a column value (nullptr is undefined).
    bool isText(const Value* value, std::string_view text) {
      const std::string* string = textOf(value);
      return string != nullptr && *string == text;
    }

    // `value === n` for a bigint literal `n`.
    bool isBigInt(const Value* value, std::int64_t expected) {
      const std::int64_t* integer = value ? std::get_if<std::int64_t>(value) : nullptr;
      return integer != nullptr && *integer == expected;
    }

    // `db.query(sql).get()?.[column]`
    const Value* field(const std::optional<Row>& row, std::string_view column) { return row ? row->get(column) : nullptr; }

    // String(value)
    std::string jsString(const Value* value) {
      if (value == nullptr) return "undefined";
      if (std::holds_alternative<std::nullptr_t>(*value)) return "null";
      if (const std::int64_t* integer = std::get_if<std::int64_t>(value)) return std::to_string(*integer);
      if (const double* number = std::get_if<double>(value)) return rt::numberToString(*number);
      if (const std::string* text = std::get_if<std::string>(value)) return *text;
      // A Uint8Array converts through Array.prototype.join: "1,2,3".
      const std::vector<std::uint8_t>& bytes = std::get<std::vector<std::uint8_t>>(*value);
      std::string joined;
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index > 0) joined += ",";
        joined += std::to_string(bytes[index]);
      }
      return joined;
    }

    // The TypeError JavaScriptCore throws for `<receiver>.<method>(<arguments>)` when the
    // receiver is not a string (the expression text is part of its message).
    [[noreturn]] void throwNotAString(const Value* receiver, std::string_view expression, std::string_view method, std::string_view arguments) {
      const std::string callee = std::string(expression) + "." + std::string(method);
      if (receiver == nullptr) throw rt::TypeError("undefined is not an object (evaluating '" + callee + "')");
      if (std::holds_alternative<std::nullptr_t>(*receiver)) throw rt::TypeError("null is not an object (evaluating '" + callee + "')");
      throw rt::TypeError(callee + " is not a function. (In '" + callee + "(" + std::string(arguments) + ")', '" + callee + "' is undefined)");
    }

    std::string normalizeSql(std::string_view sql) {
      return rt::replaceAll(rt::replaceAll(rt::replaceAll(rt::replaceAll(rt::toLowerCase(sql), " ", ""), "\n", ""), "\r", ""), "\t", "");
    }

    const std::unordered_set<std::string>& supportedSql() {
      static const std::unordered_set<std::string> supported = [] {
        std::unordered_set<std::string> set;
        for (const std::string_view ddl : NATIVE_DDL) set.insert(normalizeSql(ddl));
        set.insert(normalizeSql("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID"));
        return set;
      }();
      return supported;
    }

    int compareNames(const std::string& left, const std::string& right) { return rt::localeCompare(left, right); }

    // JSON.stringify(tables.toSorted(compareNames)) as the list of names it describes, or
    // std::nullopt when a name is not a string (such a list never equals the required
    // names). Sorting moves undefined to the end without calling the comparator. JSC sorts
    // up to 64 elements by binary insertion (after a run scan from 8 elements), which
    // passes every later element as `left` in index order: the first non-string after the
    // first element therefore throws from `left.localeCompare`. A bigint never passed as
    // `left` throws from JSON.stringify instead.
    std::optional<std::vector<std::string>> sortedTableNames(const std::vector<const Value*>& tables) {
      std::vector<const Value*> compacted;
      for (const Value* name : tables)
        if (name != nullptr) compacted.push_back(name);
      for (std::size_t index = 1; index < compacted.size(); ++index)
        if (textOf(compacted[index]) == nullptr) throwNotAString(compacted[index], "left", "localeCompare", "right");
      std::vector<std::string> names;
      bool onlyText = compacted.size() == tables.size();
      for (const Value* name : compacted) {
        if (const std::string* text = textOf(name)) names.push_back(*text);
        else if (std::holds_alternative<std::int64_t>(*name)) throw rt::TypeError("JSON.stringify cannot serialize BigInt.");
        else onlyText = false;
      }
      if (!onlyText) return std::nullopt;
      return rt::toSorted(std::move(names), compareNames);
    }

    /** The supported schema is deliberately narrow: no guessing at newer schemas or queued payloads. */
    void verifySchemaLayout(const NativeReader& db, std::string_view codec) {
      const std::vector<Row> schema = db.query("SELECT type,name,sql FROM sqlite_master ORDER BY name").all();
      std::vector<const Value*> tables;
      for (const Row& item : schema)
        if (isText(item.get(std::string_view("type")), "table")) tables.push_back(item.get(std::string_view("name")));
      std::vector<std::string> required;
      for (const ExpectedColumns& expected : expectedColumns)
        if (codec == "bun-jsc-v1" || expected.table != "uos_kv_metadata") required.emplace_back(expected.table);
      const std::optional<std::vector<std::string>> sortedTables = sortedTableNames(tables);
      if (!sortedTables || *sortedTables != rt::toSorted(required, compareNames)) throw rt::JsError("Unsupported native KV tables or codec marker");
      static const std::vector<std::string_view> allowedIndexes{"kv_expiration_ms_idx", "sqlite_autoindex_queue_1", "sqlite_autoindex_queue_running_1"};
      for (const Row& item : schema) {
        const Value* type = item.get(std::string_view("type"));
        const Value* name = item.get(std::string_view("name"));
        const bool allowedIndex =
          isText(type, "index") && textOf(name) != nullptr && std::find(allowedIndexes.begin(), allowedIndexes.end(), *textOf(name)) != allowedIndexes.end();
        if (!isText(type, "table") && !allowedIndex) throw rt::JsError("Unsupported native schema object");
        const Value* sql = item.get(std::string_view("sql"));
        if (sql != nullptr && std::holds_alternative<std::nullptr_t>(*sql)) continue;
        if (textOf(sql) == nullptr) throwNotAString(sql, "sql", "toLowerCase", "");
        if (!supportedSql().contains(normalizeSql(*textOf(sql)))) throw rt::JsError("Unsupported native schema definition");
      }
      for (const std::string& table : required) {
        const std::vector<Row> columns = db.query("PRAGMA table_info(" + table + ")").all();
        std::vector<std::string> described;
        for (const Row& column : columns) {
          const std::string name = jsString(column.get(std::string_view("name")));
          const Value* type = column.get(std::string_view("type"));
          if (textOf(type) == nullptr) throwNotAString(type, "column.type", "toUpperCase", "");
          described.push_back(name + ":" + rt::toUpperCase(*textOf(type)) + ":" + jsString(column.get(std::string_view("pk"))));
        }
        const auto expected = std::find_if(expectedColumns.begin(), expectedColumns.end(), [&table](const ExpectedColumns& entry) { return entry.table == table; });
        if (described != expected->columns) throw rt::JsError("Unsupported schema for " + table);
      }
    }

    bool isNativeInteger(const Value* value) { return value != nullptr && std::holds_alternative<std::int64_t>(*value); }

    std::int64_t nativeInteger(const Value* value) { return std::get<std::int64_t>(*value); }
  }

  NativeRevision verifyNativeSchema(const NativeReader& db, std::string_view codec) {
    verifySchemaLayout(db, codec);
    if (!isBigInt(field(db.query("PRAGMA user_version").get(), "user_version"), 0) ||
        !isBigInt(field(db.query("PRAGMA application_id").get(), "application_id"), 0))
      throw rt::JsError("Unsupported native SQLite header metadata");
    const std::vector<Row> migration = db.query("SELECT k,version FROM migration_state").all();
    if (migration.size() != 1 || !isBigInt(migration[0].get(std::string_view("k")), 0) || !isBigInt(migration[0].get(std::string_view("version")), 3))
      throw rt::JsError("Unsupported native KV migration version");
    for (const std::string_view queue : {std::string_view("queue"), std::string_view("queue_running")}) {
      if (!isBigInt(field(db.query("SELECT COUNT(*) AS count FROM " + std::string(queue)).get(), "count"), 0))
        throw rt::JsError("Nonempty native queues require a separate supported migration");
    }
    if (codec == "bun-jsc-v1") {
      if (!isText(field(db.query("SELECT value FROM uos_kv_metadata WHERE key='codec'").get(), "value"), codec)) throw rt::JsError("Incompatible source codec");
    }
    const std::vector<Row> revision = db.query("SELECT k,version,seq FROM data_version").all();
    for (const Row& row : revision) {
      for (const std::string_view column : {std::string_view("k"), std::string_view("version"), std::string_view("seq")})
        if (!isNativeInteger(row.get(column))) throw rt::JsError("Unsupported native revision metadata types");
    }
    if (revision.size() != 1 || !isBigInt(revision[0].get(std::string_view("k")), 0) || nativeInteger(revision[0].get(std::string_view("version"))) < 0 ||
        nativeInteger(revision[0].get(std::string_view("seq"))) < 0)
      throw rt::JsError("Unsupported native revision metadata");
    if (!isText(field(db.query("PRAGMA integrity_check").get(), "integrity_check"), "ok")) throw rt::JsError("Native SQLite integrity check failed");
    return NativeRevision{
      .k = nativeInteger(revision[0].get(std::string_view("k"))),
      .version = nativeInteger(revision[0].get(std::string_view("version"))),
      .seq = nativeInteger(revision[0].get(std::string_view("seq"))),
    };
  }

  void validateNativeRow(const NativeRow& row, const NativeRevision& revision) {
    for (const std::string_view column : {std::string_view("v_encoding"), std::string_view("version"), std::string_view("seq"), std::string_view("expiration_ms")})
      if (!isNativeInteger(row.get(column))) throw rt::JsError("Unsupported native row metadata types");
    const Value* k = row.get(std::string_view("k"));
    const Value* v = row.get(std::string_view("v"));
    const auto* key = k ? std::get_if<std::vector<std::uint8_t>>(k) : nullptr;
    const auto* bytes = v ? std::get_if<std::vector<std::uint8_t>>(v) : nullptr;
    if (key == nullptr || key->empty() || bytes == nullptr) throw rt::JsError("Invalid native binary row");
    const double encoding = static_cast<double>(nativeInteger(row.get(std::string_view("v_encoding"))));
    if (!(encoding == 1 || encoding == 2 || encoding == 3)) throw rt::JsError("Unsupported native value encoding");
    if (encoding == 2 && bytes->size() != 8) throw rt::JsError("Malformed native unsigned counter");
    const std::int64_t version = nativeInteger(row.get(std::string_view("version")));
    const std::int64_t seq = nativeInteger(row.get(std::string_view("seq")));
    const std::int64_t expiration = nativeInteger(row.get(std::string_view("expiration_ms")));
    if (version < 0 || version > revision.version || seq < 0 || expiration < -1) throw rt::JsError("Invalid native revision or absolute expiration");
  }
}
