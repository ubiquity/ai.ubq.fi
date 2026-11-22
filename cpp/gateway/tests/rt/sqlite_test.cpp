// Differential tests for uos/rt/sqlite.hpp (bun:sqlite). Every expected line was produced by
// running the same scenario under Bun 1.4.0 (Linux arm64, bundled SQLite 3.53.2) with a
// recorder that prints `name<TAB>{"ok": value}` or `{"threw": error}`, values encoded as:
// null, numbers (JSON), strings, {"$bigint": "n"}, {"$number": "-0" | "NaN" | "Infinity"},
// {"$bytes": hex, "ctor": "Uint8Array"}, {"$undefined": true}, rows as {"$row": [[key, value],
// ...], "proto": "Object"} in Object.keys order, errors as {"$error": {ctor, name, message,
// code, errno, byteOffset, keys, string, isSQLiteError}}. The C++ scenario encodes its result
// the same way and must print the same line. Lines that differ because the SDK's SQLite is
// 3.40.1 are marked VERSION with Bun's line in the comment. Section comments name the
// recording scripts (scratch, not kept); each C++ body is a statement-for-statement port of
// its scenario, so a scenario is re-recorded by writing it back in TypeScript.
#include "uos/rt/sqlite.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <unistd.h>
#include <exception>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace sqlite = uos::rt::sqlite;
  using sqlite::Binding;
  using sqlite::Bytes;
  using sqlite::Changes;
  using sqlite::Database;
  using sqlite::DatabaseOptions;
  using sqlite::Named;
  using sqlite::Row;
  using sqlite::SQLiteError;
  using sqlite::Statement;
  using sqlite::Value;
  namespace constants = sqlite::constants;

  // ---- the recorder's encoding

  json::value undefinedValue() {
    json::object tagged;
    tagged["$undefined"] = true;
    return tagged;
  }

  json::value number(double value) {
    if (value == 0 && std::signbit(value)) {
      json::object tagged;
      tagged["$number"] = "-0";
      return tagged;
    }
    if (!std::isfinite(value)) {
      json::object tagged;
      tagged["$number"] = rt::numberToString(value);
      return tagged;
    }
    return rt::makeNumber(value);
  }

  json::value bigint(std::int64_t value) {
    json::object tagged;
    tagged["$bigint"] = std::to_string(value);
    return tagged;
  }

  json::value enc(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) return nullptr;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return bigint(*integer);
    if (const auto* real = std::get_if<double>(&value)) return number(*real);
    if (const auto* text = std::get_if<std::string>(&value)) return json::value(*text);
    json::object tagged;
    tagged["$bytes"] = rt::toHex(std::get<Bytes>(value));
    tagged["ctor"] = "Uint8Array";
    return tagged;
  }

  json::value enc(const Row& row) {
    json::array entries;
    for (const auto& [key, value] : row) entries.push_back(json::array{json::value(key), enc(value)});
    json::object tagged;
    tagged["$row"] = std::move(entries);
    tagged["proto"] = "Object";
    return tagged;
  }

  json::value enc(const std::optional<Row>& row) { return row ? enc(*row) : json::value(nullptr); }

  json::value enc(const std::vector<Row>& rows) {
    json::array out;
    for (const Row& row : rows) out.push_back(enc(row));
    return out;
  }

  json::value enc(const Changes& changes) {
    json::array entries;
    entries.push_back(json::array{json::value("changes"), number(changes.changes)});
    if (const auto* rowid = std::get_if<std::int64_t>(&changes.lastInsertRowid)) entries.push_back(json::array{json::value("lastInsertRowid"), bigint(*rowid)});
    else entries.push_back(json::array{json::value("lastInsertRowid"), number(std::get<double>(changes.lastInsertRowid))});
    json::object tagged;
    tagged["$row"] = std::move(entries);
    tagged["proto"] = "Object";
    return tagged;
  }

  // `values()` of the rows: each row's values in column order (rows here never repeat a name).
  json::value values(const std::vector<Row>& rows) {
    json::array out;
    for (const Row& row : rows) {
      json::array line;
      for (const auto& [key, value] : row) line.push_back(enc(value));
      out.push_back(std::move(line));
    }
    return out;
  }

  json::value encError(const rt::JsError& error) {
    json::object fields;
    const auto* sqliteError = dynamic_cast<const SQLiteError*>(&error);
    fields["ctor"] = sqliteError ? "Error" : error.name;
    fields["name"] = error.name;
    fields["message"] = error.message;
    json::array keys;
    if (sqliteError) {
      if (sqliteError->code) {
        fields["code"] = *sqliteError->code;
        keys.push_back("code");
      }
      fields["errno"] = sqliteError->errno_;
      fields["byteOffset"] = sqliteError->byteOffset;
      keys.push_back("errno");
      keys.push_back("byteOffset");
    }
    fields["keys"] = std::move(keys);
    fields["string"] = error.toString();
    fields["isSQLiteError"] = sqliteError != nullptr;
    json::object tagged;
    tagged["$error"] = std::move(fields);
    return tagged;
  }

  // The error a scenario caught and returned as its value.
  template <class Body> json::value caught(Body&& body) {
    try {
      return body();
    } catch (const rt::JsError& error) {
      return encError(error);
    }
  }

  std::string recorded(std::string_view name, const std::function<json::value()>& body) {
    json::object result;
    try {
      result["ok"] = body();
    } catch (const rt::JsError& error) {
      result["threw"] = encError(error);
    }
    return std::string(name) + "\t" + rt::jsonStringify(json::value(std::move(result)));
  }

// Fresh Bun1.4.0 and host-shared SQLite3.54 observations are retained in
// /tmp/claude-1002/prod/runtime/sqlite-{oracle,shared-oracle}-v8.*.
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#define SQLITE_VERSIONED_EXPECTATION(sdk, native) native
#else
#define SQLITE_VERSIONED_EXPECTATION(sdk, native) sdk
#endif

#if defined(__APPLE__)
#define SQLITE_PLATFORM_EXPECTATION(linux, darwin) darwin
#else
#define SQLITE_PLATFORM_EXPECTATION(linux, darwin) linux
#endif

#define LINE(expected, name, ...) EXPECT_EQ(recorded(name, [&]() -> json::value { __VA_ARGS__ }), std::string(expected));

  json::value text(std::string_view value) { return json::value(std::string(value)); }
  json::value flag(bool value) { return value; }
  json::value integer(long long value) { return value; }
  json::value list(std::initializer_list<json::value> items) {
    json::array out;
    for (const json::value& item : items) out.push_back(item);
    return out;
  }

  // A throwaway directory under the system temporary directory, removed afterwards. Never a
  // kv*.sqlite3 name inside.
  struct TempDir {
    std::string root = rt::fs::realpathSync(rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-rt-sqlite-"));
    TempDir() = default;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() { rt::fs::rmSync(root, {.recursive = true, .force = true}); }
    std::string path(std::string_view name) const { return root + "/" + std::string(name); }
  };

  // `try { ... } finally { db.close(); }`
  struct Closing {
    Database database;
    explicit Closing(Database database_) : database(std::move(database_)) {}
    Closing(const Closing&) = delete;
    Closing& operator=(const Closing&) = delete;
    ~Closing() { database.close(); }
    const Database* operator->() const { return &database; }
  };

  // process.chdir(path) for one scope.
  struct WorkingDirectory {
    std::string previous = rt::process::cwd();
    explicit WorkingDirectory(const std::string& path) {
      if (::chdir(path.c_str()) != 0) throw std::runtime_error("chdir " + path);
    }
    WorkingDirectory(const WorkingDirectory&) = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
    ~WorkingDirectory() { if (::chdir(previous.c_str()) != 0) std::terminate(); }
  };

  // An oracle line recorded under /tmp/.../o1 with the directory replaced by `root`.
  std::string at(std::string expected, const std::string& from, const std::string& root) { return rt::replaceAll(expected, from, root); }
}

// ---- open (scratch o1_open.ts) ---------------------------------------------------------------

TEST("open: option flags, numeric flags, filename trimming and anonymous databases") {
  const TempDir dir;
  const std::string o1 = "/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1";
  const auto opened = [&](Database db, const std::string& file) {
    const json::value result = list({text(db.filename()), flag(rt::fs::existsSync(rt::trim(file)))});
    db.close();
    return result;
  };
  LINE(at(R"(default-creates	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/a.db",true]})", o1, dir.root), "default-creates", return opened(Database(dir.path("a.db")), dir.path("a.db"));)
  const std::string cantopen =
    R"({"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})";
  const std::string misuse =
    R"({"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"bad parameter or other API misuse","code":"SQLITE_MISUSE","errno":21,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: bad parameter or other API misuse","isSQLiteError":true}}})";
  LINE("readonly-missing\t" + cantopen, "readonly-missing", return opened(Database(dir.path("missing1.db"), {.readonly = true}), "");)
  LINE("readonly-create-false-missing\t" + cantopen, "readonly-create-false-missing", return opened(Database(dir.path("missing2.db"), {.readonly = true, .create = false}), "");)
  LINE(at(R"(create-missing	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/c.db",true]})", o1, dir.root), "create-missing", return opened(Database(dir.path("c.db"), {.create = true}), dir.path("c.db"));)
  LINE("readwrite-missing\t" + cantopen, "readwrite-missing", return opened(Database(dir.path("missing3.db"), {.readwrite = true}), "");)
  LINE(at(R"(strict-only-missing	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/s.db",true]})", o1, dir.root), "strict-only-missing", return opened(Database(dir.path("s.db"), {.strict = true}), dir.path("s.db"));)
  LINE(at(R"(safeIntegers-only-missing	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/si.db",true]})", o1, dir.root), "safeIntegers-only-missing",
       return opened(Database(dir.path("si.db"), {.safeIntegers = true}), dir.path("si.db"));)
  LINE("readonly-strict-missing\t" + cantopen, "readonly-strict-missing", return opened(Database(dir.path("missing4.db"), {.readonly = true, .strict = true}), "");)
  LINE("readonly-safe-missing\t" + cantopen, "readonly-safe-missing", return opened(Database(dir.path("missing5.db"), {.readonly = true, .safeIntegers = true}), "");)
  LINE("empty-options-missing\t" + misuse, "empty-options-missing", return opened(Database(dir.path("missing6.db"), DatabaseOptions{}), "");)
  LINE("readonly-readwrite-missing\t" + misuse, "readonly-readwrite-missing", return opened(Database(dir.path("missing7.db"), {.readonly = true, .readwrite = true}), "");)
  LINE(at(R"(readonly-create-missing	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/rc.db",true]})", o1, dir.root), "readonly-create-missing",
       return opened(Database(dir.path("rc.db"), {.readonly = true, .create = true}), dir.path("rc.db"));)
  LINE("numeric-0\t" + misuse, "numeric-0", return opened(Database(dir.path("missing9.db"), 0), "");)
  LINE("numeric-ro\t" + cantopen, "numeric-ro", return opened(Database(dir.path("missing10.db"), constants::SQLITE_OPEN_READONLY), "");)
  LINE("numeric-rw\t" + cantopen, "numeric-rw", return opened(Database(dir.path("missing11.db"), constants::SQLITE_OPEN_READWRITE), "");)
  LINE(at(R"(numeric-rwc	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/n.db",true]})", o1, dir.root), "numeric-rwc",
       return opened(Database(dir.path("n.db"), constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE), dir.path("n.db"));)
  LINE(at(R"(trimmed	{"ok":["/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/t.db",true]})", o1, dir.root), "trimmed", return opened(Database("  " + dir.path("t.db") + "\n"), dir.path("t.db"));)
  const std::string anonymous =
    R"({"threw":{"$error":{"ctor":"Error","name":"Error","message":"Cannot open an anonymous database in read-only mode.","keys":[],"string":"Error: Cannot open an anonymous database in read-only mode.","isSQLiteError":false}}})";
  LINE("anon-readonly-empty\t" + anonymous, "anon-readonly-empty", return opened(Database("", {.readonly = true}), "");)
  LINE("anon-readonly-memory\t" + anonymous, "anon-readonly-memory", return opened(Database(":memory:", {.readonly = true}), "");)
  LINE("anon-readonly-memory-trim\t" + anonymous, "anon-readonly-memory-trim", return opened(Database("  :memory: ", {.readonly = true}), "");)
  LINE("anon-readonly-numeric\t" + anonymous, "anon-readonly-numeric", return opened(Database(":memory:", constants::SQLITE_OPEN_READONLY), "");)
  LINE(R"(anon-empty	{"ok":["",false]})", "anon-empty", return opened(Database(""), "");)
  LINE(R"(no-args	{"ok":[":memory:",{"$row":[["one",1]],"proto":"Object"}]})", "no-args", {
    Database db;
    const json::value result = list({text(db.filename()), enc(db.query("SELECT 1 AS one").get())});
    db.close();
    return result;
  })
}

TEST("open: a file: name is a plain path unless SQLITE_OPEN_URI is given") {
  const TempDir dir;
  const std::string o1 = "/tmp/claude-1002/wave/rt-sqlite/scratch/db/o1";
  {
    Database db(dir.path("ro.db"));
    db.run("CREATE TABLE t(a)");
    db.close();
  }
  {
    // The recorder chdir()s into its directory: a relative "file:" name is created there.
    const WorkingDirectory inside(dir.root);
    LINE(SQLITE_PLATFORM_EXPECTATION(R"(uri-plain-relative	{"ok":[{"$row":[["one",1]],"proto":"Object"},["file:urirel.db?mode=ro"]]})",
      R"(uri-plain-relative	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})"), "uri-plain-relative", {
      const Closing db(Database("file:urirel.db?mode=ro"));
      const json::value row = enc(db->query("SELECT 1 AS one").get());
      json::array files;
      for (const std::string& name : rt::fs::readdirSync(dir.root))
        if (rt::startsWith(name, "file")) files.push_back(json::value(name));
      return list({row, files});
    })
  }
  LINE(
    R"(uri-plain-absolute	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})",
    "uri-plain-absolute", {
      const Closing db(Database("file://" + dir.path("abs.db") + "?mode=ro"));
      return nullptr;
    })
  LINE(
    SQLITE_PLATFORM_EXPECTATION(R"(uri-readonly-strict-immutable	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})",
      R"(uri-readonly-strict-immutable	{"ok":1})"),
    "uri-readonly-strict-immutable", {
      const Closing db(Database("file://" + dir.path("ro.db") + "?immutable=1", {.readonly = true, .strict = true}));
      return 1;
    })
  LINE(at(R"(uri-flag-immutable	{"ok":["file:///tmp/claude-1002/wave/rt-sqlite/scratch/db/o1/ro.db?immutable=1",{"$row":[["n",0]],"proto":"Object"}]})", o1, dir.root), "uri-flag-immutable", {
    const Closing db(Database("file://" + dir.path("ro.db") + "?immutable=1", constants::SQLITE_OPEN_READONLY | constants::SQLITE_OPEN_URI));
    return list({text(db->filename()), enc(db->query("SELECT count(*) AS n FROM t").get())});
  })
  LINE(
    R"(uri-flag-immutable-write	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"attempt to write a readonly database","code":"SQLITE_READONLY","errno":8,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: attempt to write a readonly database","isSQLiteError":true}}})",
    "uri-flag-immutable-write", {
      const Closing db(Database("file://" + dir.path("ro.db") + "?immutable=1", constants::SQLITE_OPEN_READONLY | constants::SQLITE_OPEN_URI));
      return enc(db->run("INSERT INTO t VALUES (2)"));
    })
  LINE(
    R"(uri-flag-missing	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})",
    "uri-flag-missing", {
      const Closing db(Database("file://" + dir.path("nouri.db") + "?mode=ro", constants::SQLITE_OPEN_READONLY | constants::SQLITE_OPEN_URI));
      return 1;
    })
  LINE(R"(uri-flag-memory	{"ok":{"$row":[["one",1]],"proto":"Object"}})", "uri-flag-memory", {
    const Closing db(Database("file:memdb1?mode=memory&cache=shared", constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE | constants::SQLITE_OPEN_URI));
    return enc(db->query("SELECT 1 AS one").get());
  })
}

// ---- open: failures (scratch o1_open.ts) -------------------------------------------------------

TEST("open: read-only writes, directories and non-database files fail like Bun") {
  const TempDir dir;
  rt::fs::mkdirSync(dir.path("dir"));
  LINE(R"~(directory	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})~", "directory", return list({text(Database(dir.path("dir")).filename())});)
  LINE(R"~(directory-readonly	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"disk I/O error","code":"SQLITE_IOERR_READ","errno":266,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: disk I/O error","isSQLiteError":true}}})~", "directory-readonly", return list({text(Database(dir.path("dir"), {.readonly = true}).filename())});)
  rt::fs::writeFileSync(dir.path("garbage.db"), "this is not a database at all, definitely not, more than 100 bytes of text to fill a header page and then some more text here");
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(garbage-open-then-query	{"ok":{"$row":[["one",1]],"proto":"Object"}})~", R"~(garbage-open-then-query	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"file is not a database","code":"SQLITE_NOTADB","errno":26,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: file is not a database","isSQLiteError":true}}})~"), "garbage-open-then-query", {
    const Closing db(Database(dir.path("garbage.db")));
    return enc(db->query("SELECT 1 AS one").get());
  })
  LINE(R"~(garbage-open-then-select-table	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"file is not a database","code":"SQLITE_NOTADB","errno":26,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: file is not a database","isSQLiteError":true}}})~", "garbage-open-then-select-table", {
    const Closing db(Database(dir.path("garbage.db")));
    return enc(db->query("SELECT * FROM sqlite_master").all());
  })
  LINE(R"~(garbage-run	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"file is not a database","code":"SQLITE_NOTADB","errno":26,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: file is not a database","isSQLiteError":true}}})~", "garbage-run", {
    const Closing db(Database(dir.path("garbage.db")));
    return enc(db->run("CREATE TABLE x(a)"));
  })
  {
    Database db(dir.path("ro.db"));
    db.run("CREATE TABLE t(a)");
    db.close();
  }
  LINE(R"~(readonly-write	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"attempt to write a readonly database","code":"SQLITE_READONLY","errno":8,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: attempt to write a readonly database","isSQLiteError":true}}})~", "readonly-write", {
    const Closing db(Database(dir.path("ro.db"), {.readonly = true}));
    return enc(db->run("INSERT INTO t VALUES (1)"));
  })
  LINE(R"~(readonly-write-query	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"attempt to write a readonly database","code":"SQLITE_READONLY","errno":8,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: attempt to write a readonly database","isSQLiteError":true}}})~", "readonly-write-query", {
    const Closing db(Database(dir.path("ro.db"), {.readonly = true}));
    return enc(db->query("INSERT INTO t VALUES (1)").run());
  })
  LINE(R"~(readonly-read	{"ok":{"$row":[["n",0]],"proto":"Object"}})~", "readonly-read", {
    const Closing db(Database(dir.path("ro.db"), {.readonly = true}));
    return enc(db->query("SELECT count(*) AS n FROM t").get());
  })
}

// ---- values (scratch o2_values.ts, o11.ts) -----------------------------------------------------

namespace {
  struct ValueCase {
    const char* name;
    const char* sql;
    const char* get;
    const char* safe;
  };
  const ValueCase kValueCases[] = {
    {"int-small", R"~(SELECT 1 AS v)~",
     R"~(int-small:get	{"ok":{"$row":[["v",1]],"proto":"Object"}})~",
     R"~(int-small:safe	{"ok":{"$row":[["v",{"$bigint":"1"}]],"proto":"Object"}})~"},
    {"int-neg", R"~(SELECT -5 AS v)~",
     R"~(int-neg:get	{"ok":{"$row":[["v",-5]],"proto":"Object"}})~",
     R"~(int-neg:safe	{"ok":{"$row":[["v",{"$bigint":"-5"}]],"proto":"Object"}})~"},
    {"int-2^31", R"~(SELECT 2147483648 AS v)~",
     R"~(int-2^31:get	{"ok":{"$row":[["v",2147483648]],"proto":"Object"}})~",
     R"~(int-2^31:safe	{"ok":{"$row":[["v",{"$bigint":"2147483648"}]],"proto":"Object"}})~"},
    {"int-2^53-1", R"~(SELECT 9007199254740991 AS v)~",
     R"~(int-2^53-1:get	{"ok":{"$row":[["v",9007199254740991]],"proto":"Object"}})~",
     R"~(int-2^53-1:safe	{"ok":{"$row":[["v",{"$bigint":"9007199254740991"}]],"proto":"Object"}})~"},
    {"int-2^53+1", R"~(SELECT 9007199254740993 AS v)~",
     R"~(int-2^53+1:get	{"ok":{"$row":[["v",9007199254740992]],"proto":"Object"}})~",
     R"~(int-2^53+1:safe	{"ok":{"$row":[["v",{"$bigint":"9007199254740993"}]],"proto":"Object"}})~"},
    {"int-max", R"~(SELECT 9223372036854775807 AS v)~",
     R"~(int-max:get	{"ok":{"$row":[["v",9223372036854776000]],"proto":"Object"}})~",
     R"~(int-max:safe	{"ok":{"$row":[["v",{"$bigint":"9223372036854775807"}]],"proto":"Object"}})~"},
    {"int-min", R"~(SELECT -9223372036854775808 AS v)~",
     R"~(int-min:get	{"ok":{"$row":[["v",-9223372036854776000]],"proto":"Object"}})~",
     R"~(int-min:safe	{"ok":{"$row":[["v",{"$bigint":"-9223372036854775808"}]],"proto":"Object"}})~"},
    {"int-zero", R"~(SELECT 0 AS v)~",
     R"~(int-zero:get	{"ok":{"$row":[["v",0]],"proto":"Object"}})~",
     R"~(int-zero:safe	{"ok":{"$row":[["v",{"$bigint":"0"}]],"proto":"Object"}})~"},
    {"real", R"~(SELECT 1.5 AS v)~",
     R"~(real:get	{"ok":{"$row":[["v",1.5]],"proto":"Object"}})~",
     R"~(real:safe	{"ok":{"$row":[["v",1.5]],"proto":"Object"}})~"},
    {"real-integral", R"~(SELECT 2.0 AS v)~",
     R"~(real-integral:get	{"ok":{"$row":[["v",2]],"proto":"Object"}})~",
     R"~(real-integral:safe	{"ok":{"$row":[["v",2]],"proto":"Object"}})~"},
    {"real-negzero", R"~(SELECT -0.0 AS v)~",
     R"~(real-negzero:get	{"ok":{"$row":[["v",{"$number":"-0"}]],"proto":"Object"}})~",
     R"~(real-negzero:safe	{"ok":{"$row":[["v",{"$number":"-0"}]],"proto":"Object"}})~"},
    {"real-negzero-expr", R"~(SELECT -(0.0) AS v)~",
     R"~(real-negzero-expr:get	{"ok":{"$row":[["v",{"$number":"-0"}]],"proto":"Object"}})~",
     R"~(real-negzero-expr:safe	{"ok":{"$row":[["v",{"$number":"-0"}]],"proto":"Object"}})~"},
    {"real-inf", R"~(SELECT 1e999 AS v)~",
     R"~(real-inf:get	{"ok":{"$row":[["v",{"$number":"Infinity"}]],"proto":"Object"}})~",
     R"~(real-inf:safe	{"ok":{"$row":[["v",{"$number":"Infinity"}]],"proto":"Object"}})~"},
    {"real-neginf", R"~(SELECT -1e999 AS v)~",
     R"~(real-neginf:get	{"ok":{"$row":[["v",{"$number":"-Infinity"}]],"proto":"Object"}})~",
     R"~(real-neginf:safe	{"ok":{"$row":[["v",{"$number":"-Infinity"}]],"proto":"Object"}})~"},
    {"real-tiny", R"~(SELECT 5e-324 AS v)~",
     R"~(real-tiny:get	{"ok":{"$row":[["v",5e-324]],"proto":"Object"}})~",
     R"~(real-tiny:safe	{"ok":{"$row":[["v",5e-324]],"proto":"Object"}})~"},
    {"real-big", R"~(SELECT 1e300 * 1e5 AS v)~",
     R"~(real-big:get	{"ok":{"$row":[["v",1.0000000000000001e+305]],"proto":"Object"}})~",
     R"~(real-big:safe	{"ok":{"$row":[["v",1.0000000000000001e+305]],"proto":"Object"}})~"},
    {"real-0.1+0.2", R"~(SELECT 0.1 + 0.2 AS v)~",
     R"~(real-0.1+0.2:get	{"ok":{"$row":[["v",0.30000000000000004]],"proto":"Object"}})~",
     R"~(real-0.1+0.2:safe	{"ok":{"$row":[["v",0.30000000000000004]],"proto":"Object"}})~"},
    {"text-ascii", R"~(SELECT 'hello' AS v)~",
     R"~(text-ascii:get	{"ok":{"$row":[["v","hello"]],"proto":"Object"}})~",
     R"~(text-ascii:safe	{"ok":{"$row":[["v","hello"]],"proto":"Object"}})~"},
    {"text-utf8", R"~(SELECT 'héllo wörld €' AS v)~",
     R"~(text-utf8:get	{"ok":{"$row":[["v","héllo wörld €"]],"proto":"Object"}})~",
     R"~(text-utf8:safe	{"ok":{"$row":[["v","héllo wörld €"]],"proto":"Object"}})~"},
    {"text-emoji", R"~(SELECT '😀x' AS v)~",
     R"~(text-emoji:get	{"ok":{"$row":[["v","😀x"]],"proto":"Object"}})~",
     R"~(text-emoji:safe	{"ok":{"$row":[["v","😀x"]],"proto":"Object"}})~"},
    {"text-empty", R"~(SELECT '' AS v)~",
     R"~(text-empty:get	{"ok":{"$row":[["v",""]],"proto":"Object"}})~",
     R"~(text-empty:safe	{"ok":{"$row":[["v",""]],"proto":"Object"}})~"},
    {"text-nul", R"~(SELECT 'a' || char(0) || 'b' AS v)~",
     R"~(text-nul:get	{"ok":{"$row":[["v","a\u0000b"]],"proto":"Object"}})~",
     R"~(text-nul:safe	{"ok":{"$row":[["v","a\u0000b"]],"proto":"Object"}})~"},
    {"text-invalid-ff", R"~(SELECT CAST(X'41FF42' AS TEXT) AS v)~",
     R"~(text-invalid-ff:get	{"ok":{"$row":[["v","A�B"]],"proto":"Object"}})~",
     R"~(text-invalid-ff:safe	{"ok":{"$row":[["v","A�B"]],"proto":"Object"}})~"},
    {"text-invalid-c3", R"~(SELECT CAST(X'C3' AS TEXT) AS v)~",
     R"~(text-invalid-c3:get	{"ok":{"$row":[["v","�"]],"proto":"Object"}})~",
     R"~(text-invalid-c3:safe	{"ok":{"$row":[["v","�"]],"proto":"Object"}})~"},
    {"text-invalid-c328", R"~(SELECT CAST(X'C328' AS TEXT) AS v)~",
     R"~(text-invalid-c328:get	{"ok":{"$row":[["v","�("]],"proto":"Object"}})~",
     R"~(text-invalid-c328:safe	{"ok":{"$row":[["v","�("]],"proto":"Object"}})~"},
    {"text-lone-high", R"~(SELECT CAST(X'EDA080' AS TEXT) AS v)~",
     R"~(text-lone-high:get	{"ok":{"$row":[["v","���"]],"proto":"Object"}})~",
     R"~(text-lone-high:safe	{"ok":{"$row":[["v","���"]],"proto":"Object"}})~"},
    {"text-lone-low", R"~(SELECT CAST(X'EDB080' AS TEXT) AS v)~",
     R"~(text-lone-low:get	{"ok":{"$row":[["v","���"]],"proto":"Object"}})~",
     R"~(text-lone-low:safe	{"ok":{"$row":[["v","���"]],"proto":"Object"}})~"},
    {"text-split-pair", R"~(SELECT CAST(X'EDA0BDEDB880' AS TEXT) AS v)~",
     R"~(text-split-pair:get	{"ok":{"$row":[["v","������"]],"proto":"Object"}})~",
     R"~(text-split-pair:safe	{"ok":{"$row":[["v","������"]],"proto":"Object"}})~"},
    {"text-overlong", R"~(SELECT CAST(X'C0AF' AS TEXT) AS v)~",
     R"~(text-overlong:get	{"ok":{"$row":[["v","��"]],"proto":"Object"}})~",
     R"~(text-overlong:safe	{"ok":{"$row":[["v","��"]],"proto":"Object"}})~"},
    {"text-bom", R"~(SELECT CAST(X'EFBBBF41' AS TEXT) AS v)~",
     R"~(text-bom:get	{"ok":{"$row":[["v","﻿A"]],"proto":"Object"}})~",
     R"~(text-bom:safe	{"ok":{"$row":[["v","﻿A"]],"proto":"Object"}})~"},
    {"text-f4908080", R"~(SELECT CAST(X'F4908080' AS TEXT) AS v)~",
     R"~(text-f4908080:get	{"ok":{"$row":[["v","����"]],"proto":"Object"}})~",
     R"~(text-f4908080:safe	{"ok":{"$row":[["v","����"]],"proto":"Object"}})~"},
    {"text-truncated-4", R"~(SELECT CAST(X'F09F98' AS TEXT) AS v)~",
     R"~(text-truncated-4:get	{"ok":{"$row":[["v","�"]],"proto":"Object"}})~",
     R"~(text-truncated-4:safe	{"ok":{"$row":[["v","�"]],"proto":"Object"}})~"},
    {"text-latin1-range", R"~(SELECT CAST(X'C3BF' AS TEXT) AS v)~",
     R"~(text-latin1-range:get	{"ok":{"$row":[["v","ÿ"]],"proto":"Object"}})~",
     R"~(text-latin1-range:safe	{"ok":{"$row":[["v","ÿ"]],"proto":"Object"}})~"},
    {"blob", R"~(SELECT X'0001FF' AS v)~",
     R"~(blob:get	{"ok":{"$row":[["v",{"$bytes":"0001ff","ctor":"Uint8Array"}]],"proto":"Object"}})~",
     R"~(blob:safe	{"ok":{"$row":[["v",{"$bytes":"0001ff","ctor":"Uint8Array"}]],"proto":"Object"}})~"},
    {"blob-empty", R"~(SELECT X'' AS v)~",
     R"~(blob-empty:get	{"ok":{"$row":[["v",{"$bytes":"","ctor":"Uint8Array"}]],"proto":"Object"}})~",
     R"~(blob-empty:safe	{"ok":{"$row":[["v",{"$bytes":"","ctor":"Uint8Array"}]],"proto":"Object"}})~"},
    {"zeroblob", R"~(SELECT zeroblob(3) AS v)~",
     R"~(zeroblob:get	{"ok":{"$row":[["v",{"$bytes":"000000","ctor":"Uint8Array"}]],"proto":"Object"}})~",
     R"~(zeroblob:safe	{"ok":{"$row":[["v",{"$bytes":"000000","ctor":"Uint8Array"}]],"proto":"Object"}})~"},
    {"zeroblob-0", R"~(SELECT zeroblob(0) AS v)~",
     R"~(zeroblob-0:get	{"ok":{"$row":[["v",{"$bytes":"","ctor":"Uint8Array"}]],"proto":"Object"}})~",
     R"~(zeroblob-0:safe	{"ok":{"$row":[["v",{"$bytes":"","ctor":"Uint8Array"}]],"proto":"Object"}})~"},
    {"null", R"~(SELECT NULL AS v)~",
     R"~(null:get	{"ok":{"$row":[["v",null]],"proto":"Object"}})~",
     R"~(null:safe	{"ok":{"$row":[["v",null]],"proto":"Object"}})~"},
    {"dup-names", R"~(SELECT 1 AS a, 2 AS a)~",
     R"~(dup-names:get	{"ok":{"$row":[["a",2]],"proto":"Object"}})~",
     R"~(dup-names:safe	{"ok":{"$row":[["a",{"$bigint":"2"}]],"proto":"Object"}})~"},
    {"dup-names-3", R"~(SELECT 1 AS a, 2 AS b, 3 AS a)~",
     R"~(dup-names-3:get	{"ok":{"$row":[["b",2],["a",3]],"proto":"Object"}})~",
     R"~(dup-names-3:safe	{"ok":{"$row":[["b",{"$bigint":"2"}],["a",{"$bigint":"3"}]],"proto":"Object"}})~"},
    {"dup-names-null", R"~(SELECT 1 AS a, NULL AS a)~",
     R"~(dup-names-null:get	{"ok":{"$row":[["a",null]],"proto":"Object"}})~",
     R"~(dup-names-null:safe	{"ok":{"$row":[["a",null]],"proto":"Object"}})~"},
    {"index-names", R"~(SELECT 1 AS "1", 2 AS "0", 3 AS b, 4 AS "10", 5 AS "-1", 6 AS "01")~",
     R"~(index-names:get	{"ok":{"$row":[["1",1],["0",2],["b",3],["10",4],["-1",5],["01",6]],"proto":"Object"}})~",
     R"~(index-names:safe	{"ok":{"$row":[["1",{"$bigint":"1"}],["0",{"$bigint":"2"}],["b",{"$bigint":"3"}],["10",{"$bigint":"4"}],["-1",{"$bigint":"5"}],["01",{"$bigint":"6"}]],"proto":"Object"}})~"},
    {"index-big", R"~(SELECT 1 AS "4294967294", 2 AS "4294967295", 3 AS a)~",
     R"~(index-big:get	{"ok":{"$row":[["4294967294",1],["4294967295",2],["a",3]],"proto":"Object"}})~",
     R"~(index-big:safe	{"ok":{"$row":[["4294967294",{"$bigint":"1"}],["4294967295",{"$bigint":"2"}],["a",{"$bigint":"3"}]],"proto":"Object"}})~"},
    {"expr-name", R"~(SELECT 1+1, 'x')~",
     R"~(expr-name:get	{"ok":{"$row":[["1+1",2],["'x'","x"]],"proto":"Object"}})~",
     R"~(expr-name:safe	{"ok":{"$row":[["1+1",{"$bigint":"2"}],["'x'","x"]],"proto":"Object"}})~"},
    {"empty-name", R"~(SELECT 1 AS "")~",
     R"~(empty-name:get	{"ok":{"$row":[["",1]],"proto":"Object"}})~",
     R"~(empty-name:safe	{"ok":{"$row":[["",{"$bigint":"1"}]],"proto":"Object"}})~"},
    {"proto-name", R"~(SELECT 1 AS __proto__, 2 AS b)~",
     R"~(proto-name:get	{"ok":{"$row":[["__proto__",1],["b",2]],"proto":"Object"}})~",
     R"~(proto-name:safe	{"ok":{"$row":[["__proto__",{"$bigint":"1"}],["b",{"$bigint":"2"}]],"proto":"Object"}})~"},
    {"unicode-name", R"~(SELECT 1 AS "é😀")~",
     R"~(unicode-name:get	{"ok":{"$row":[["é😀",1]],"proto":"Object"}})~",
     R"~(unicode-name:safe	{"ok":{"$row":[["é😀",{"$bigint":"1"}]],"proto":"Object"}})~"},
    {"no-rows-get", R"~(SELECT 1 AS v WHERE 0)~",
     R"~(no-rows-get:get	{"ok":null})~",
     R"~(no-rows-get:safe	{"ok":null})~"},
    {"mixed-types", R"~(SELECT 1 AS i, 1.5 AS r, 't' AS s, X'01' AS b, NULL AS n)~",
     R"~(mixed-types:get	{"ok":{"$row":[["i",1],["r",1.5],["s","t"],["b",{"$bytes":"01","ctor":"Uint8Array"}],["n",null]],"proto":"Object"}})~",
     R"~(mixed-types:safe	{"ok":{"$row":[["i",{"$bigint":"1"}],["r",1.5],["s","t"],["b",{"$bytes":"01","ctor":"Uint8Array"}],["n",null]],"proto":"Object"}})~"},
  };
}

TEST("values: INTEGER, REAL, TEXT, BLOB and NULL as bun:sqlite returns them, with and without safe integers") {
  const Closing db(Database(":memory:"));
  const Closing safe(Database(":memory:", {.safeIntegers = true}));
  for (const ValueCase& entry : kValueCases) {
    LINE(entry.get, std::string(entry.name) + ":get", return enc(db->query(entry.sql).get());)
    LINE(entry.safe, std::string(entry.name) + ":safe", return enc(safe->query(entry.sql).get());)
  }
  LINE(R"~(no-rows-all	{"ok":[]})~", "no-rows-all", return enc(db->query("SELECT 1 AS v WHERE 0").all());)
  LINE(R"~(all-rows	{"ok":[{"$row":[["v",1]],"proto":"Object"},{"$row":[["v",2]],"proto":"Object"}]})~", "all-rows", return enc(db->query("SELECT 1 AS v UNION ALL SELECT 2").all());)
  LINE(R"~(iterate-rows	{"ok":[{"$row":[["v",1]],"proto":"Object"},{"$row":[["v",2]],"proto":"Object"}]})~", "iterate-rows", {
    std::vector<Row> rows;
    for (Row& row : db->query("SELECT 1 AS v UNION ALL SELECT 2").iterate()) rows.push_back(row);
    return enc(rows);
  })
  db->run("CREATE TABLE t (i INTEGER, r REAL, s TEXT, b BLOB, n NUMERIC, a ANY)");
  db->run("INSERT INTO t VALUES (1, 1, 1, 1, 1, 1), ('2', '2', '2', '2', '2', '2'), (1.5, 1.5, 1.5, 1.5, 1.5, 1.5), ('x', 'x', 'x', 'x', 'x', 'x'), (X'01', X'01', X'01', X'01', X'01', X'01')");
  LINE(R"~(affinity	{"ok":[{"$row":[["i",1],["r",1],["s","1"],["b",1],["n",1],["a",1]],"proto":"Object"},{"$row":[["i",2],["r",2],["s","2"],["b","2"],["n",2],["a",2]],"proto":"Object"},{"$row":[["i",1.5],["r",1.5],["s","1.5"],["b",1.5],["n",1.5],["a",1.5]],"proto":"Object"},{"$row":[["i","x"],["r","x"],["s","x"],["b","x"],["n","x"],["a","x"]],"proto":"Object"},{"$row":[["i",{"$bytes":"01","ctor":"Uint8Array"}],["r",{"$bytes":"01","ctor":"Uint8Array"}],["s",{"$bytes":"01","ctor":"Uint8Array"}],["b",{"$bytes":"01","ctor":"Uint8Array"}],["n",{"$bytes":"01","ctor":"Uint8Array"}],["a",{"$bytes":"01","ctor":"Uint8Array"}]],"proto":"Object"}]})~", "affinity", return enc(db->query("SELECT * FROM t").all());)
  LINE(R"~(affinity-typeof	{"ok":[["integer","real","text","integer","integer","integer"],["integer","real","text","text","integer","integer"],["real","real","text","real","real","real"],["text","text","text","text","text","text"],["blob","blob","blob","blob","blob","blob"]]})~", "affinity-typeof", return values(db->query("SELECT typeof(i), typeof(r), typeof(s), typeof(b), typeof(n), typeof(a) FROM t").all());)
  LINE(R"~(big-int-sum	{"ok":{"$row":[["v",9223372036854776000]],"proto":"Object"}})~", "big-int-sum", return enc(db->query("SELECT 9223372036854775807 + 1 AS v").get());)
  LINE(R"~(int-overflow-sum	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"integer overflow","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: integer overflow","isSQLiteError":true}}})~", "int-overflow-sum", return enc(db->query("SELECT sum(x) AS v FROM (SELECT 9223372036854775807 AS x UNION ALL SELECT 1)").get());)
}

TEST("values: a repeated column name keeps its last column's value and position") {
  const Closing db(Database(":memory:"));
  LINE(R"~(dup-abcab	{"ok":{"$row":[["a",3],["b",4],["c",5]],"proto":"Object"}})~", "dup-abcab", return enc(db->query("SELECT 1 AS a, 2 AS b, 3 AS a, 4 AS b, 5 AS c").get());)
  LINE(R"~(dup-abca	{"ok":{"$row":[["b",2],["c",3],["a",4]],"proto":"Object"}})~", "dup-abca", return enc(db->query("SELECT 1 AS a, 2 AS b, 3 AS c, 4 AS a").get());)
  LINE(R"~(dup-aab	{"ok":{"$row":[["a",2],["b",3]],"proto":"Object"}})~", "dup-aab", return enc(db->query("SELECT 1 AS a, 2 AS a, 3 AS b").get());)
  LINE(R"~(dup-baab	{"ok":{"$row":[["a",3],["b",4]],"proto":"Object"}})~", "dup-baab", return enc(db->query("SELECT 1 AS b, 2 AS a, 3 AS a, 4 AS b").get());)
  LINE(R"~(dup-all	{"ok":[{"$row":[["a",3],["b",4],["c",5]],"proto":"Object"}]})~", "dup-all", return enc(db->query("SELECT 1 AS a, 2 AS b, 3 AS a, 4 AS b, 5 AS c").all());)
  LINE(R"~(dup-iterate	{"ok":[{"$row":[["b",2],["a",3]],"proto":"Object"}]})~", "dup-iterate", {
    std::vector<Row> rows;
    for (Row& row : db->query("SELECT 1 AS a, 2 AS b, 3 AS a").iterate()) rows.push_back(row);
    return enc(rows);
  })
  LINE(R"~(dup-case	{"ok":{"$row":[["a",1],["A",2]],"proto":"Object"}})~", "dup-case", return enc(db->query("SELECT 1 AS a, 2 AS A").get());)
  // Lone surrogates in SQL text reach SQLite as U+FFFD (scratch o8.ts).
  LINE(R"~(name-lone-surrogate	{"ok":{"$row":[["a�",1]],"proto":"Object"}})~", "name-lone-surrogate", return enc(db->query(rt::fromUtf16(u"SELECT 1 AS \"a\xD800\"")).get());)
  LINE(R"~(value-lone-in-sql	{"ok":{"$row":[["v","�"],["h","EFBFBD"],["w","x�y"],["hw","78EFBFBD79"]],"proto":"Object"}})~", "value-lone-in-sql",
       return enc(db->query(rt::fromUtf16(u"SELECT '\xD800' AS v, hex('\xD800') AS h, 'x\xD800" u"y' AS w, hex('x\xD800" u"y') AS hw")).get());)
}

TEST("values: rows convert to JSON objects where every column is JSON-shaped") {
  const Closing db(Database(":memory:"));
  const std::optional<Row> row = db->query("SELECT 1 AS i, 1.5 AS r, 't' AS s, NULL AS n, -0.0 AS z").get();
  ASSERT(row.has_value());
  EXPECT_EQ(rt::jsonStringify(json::value(sqlite::toJson(*row))), std::string(R"({"i":1,"r":1.5,"s":"t","n":null,"z":0})"));
  EXPECT_THROWS_MESSAGE(sqlite::toJson(*db->query("SELECT X'00' AS b").get()), rt::TypeError, "bun:sqlite value is a Uint8Array, not JSON");
  const Closing safe(Database(":memory:", {.safeIntegers = true}));
  EXPECT_THROWS_MESSAGE(sqlite::toJson(*safe->query("SELECT 1 AS i").get()), rt::TypeError, "bun:sqlite value is a bigint, not JSON");
  const Value* integer = row->get(std::string_view("i"));
  EXPECT(sqlite::getNumber(integer) != nullptr && *sqlite::getNumber(integer) == 1);
  EXPECT(sqlite::getBigInt(integer) == nullptr);
  EXPECT(sqlite::isNull(row->get(std::string_view("n"))));
  EXPECT(sqlite::getString(row->get(std::string_view("missing"))) == nullptr);
}

// ---- binding (scratch o3_bind.ts, o3c.ts, o3d.ts, o3e.ts) ---------------------------------------

namespace {
  // `d.query("SELECT typeof(?1) AS t, ?1 AS v, hex(?1) AS h").get(value)`
  json::value typed(const Database& db, const Binding& value) { return enc(db.query("SELECT typeof(?1) AS t, ?1 AS v, hex(?1) AS h").get(value)); }
  // The same call with an object argument (`get(new Date(0))`, `get({ a: 1 })`): named binding.
  json::value typedObject(const Database& db) { return enc(db.query("SELECT typeof(?1) AS t, ?1 AS v, hex(?1) AS h").get(Named{})); }
  // An object or Symbol inside a positional list.
  json::value anObject() {
    json::object object;
    object["a"] = 1;
    return object;
  }
  Binding big(const char* decimal) { return Binding(rt::BigInt::fromString(decimal)); }
  Bytes doubleBytes(double value) {
    Bytes bytes(sizeof value);
    std::memcpy(bytes.data(), &value, sizeof value);
    return bytes;
  }
}

TEST("binding: each JavaScript value kind binds as Bun binds it") {
  const Closing db(Database(":memory:"));
  const Closing sdb(Database(":memory:", {.safeIntegers = true}));
  LINE(R"~(bind:null	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind:null", return typed(db.database, nullptr);)
  LINE(R"~(bind-safe:null	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind-safe:null", return typed(sdb.database, nullptr);)
  LINE(R"~(bind:undefined	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind:undefined", return typed(db.database, std::nullopt);)
  LINE(R"~(bind-safe:undefined	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind-safe:undefined", return typed(sdb.database, std::nullopt);)
  LINE(R"~(bind:true	{"ok":{"$row":[["t","integer"],["v",1],["h","31"]],"proto":"Object"}})~", "bind:true", return typed(db.database, true);)
  LINE(R"~(bind-safe:true	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"1"}],["h","31"]],"proto":"Object"}})~", "bind-safe:true", return typed(sdb.database, true);)
  LINE(R"~(bind:false	{"ok":{"$row":[["t","integer"],["v",0],["h","30"]],"proto":"Object"}})~", "bind:false", return typed(db.database, false);)
  LINE(R"~(bind-safe:false	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"0"}],["h","30"]],"proto":"Object"}})~", "bind-safe:false", return typed(sdb.database, false);)
  LINE(R"~(bind:int	{"ok":{"$row":[["t","integer"],["v",42],["h","3432"]],"proto":"Object"}})~", "bind:int", return typed(db.database, 42.0);)
  LINE(R"~(bind-safe:int	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"42"}],["h","3432"]],"proto":"Object"}})~", "bind-safe:int", return typed(sdb.database, 42.0);)
  LINE(R"~(bind:neg-int	{"ok":{"$row":[["t","integer"],["v",-7],["h","2D37"]],"proto":"Object"}})~", "bind:neg-int", return typed(db.database, -7.0);)
  LINE(R"~(bind-safe:neg-int	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"-7"}],["h","2D37"]],"proto":"Object"}})~", "bind-safe:neg-int", return typed(sdb.database, -7.0);)
  LINE(R"~(bind:int32-max	{"ok":{"$row":[["t","integer"],["v",2147483647],["h","32313437343833363437"]],"proto":"Object"}})~", "bind:int32-max", return typed(db.database, 2147483647.0);)
  LINE(R"~(bind-safe:int32-max	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"2147483647"}],["h","32313437343833363437"]],"proto":"Object"}})~", "bind-safe:int32-max", return typed(sdb.database, 2147483647.0);)
  LINE(R"~(bind:int32-max+1	{"ok":{"$row":[["t","integer"],["v",2147483648],["h","32313437343833363438"]],"proto":"Object"}})~", "bind:int32-max+1", return typed(db.database, 2147483648.0);)
  LINE(R"~(bind-safe:int32-max+1	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"2147483648"}],["h","32313437343833363438"]],"proto":"Object"}})~", "bind-safe:int32-max+1", return typed(sdb.database, 2147483648.0);)
  LINE(R"~(bind:int32-min-1	{"ok":{"$row":[["t","integer"],["v",-2147483649],["h","2D32313437343833363439"]],"proto":"Object"}})~", "bind:int32-min-1", return typed(db.database, -2147483649.0);)
  LINE(R"~(bind-safe:int32-min-1	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"-2147483649"}],["h","2D32313437343833363439"]],"proto":"Object"}})~", "bind-safe:int32-min-1", return typed(sdb.database, -2147483649.0);)
  LINE(R"~(bind:2^51-1	{"ok":{"$row":[["t","integer"],["v",2251799813685247],["h","32323531373939383133363835323437"]],"proto":"Object"}})~", "bind:2^51-1", return typed(db.database, 2251799813685247.0);)
  LINE(R"~(bind-safe:2^51-1	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"2251799813685247"}],["h","32323531373939383133363835323437"]],"proto":"Object"}})~", "bind-safe:2^51-1", return typed(sdb.database, 2251799813685247.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind:2^51	{"ok":{"$row":[["t","real"],["v",2251799813685248],["h","322E3235313739393831333638353235652B3135"]],"proto":"Object"}})~", R"~(bind:2^51	{"ok":{"$row":[["t","real"],["v",2251799813685248],["h","323235313739393831333638353234382E30"]],"proto":"Object"}})~"), "bind:2^51", return typed(db.database, 2251799813685248.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind-safe:2^51	{"ok":{"$row":[["t","real"],["v",2251799813685248],["h","322E3235313739393831333638353235652B3135"]],"proto":"Object"}})~", R"~(bind-safe:2^51	{"ok":{"$row":[["t","real"],["v",2251799813685248],["h","323235313739393831333638353234382E30"]],"proto":"Object"}})~"), "bind-safe:2^51", return typed(sdb.database, 2251799813685248.0);)
  LINE(R"~(bind:-2^51	{"ok":{"$row":[["t","integer"],["v",-2251799813685248],["h","2D32323531373939383133363835323438"]],"proto":"Object"}})~", "bind:-2^51", return typed(db.database, -2251799813685248.0);)
  LINE(R"~(bind-safe:-2^51	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"-2251799813685248"}],["h","2D32323531373939383133363835323438"]],"proto":"Object"}})~", "bind-safe:-2^51", return typed(sdb.database, -2251799813685248.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind:-2^51-1	{"ok":{"$row":[["t","real"],["v",-2251799813685249],["h","2D322E3235313739393831333638353235652B3135"]],"proto":"Object"}})~", R"~(bind:-2^51-1	{"ok":{"$row":[["t","real"],["v",-2251799813685249],["h","2D323235313739393831333638353234392E30"]],"proto":"Object"}})~"), "bind:-2^51-1", return typed(db.database, -2251799813685249.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind-safe:-2^51-1	{"ok":{"$row":[["t","real"],["v",-2251799813685249],["h","2D322E3235313739393831333638353235652B3135"]],"proto":"Object"}})~", R"~(bind-safe:-2^51-1	{"ok":{"$row":[["t","real"],["v",-2251799813685249],["h","2D323235313739393831333638353234392E30"]],"proto":"Object"}})~"), "bind-safe:-2^51-1", return typed(sdb.database, -2251799813685249.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind:2^52	{"ok":{"$row":[["t","real"],["v",4503599627370496],["h","342E35303335393936323733373035652B3135"]],"proto":"Object"}})~", R"~(bind:2^52	{"ok":{"$row":[["t","real"],["v",4503599627370496],["h","343530333539393632373337303439362E30"]],"proto":"Object"}})~"), "bind:2^52", return typed(db.database, 4503599627370496.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind-safe:2^52	{"ok":{"$row":[["t","real"],["v",4503599627370496],["h","342E35303335393936323733373035652B3135"]],"proto":"Object"}})~", R"~(bind-safe:2^52	{"ok":{"$row":[["t","real"],["v",4503599627370496],["h","343530333539393632373337303439362E30"]],"proto":"Object"}})~"), "bind-safe:2^52", return typed(sdb.database, 4503599627370496.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind:2^53	{"ok":{"$row":[["t","real"],["v",9007199254740992],["h","392E3030373139393235343734303939652B3135"]],"proto":"Object"}})~", R"~(bind:2^53	{"ok":{"$row":[["t","real"],["v",9007199254740992],["h","393030373139393235343734303939322E30"]],"proto":"Object"}})~"), "bind:2^53", return typed(db.database, 9007199254740992.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind-safe:2^53	{"ok":{"$row":[["t","real"],["v",9007199254740992],["h","392E3030373139393235343734303939652B3135"]],"proto":"Object"}})~", R"~(bind-safe:2^53	{"ok":{"$row":[["t","real"],["v",9007199254740992],["h","393030373139393235343734303939322E30"]],"proto":"Object"}})~"), "bind-safe:2^53", return typed(sdb.database, 9007199254740992.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind:2^53+2	{"ok":{"$row":[["t","real"],["v",9007199254740994],["h","392E3030373139393235343734303939652B3135"]],"proto":"Object"}})~", R"~(bind:2^53+2	{"ok":{"$row":[["t","real"],["v",9007199254740994],["h","393030373139393235343734303939342E30"]],"proto":"Object"}})~"), "bind:2^53+2", return typed(db.database, 9007199254740994.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind-safe:2^53+2	{"ok":{"$row":[["t","real"],["v",9007199254740994],["h","392E3030373139393235343734303939652B3135"]],"proto":"Object"}})~", R"~(bind-safe:2^53+2	{"ok":{"$row":[["t","real"],["v",9007199254740994],["h","393030373139393235343734303939342E30"]],"proto":"Object"}})~"), "bind-safe:2^53+2", return typed(sdb.database, 9007199254740994.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind:2^63	{"ok":{"$row":[["t","real"],["v",9223372036854776000],["h","392E3232333337323033363835343738652B3138"]],"proto":"Object"}})~", R"~(bind:2^63	{"ok":{"$row":[["t","real"],["v",9223372036854776000],["h","392E32323333373230333638353437373538652B3138"]],"proto":"Object"}})~"), "bind:2^63", return typed(db.database, 9223372036854775808.0);)
  LINE(
    SQLITE_VERSIONED_EXPECTATION(R"~(bind-safe:2^63	{"ok":{"$row":[["t","real"],["v",9223372036854776000],["h","392E3232333337323033363835343738652B3138"]],"proto":"Object"}})~", R"~(bind-safe:2^63	{"ok":{"$row":[["t","real"],["v",9223372036854776000],["h","392E32323333373230333638353437373538652B3138"]],"proto":"Object"}})~"), "bind-safe:2^63", return typed(sdb.database, 9223372036854775808.0);)
  LINE(R"~(bind:1e21	{"ok":{"$row":[["t","real"],["v",1e+21],["h","312E30652B3231"]],"proto":"Object"}})~", "bind:1e21", return typed(db.database, 1e21);)
  LINE(R"~(bind-safe:1e21	{"ok":{"$row":[["t","real"],["v",1e+21],["h","312E30652B3231"]],"proto":"Object"}})~", "bind-safe:1e21", return typed(sdb.database, 1e21);)
  LINE(R"~(bind:1.5	{"ok":{"$row":[["t","real"],["v",1.5],["h","312E35"]],"proto":"Object"}})~", "bind:1.5", return typed(db.database, 1.5);)
  LINE(R"~(bind-safe:1.5	{"ok":{"$row":[["t","real"],["v",1.5],["h","312E35"]],"proto":"Object"}})~", "bind-safe:1.5", return typed(sdb.database, 1.5);)
  LINE(R"~(bind:-0	{"ok":{"$row":[["t","real"],["v",{"$number":"-0"}],["h","302E30"]],"proto":"Object"}})~", "bind:-0", return typed(db.database, -0.0);)
  LINE(R"~(bind-safe:-0	{"ok":{"$row":[["t","real"],["v",{"$number":"-0"}],["h","302E30"]],"proto":"Object"}})~", "bind-safe:-0", return typed(sdb.database, -0.0);)
  LINE(R"~(bind:0	{"ok":{"$row":[["t","integer"],["v",0],["h","30"]],"proto":"Object"}})~", "bind:0", return typed(db.database, 0.0);)
  LINE(R"~(bind-safe:0	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"0"}],["h","30"]],"proto":"Object"}})~", "bind-safe:0", return typed(sdb.database, 0.0);)
  LINE(R"~(bind:NaN	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind:NaN", return typed(db.database, std::numeric_limits<double>::quiet_NaN());)
  LINE(R"~(bind-safe:NaN	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind-safe:NaN", return typed(sdb.database, std::numeric_limits<double>::quiet_NaN());)
  LINE(R"~(bind:Infinity	{"ok":{"$row":[["t","real"],["v",{"$number":"Infinity"}],["h","496E66"]],"proto":"Object"}})~", "bind:Infinity", return typed(db.database, std::numeric_limits<double>::infinity());)
  LINE(R"~(bind-safe:Infinity	{"ok":{"$row":[["t","real"],["v",{"$number":"Infinity"}],["h","496E66"]],"proto":"Object"}})~", "bind-safe:Infinity", return typed(sdb.database, std::numeric_limits<double>::infinity());)
  LINE(R"~(bind:-Infinity	{"ok":{"$row":[["t","real"],["v",{"$number":"-Infinity"}],["h","2D496E66"]],"proto":"Object"}})~", "bind:-Infinity", return typed(db.database, -std::numeric_limits<double>::infinity());)
  LINE(R"~(bind-safe:-Infinity	{"ok":{"$row":[["t","real"],["v",{"$number":"-Infinity"}],["h","2D496E66"]],"proto":"Object"}})~", "bind-safe:-Infinity", return typed(sdb.database, -std::numeric_limits<double>::infinity());)
  LINE(R"~(bind:str-ascii	{"ok":{"$row":[["t","text"],["v","abc"],["h","616263"]],"proto":"Object"}})~", "bind:str-ascii", return typed(db.database, "abc");)
  LINE(R"~(bind-safe:str-ascii	{"ok":{"$row":[["t","text"],["v","abc"],["h","616263"]],"proto":"Object"}})~", "bind-safe:str-ascii", return typed(sdb.database, "abc");)
  LINE(R"~(bind:str-latin1	{"ok":{"$row":[["t","text"],["v","caféÿ"],["h","636166C3A9C3BF"]],"proto":"Object"}})~", "bind:str-latin1", return typed(db.database, "caf\xC3\xA9\xC3\xBF");)
  LINE(R"~(bind-safe:str-latin1	{"ok":{"$row":[["t","text"],["v","caféÿ"],["h","636166C3A9C3BF"]],"proto":"Object"}})~", "bind-safe:str-latin1", return typed(sdb.database, "caf\xC3\xA9\xC3\xBF");)
  LINE(R"~(bind:str-utf16	{"ok":{"$row":[["t","text"],["v","€中"],["h","E282ACE4B8AD"]],"proto":"Object"}})~", "bind:str-utf16", return typed(db.database, "\xE2\x82\xAC\xE4\xB8\xAD");)
  LINE(R"~(bind-safe:str-utf16	{"ok":{"$row":[["t","text"],["v","€中"],["h","E282ACE4B8AD"]],"proto":"Object"}})~", "bind-safe:str-utf16", return typed(sdb.database, "\xE2\x82\xAC\xE4\xB8\xAD");)
  LINE(R"~(bind:str-emoji	{"ok":{"$row":[["t","text"],["v","😀"],["h","F09F9880"]],"proto":"Object"}})~", "bind:str-emoji", return typed(db.database, "\xF0\x9F\x98\x80");)
  LINE(R"~(bind-safe:str-emoji	{"ok":{"$row":[["t","text"],["v","😀"],["h","F09F9880"]],"proto":"Object"}})~", "bind-safe:str-emoji", return typed(sdb.database, "\xF0\x9F\x98\x80");)
  LINE(R"~(bind:str-empty	{"ok":{"$row":[["t","text"],["v",""],["h",""]],"proto":"Object"}})~", "bind:str-empty", return typed(db.database, "");)
  LINE(R"~(bind-safe:str-empty	{"ok":{"$row":[["t","text"],["v",""],["h",""]],"proto":"Object"}})~", "bind-safe:str-empty", return typed(sdb.database, "");)
  LINE(R"~(bind:str-nul	{"ok":{"$row":[["t","text"],["v","a\u0000b"],["h","610062"]],"proto":"Object"}})~", "bind:str-nul", return typed(db.database, std::string("a\0b", 3));)
  LINE(R"~(bind-safe:str-nul	{"ok":{"$row":[["t","text"],["v","a\u0000b"],["h","610062"]],"proto":"Object"}})~", "bind-safe:str-nul", return typed(sdb.database, std::string("a\0b", 3));)
  LINE(R"~(bind:str-lone-high	{"ok":{"$row":[["t","text"],["v","a𐁢"],["h","61F09081A2"]],"proto":"Object"}})~", "bind:str-lone-high", return typed(db.database, rt::fromUtf16(u"a\xD800" u"b"));)
  LINE(R"~(bind-safe:str-lone-high	{"ok":{"$row":[["t","text"],["v","a𐁢"],["h","61F09081A2"]],"proto":"Object"}})~", "bind-safe:str-lone-high", return typed(sdb.database, rt::fromUtf16(u"a\xD800" u"b"));)
  LINE(R"~(bind:str-lone-low	{"ok":{"$row":[["t","text"],["v","a𐁢"],["h","61F09081A2"]],"proto":"Object"}})~", "bind:str-lone-low", return typed(db.database, rt::fromUtf16(u"a\xDC00" u"b"));)
  LINE(R"~(bind-safe:str-lone-low	{"ok":{"$row":[["t","text"],["v","a𐁢"],["h","61F09081A2"]],"proto":"Object"}})~", "bind-safe:str-lone-low", return typed(sdb.database, rt::fromUtf16(u"a\xDC00" u"b"));)
  LINE(R"~(bind:str-lone-high-end	{"ok":{"$row":[["t","text"],["v","a���"],["h","61EDA080"]],"proto":"Object"}})~", "bind:str-lone-high-end", return typed(db.database, rt::fromUtf16(u"a\xD800"));)
  LINE(R"~(bind-safe:str-lone-high-end	{"ok":{"$row":[["t","text"],["v","a���"],["h","61EDA080"]],"proto":"Object"}})~", "bind-safe:str-lone-high-end", return typed(sdb.database, rt::fromUtf16(u"a\xD800"));)
  LINE(R"~(bind:str-lone-low-start	{"ok":{"$row":[["t","text"],["v","𐁡"],["h","F09081A1"]],"proto":"Object"}})~", "bind:str-lone-low-start", return typed(db.database, rt::fromUtf16(u"\xDC00" u"a"));)
  LINE(R"~(bind-safe:str-lone-low-start	{"ok":{"$row":[["t","text"],["v","𐁡"],["h","F09081A1"]],"proto":"Object"}})~", "bind-safe:str-lone-low-start", return typed(sdb.database, rt::fromUtf16(u"\xDC00" u"a"));)
  LINE(R"~(bind:str-reversed-pair	{"ok":{"$row":[["t","text"],["v","򐀽"],["h","F29080BD"]],"proto":"Object"}})~", "bind:str-reversed-pair", return typed(db.database, rt::fromUtf16(u"\xDE00\xD83D"));)
  LINE(R"~(bind-safe:str-reversed-pair	{"ok":{"$row":[["t","text"],["v","򐀽"],["h","F29080BD"]],"proto":"Object"}})~", "bind-safe:str-reversed-pair", return typed(sdb.database, rt::fromUtf16(u"\xDE00\xD83D"));)
  LINE(R"~(bind:str-two-high	{"ok":{"$row":[["t","text"],["v","𐀁x"],["h","F090808178"]],"proto":"Object"}})~", "bind:str-two-high", return typed(db.database, rt::fromUtf16(u"\xD800\xD801" u"x"));)
  LINE(R"~(bind-safe:str-two-high	{"ok":{"$row":[["t","text"],["v","𐀁x"],["h","F090808178"]],"proto":"Object"}})~", "bind-safe:str-two-high", return typed(sdb.database, rt::fromUtf16(u"\xD800\xD801" u"x"));)
  LINE(R"~(bind:str-lone-high-latin	{"ok":{"$row":[["t","text"],["v","é���"],["h","C3A9EDA080"]],"proto":"Object"}})~", "bind:str-lone-high-latin", return typed(db.database, rt::fromUtf16(u"\x00E9\xD800"));)
  LINE(R"~(bind-safe:str-lone-high-latin	{"ok":{"$row":[["t","text"],["v","é���"],["h","C3A9EDA080"]],"proto":"Object"}})~", "bind-safe:str-lone-high-latin", return typed(sdb.database, rt::fromUtf16(u"\x00E9\xD800"));)
  LINE(R"~(bind:bigint-small	{"ok":{"$row":[["t","integer"],["v",5],["h","35"]],"proto":"Object"}})~", "bind:bigint-small", return typed(db.database, std::int64_t{5});)
  LINE(R"~(bind-safe:bigint-small	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"5"}],["h","35"]],"proto":"Object"}})~", "bind-safe:bigint-small", return typed(sdb.database, std::int64_t{5});)
  LINE(R"~(bind:bigint-neg	{"ok":{"$row":[["t","integer"],["v",-5],["h","2D35"]],"proto":"Object"}})~", "bind:bigint-neg", return typed(db.database, std::int64_t{-5});)
  LINE(R"~(bind-safe:bigint-neg	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"-5"}],["h","2D35"]],"proto":"Object"}})~", "bind-safe:bigint-neg", return typed(sdb.database, std::int64_t{-5});)
  LINE(R"~(bind:bigint-2^53+1	{"ok":{"$row":[["t","integer"],["v",9007199254740992],["h","39303037313939323534373430393933"]],"proto":"Object"}})~", "bind:bigint-2^53+1", return typed(db.database, std::int64_t{9007199254740993});)
  LINE(R"~(bind-safe:bigint-2^53+1	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"9007199254740993"}],["h","39303037313939323534373430393933"]],"proto":"Object"}})~", "bind-safe:bigint-2^53+1", return typed(sdb.database, std::int64_t{9007199254740993});)
  LINE(R"~(bind:bigint-max	{"ok":{"$row":[["t","integer"],["v",9223372036854776000],["h","39323233333732303336383534373735383037"]],"proto":"Object"}})~", "bind:bigint-max", return typed(db.database, std::numeric_limits<std::int64_t>::max());)
  LINE(R"~(bind-safe:bigint-max	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"9223372036854775807"}],["h","39323233333732303336383534373735383037"]],"proto":"Object"}})~", "bind-safe:bigint-max", return typed(sdb.database, std::numeric_limits<std::int64_t>::max());)
  LINE(R"~(bind:bigint-min	{"ok":{"$row":[["t","integer"],["v",-9223372036854776000],["h","2D39323233333732303336383534373735383038"]],"proto":"Object"}})~", "bind:bigint-min", return typed(db.database, std::numeric_limits<std::int64_t>::min());)
  LINE(R"~(bind-safe:bigint-min	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"-9223372036854775808"}],["h","2D39323233333732303336383534373735383038"]],"proto":"Object"}})~", "bind-safe:bigint-min", return typed(sdb.database, std::numeric_limits<std::int64_t>::min());)
  LINE(R"~(bind:bigint-2^63	{"ok":{"$row":[["t","integer"],["v",-9223372036854776000],["h","2D39323233333732303336383534373735383038"]],"proto":"Object"}})~", "bind:bigint-2^63", return typed(db.database, big("9223372036854775808"));)
  LINE(R"~(bind-safe:bigint-2^63	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '9223372036854775808' is out of range","keys":[],"string":"RangeError: BigInt value '9223372036854775808' is out of range","isSQLiteError":false}}})~", "bind-safe:bigint-2^63", return typed(sdb.database, big("9223372036854775808"));)
  LINE(R"~(bind:bigint-min-1	{"ok":{"$row":[["t","integer"],["v",9223372036854776000],["h","39323233333732303336383534373735383037"]],"proto":"Object"}})~", "bind:bigint-min-1", return typed(db.database, big("-9223372036854775809"));)
  LINE(R"~(bind-safe:bigint-min-1	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '-9223372036854775809' is out of range","keys":[],"string":"RangeError: BigInt value '-9223372036854775809' is out of range","isSQLiteError":false}}})~", "bind-safe:bigint-min-1", return typed(sdb.database, big("-9223372036854775809"));)
  LINE(R"~(bind:bigint-2^64+5	{"ok":{"$row":[["t","integer"],["v",5],["h","35"]],"proto":"Object"}})~", "bind:bigint-2^64+5", return typed(db.database, big("18446744073709551621"));)
  LINE(R"~(bind-safe:bigint-2^64+5	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '18446744073709551621' is out of range","keys":[],"string":"RangeError: BigInt value '18446744073709551621' is out of range","isSQLiteError":false}}})~", "bind-safe:bigint-2^64+5", return typed(sdb.database, big("18446744073709551621"));)
  LINE(R"~(bind:bigint-huge	{"ok":{"$row":[["t","integer"],["v",3],["h","33"]],"proto":"Object"}})~", "bind:bigint-huge", return typed(db.database, big("1267650600228229401496703205379"));)
  LINE(R"~(bind-safe:bigint-huge	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '1267650600228229401496703205379' is out of range","keys":[],"string":"RangeError: BigInt value '1267650600228229401496703205379' is out of range","isSQLiteError":false}}})~", "bind-safe:bigint-huge", return typed(sdb.database, big("1267650600228229401496703205379"));)
  LINE(R"~(bind:bigint-neg-huge	{"ok":{"$row":[["t","integer"],["v",-3],["h","2D33"]],"proto":"Object"}})~", "bind:bigint-neg-huge", return typed(db.database, big("-1267650600228229401496703205379"));)
  LINE(R"~(bind-safe:bigint-neg-huge	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '-1267650600228229401496703205379' is out of range","keys":[],"string":"RangeError: BigInt value '-1267650600228229401496703205379' is out of range","isSQLiteError":false}}})~", "bind-safe:bigint-neg-huge", return typed(sdb.database, big("-1267650600228229401496703205379"));)
  LINE(R"~(bind:u8	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0102ff","ctor":"Uint8Array"}],["h","0102FF"]],"proto":"Object"}})~", "bind:u8", return typed(db.database, Bytes{1, 2, 255});)
  LINE(R"~(bind-safe:u8	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0102ff","ctor":"Uint8Array"}],["h","0102FF"]],"proto":"Object"}})~", "bind-safe:u8", return typed(sdb.database, Bytes{1, 2, 255});)
  LINE(R"~(bind:u8-empty	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"","ctor":"Uint8Array"}],["h",""]],"proto":"Object"}})~", "bind:u8-empty", return typed(db.database, Bytes{});)
  LINE(R"~(bind-safe:u8-empty	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"","ctor":"Uint8Array"}],["h",""]],"proto":"Object"}})~", "bind-safe:u8-empty", return typed(sdb.database, Bytes{});)
  LINE(R"~(bind:u8-sub	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"010203","ctor":"Uint8Array"}],["h","010203"]],"proto":"Object"}})~", "bind:u8-sub", return typed(db.database, Bytes{1, 2, 3});)
  LINE(R"~(bind-safe:u8-sub	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"010203","ctor":"Uint8Array"}],["h","010203"]],"proto":"Object"}})~", "bind-safe:u8-sub", return typed(sdb.database, Bytes{1, 2, 3});)
  LINE(R"~(bind:buffer	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0708","ctor":"Uint8Array"}],["h","0708"]],"proto":"Object"}})~", "bind:buffer", return typed(db.database, Bytes{7, 8});)
  LINE(R"~(bind-safe:buffer	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0708","ctor":"Uint8Array"}],["h","0708"]],"proto":"Object"}})~", "bind-safe:buffer", return typed(sdb.database, Bytes{7, 8});)
  LINE(R"~(bind:i16	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0100ffff","ctor":"Uint8Array"}],["h","0100FFFF"]],"proto":"Object"}})~", "bind:i16", return typed(db.database, Bytes{1, 0, 255, 255});)
  LINE(R"~(bind-safe:i16	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0100ffff","ctor":"Uint8Array"}],["h","0100FFFF"]],"proto":"Object"}})~", "bind-safe:i16", return typed(sdb.database, Bytes{1, 0, 255, 255});)
  LINE(R"~(bind:f64	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"000000000000f03f","ctor":"Uint8Array"}],["h","000000000000F03F"]],"proto":"Object"}})~", "bind:f64", return typed(db.database, doubleBytes(1.0));)
  LINE(R"~(bind-safe:f64	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"000000000000f03f","ctor":"Uint8Array"}],["h","000000000000F03F"]],"proto":"Object"}})~", "bind-safe:f64", return typed(sdb.database, doubleBytes(1.0));)
  LINE(R"~(bind:dataview	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0607","ctor":"Uint8Array"}],["h","0607"]],"proto":"Object"}})~", "bind:dataview", return typed(db.database, Bytes{6, 7});)
  LINE(R"~(bind-safe:dataview	{"ok":{"$row":[["t","blob"],["v",{"$bytes":"0607","ctor":"Uint8Array"}],["h","0607"]],"proto":"Object"}})~", "bind-safe:dataview", return typed(sdb.database, Bytes{6, 7});)
  // An ArrayBuffer, a Date or a plain object as the single argument is an object of named parameters.
  LINE(R"~(bind:arraybuffer	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind:arraybuffer", return typedObject(db.database);)
  LINE(R"~(bind-safe:arraybuffer	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind-safe:arraybuffer", return typedObject(sdb.database);)
  LINE(R"~(bind:date	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind:date", return typedObject(db.database);)
  LINE(R"~(bind-safe:date	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind-safe:date", return typedObject(sdb.database);)
  LINE(R"~(bind:object	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind:object", return typedObject(db.database);)
  LINE(R"~(bind-safe:object	{"ok":{"$row":[["t","null"],["v",null],["h",""]],"proto":"Object"}})~", "bind-safe:object", return typedObject(sdb.database);)
  // A Symbol or a function is not bindable: here an object inside the positional list.
  LINE(R"~(bind:symbol	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "bind:symbol", return typed(db.database, anObject());)
  LINE(R"~(bind-safe:symbol	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "bind-safe:symbol", return typed(sdb.database, anObject());)
  LINE(R"~(bind:function	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "bind:function", return typed(db.database, anObject());)
  LINE(R"~(bind-safe:function	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "bind-safe:function", return typed(sdb.database, anObject());)
  // `get([1])`: an array argument is the positional list.
  LINE(R"~(bind:array-nested	{"ok":{"$row":[["t","integer"],["v",1],["h","31"]],"proto":"Object"}})~", "bind:array-nested", return enc(db.database.query("SELECT typeof(?1) AS t, ?1 AS v, hex(?1) AS h").get(std::vector<Binding>{1.0}));)
  LINE(R"~(bind-safe:array-nested	{"ok":{"$row":[["t","integer"],["v",{"$bigint":"1"}],["h","31"]],"proto":"Object"}})~", "bind-safe:array-nested", return enc(sdb.database.query("SELECT typeof(?1) AS t, ?1 AS v, hex(?1) AS h").get(std::vector<Binding>{1.0}));)
}

TEST("binding: positional parameters, counts and kept bindings") {
  const Closing db(Database(":memory:"));
  const Statement two = db->query("SELECT ? AS a, ? AS b");
  LINE(R"~(pos-spread	{"ok":{"$row":[["a",1],["b","x"]],"proto":"Object"}})~", "pos-spread", return enc(two.get(1.0, "x"));)
  LINE(R"~(pos-array	{"ok":{"$row":[["a",1],["b","x"]],"proto":"Object"}})~", "pos-array", return enc(two.get(std::vector<Binding>{1.0, "x"}));)
  LINE(R"~(pos-too-few	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 2 values, received 1","keys":[],"string":"Error: SQLite query expected 2 values, received 1","isSQLiteError":false}}})~", "pos-too-few", return enc(two.get(1.0));)
  LINE(R"~(pos-too-many	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 2 values, received 3","keys":[],"string":"Error: SQLite query expected 2 values, received 3","isSQLiteError":false}}})~", "pos-too-many", return enc(two.get(1.0, 2.0, 3.0));)
  LINE(R"~(pos-array-too-few	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 2 values, received 1","keys":[],"string":"Error: SQLite query expected 2 values, received 1","isSQLiteError":false}}})~", "pos-array-too-few", return enc(two.get(std::vector<Binding>{1.0}));)
  LINE(R"~(pos-none-keeps	{"ok":{"$row":[["a",5],["b",6]],"proto":"Object"}})~", "pos-none-keeps", {
    two.get(5.0, 6.0);
    return enc(two.get());
  })
  LINE(R"~(pos-none-all-keeps	{"ok":[{"$row":[["a",7],["b",8]],"proto":"Object"}]})~", "pos-none-all-keeps", {
    two.get(7.0, 8.0);
    return enc(two.all());
  })
  LINE(R"~(pos-object-arg-on-positional	{"ok":{"$row":[["a",null],["b",null]],"proto":"Object"}})~", "pos-object-arg-on-positional", return enc(two.get(Named{{"a", 1.0}}));)
  LINE(R"~(pos-object-1-on-positional	{"ok":{"$row":[["a",null],["b","one"]],"proto":"Object"}})~", "pos-object-1-on-positional", return enc(two.get(Named{{"1", "one"}, {"2", "two"}}));)
  LINE(R"~(pos-u8-first	{"ok":{"$row":[["a",{"$bytes":"01","ctor":"Uint8Array"}],["b",2]],"proto":"Object"}})~", "pos-u8-first", return enc(two.get(Bytes{1}, 2.0));)
  LINE(R"~(pos-null-first	{"ok":{"$row":[["a",null],["b",2]],"proto":"Object"}})~", "pos-null-first", return enc(two.get(nullptr, 2.0));)
  LINE(R"~(pos-undefined-first	{"ok":{"$row":[["a",null],["b",2]],"proto":"Object"}})~", "pos-undefined-first", return enc(two.get(std::nullopt, 2.0));)
  LINE(R"~(pos-zero-arg-statement	{"ok":{"$row":[["one",1]],"proto":"Object"}})~", "pos-zero-arg-statement", return enc(db->query("SELECT 1 AS one").get(1.0, 2.0, 3.0));)
  LINE(R"~(pos-zero-arg-statement-obj	{"ok":[{"$row":[["one",1]],"proto":"Object"}]})~", "pos-zero-arg-statement-obj", return enc(db->query("SELECT 1 AS one").all(Named{{"$x", 1.0}}));)
  LINE(R"~(numbered	{"ok":{"$row":[["a",2],["b",1]],"proto":"Object"}})~", "numbered", return enc(db->query("SELECT ?2 AS a, ?1 AS b").get(1.0, 2.0));)
  LINE(R"~(numbered-count	{"ok":5})~", "numbered-count", return db->query("SELECT ?2 AS a, ?1 AS b, ?5 AS c").paramsCount();)
  LINE(R"~(numbered-gap-bind	{"ok":{"$row":[["a",3]],"proto":"Object"}})~", "numbered-gap-bind", return enc(db->query("SELECT ?3 AS a").get(1.0, 2.0, 3.0));)
  LINE(R"~(numbered-gap-bind-1	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 3 values, received 1","keys":[],"string":"Error: SQLite query expected 3 values, received 1","isSQLiteError":false}}})~", "numbered-gap-bind-1", return enc(db->query("SELECT ?3 AS a").get(1.0));)
  LINE(R"~(paramsCount	{"ok":[1,4,0]})~", "paramsCount",
       return list({db->query("SELECT ?").paramsCount(), db->query("SELECT $a, $a, :b, @c, ?").paramsCount(), db->query("SELECT 1").paramsCount()});)
}

TEST("binding: object parameters with and without strict") {
  const Closing db(Database(":memory:"));
  const Closing strict(Database(":memory:", {.strict = true}));
  const Statement named = db->query("SELECT $a AS a, :b AS b, @c AS c");
  LINE(R"~(named-prefixed	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "named-prefixed", return enc(named.get(Named{{"$a", 1.0}, {":b", 2.0}, {"@c", 3.0}}));)
  LINE(R"~(named-unprefixed	{"ok":{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}})~", "named-unprefixed", return enc(named.get(Named{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}}));)
  LINE(R"~(named-missing	{"ok":{"$row":[["a",1],["b",null],["c",null]],"proto":"Object"}})~", "named-missing", return enc(named.get(Named{{"$a", 1.0}}));)
  LINE(R"~(named-extra	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "named-extra", return enc(named.get(Named{{"$a", 1.0}, {":b", 2.0}, {"@c", 3.0}, {"$z", 9.0}}));)
  LINE(R"~(named-undefined-value	{"ok":{"$row":[["a",null],["b",null],["c",3]],"proto":"Object"}})~", "named-undefined-value", return enc(named.get(Named{{"$a", std::nullopt}, {":b", nullptr}, {"@c", 3.0}}));)
  LINE(R"~(named-array-arg	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "named-array-arg", return enc(named.get(std::vector<Binding>{1.0, 2.0, 3.0}));)
  LINE(R"~(named-spread	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "named-spread", return enc(named.get(1.0, 2.0, 3.0));)
  LINE(R"~(named-dup	{"ok":{"$row":[["x",4],["y",4]],"proto":"Object"}})~", "named-dup", return enc(db->query("SELECT $a AS x, $a AS y").get(Named{{"$a", 4.0}}));)
  LINE(R"~(named-dup-positional	{"ok":{"$row":[["x",4],["y",4]],"proto":"Object"}})~", "named-dup-positional", return enc(db->query("SELECT $a AS x, $a AS y").get(4.0));)
  LINE(R"~(named-mixed-q	{"ok":{"$row":[["x",1],["y",null]],"proto":"Object"}})~", "named-mixed-q", return enc(db->query("SELECT $a AS x, ? AS y").get(Named{{"$a", 1.0}}));)
  LINE(R"~(named-mixed-q-pos	{"ok":{"$row":[["x",1],["y",2]],"proto":"Object"}})~", "named-mixed-q-pos", return enc(db->query("SELECT $a AS x, ? AS y").get(1.0, 2.0));)
  LINE(R"~(named-wrong-prefix	{"ok":{"$row":[["x",null]],"proto":"Object"}})~", "named-wrong-prefix", return enc(db->query("SELECT $a AS x").get(Named{{":a", 1.0}}));)
  // `Object.create({ $a: 7 })`: inherited properties are not read; it has no own keys.
  LINE(R"~(named-proto	{"ok":{"$row":[["x",null]],"proto":"Object"}})~", "named-proto", return enc(db->query("SELECT $a AS x").get(Named{}));)
  LINE(R"~(named-number-key	{"ok":{"$row":[["x",null],["y",null]],"proto":"Object"}})~", "named-number-key", return enc(db->query("SELECT ?1 AS x, ?2 AS y").get(Named{{"1", "a"}, {"2", "b"}}));)
  LINE(R"~(named-keeps-after-missing	{"ok":{"$row":[["a",9],["b",null],["c",null]],"proto":"Object"}})~", "named-keeps-after-missing", {
    named.get(Named{{"$a", 1.0}, {":b", 2.0}, {"@c", 3.0}});
    return enc(named.get(Named{{"$a", 9.0}}));
  })
  const Statement sn = strict->query("SELECT $a AS a, :b AS b, @c AS c");
  LINE(R"~(strict-unprefixed	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "strict-unprefixed", return enc(sn.get(Named{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}}));)
  LINE(R"~(strict-prefixed	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"a\"","keys":[],"string":"Error: Missing parameter \"a\"","isSQLiteError":false}}})~", "strict-prefixed", return enc(sn.get(Named{{"$a", 1.0}, {":b", 2.0}, {"@c", 3.0}}));)
  LINE(R"~(strict-missing	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"c\"","keys":[],"string":"Error: Missing parameter \"c\"","isSQLiteError":false}}})~", "strict-missing", return enc(sn.get(Named{{"a", 1.0}, {"b", 2.0}}));)
  LINE(R"~(strict-missing-first	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"a\"","keys":[],"string":"Error: Missing parameter \"a\"","isSQLiteError":false}}})~", "strict-missing-first", return enc(sn.get(Named{{"c", 1.0}}));)
  LINE(R"~(strict-extra	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "strict-extra", return enc(sn.get(Named{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}, {"d", 4.0}}));)
  LINE(R"~(strict-undefined	{"ok":{"$row":[["a",null],["b",2],["c",3]],"proto":"Object"}})~", "strict-undefined", return enc(sn.get(Named{{"a", std::nullopt}, {"b", 2.0}, {"c", 3.0}}));)
  LINE(R"~(strict-positional	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", "strict-positional", return enc(sn.get(1.0, 2.0, 3.0));)
  LINE(R"~(strict-q	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"1\"","keys":[],"string":"Error: Missing parameter \"1\"","isSQLiteError":false}}})~", "strict-q", return enc(strict->query("SELECT ? AS x").get(Named{{"a", 1.0}}));)
  LINE(R"~(strict-q-numbered-obj	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?1\"","keys":[],"string":"Error: Missing parameter \"?1\"","isSQLiteError":false}}})~", "strict-q-numbered-obj", return enc(strict->query("SELECT ?1 AS x").get(Named{{"1", 5.0}}));)
  LINE(R"~(strict-mixed	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"1\"","keys":[],"string":"Error: Missing parameter \"1\"","isSQLiteError":false}}})~", "strict-mixed", return enc(strict->query("SELECT $a AS x, ? AS y").get(Named{{"a", 1.0}}));)
  LINE(R"~(strict-dup	{"ok":{"$row":[["x",4],["y",4]],"proto":"Object"}})~", "strict-dup", return enc(strict->query("SELECT $a AS x, $a AS y").get(Named{{"a", 4.0}}));)
  // `?` and `?NNN` parameters in an object binding (scratch o3c.ts, o3d.ts).
  const Statement three = db->query("SELECT ? AS a, ? AS b, ? AS c");
  const Statement sthree = strict->query("SELECT ? AS a, ? AS b, ? AS c");
  const Named digits{{"0", "z"}, {"1", "one"}, {"2", "two"}, {"3", "three"}};
  LINE(R"~(obj-unnamed-0based	{"ok":{"$row":[["a","z"],["b","one"],["c","two"]],"proto":"Object"}})~", "obj-unnamed-0based", return enc(three.get(digits));)
  LINE(R"~(obj-unnamed-strict	{"ok":{"$row":[["a","z"],["b","one"],["c","two"]],"proto":"Object"}})~", "obj-unnamed-strict", return enc(sthree.get(digits));)
  LINE(R"~(obj-unnamed-strict-123	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"1\"","keys":[],"string":"Error: Missing parameter \"1\"","isSQLiteError":false}}})~", "obj-unnamed-strict-123", return enc(sthree.get(Named{{"1", "one"}, {"2", "two"}, {"3", "three"}}));)
  LINE(R"~(obj-mixed-named-unnamed	{"ok":{"$row":[["a","X"],["b","one"]],"proto":"Object"}})~", "obj-mixed-named-unnamed",
       return enc(db->query("SELECT $x AS a, ? AS b").get(Named{{"0", "zero"}, {"1", "one"}, {"2", "two"}, {"$x", "X"}}));)
  LINE(R"~(obj-mixed-strict	{"ok":{"$row":[["a","X"],["b","one"]],"proto":"Object"}})~", "obj-mixed-strict", return enc(strict->query("SELECT $x AS a, ? AS b").get(Named{{"0", "zero"}, {"1", "one"}, {"2", "two"}, {"x", "X"}}));)
  const Named numberedKeys{{"0", "zero"}, {"1", "one"}, {"2", "two"}, {"?1", "q1"}, {"?2", "q2"}};
  LINE(R"~(obj-numbered	{"ok":{"$row":[["a","q2"],["b","q1"]],"proto":"Object"}})~", "obj-numbered", return enc(db->query("SELECT ?2 AS a, ?1 AS b").get(numberedKeys));)
  LINE(R"~(obj-numbered-strict	{"ok":{"$row":[["a","one"],["b","zero"]],"proto":"Object"}})~", "obj-numbered-strict", return enc(strict->query("SELECT ?2 AS a, ?1 AS b").get(numberedKeys));)
  const Statement s3 = strict->query("SELECT ? AS a, ? AS b, ? AS c");
  LINE(R"~(strict3 {}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"1\"","keys":[],"string":"Error: Missing parameter \"1\"","isSQLiteError":false}}})~", "strict3 {}", return enc(s3.get(Named{}));)
  LINE(R"~(strict3 {"0":1}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"2\"","keys":[],"string":"Error: Missing parameter \"2\"","isSQLiteError":false}}})~", R"(strict3 {"0":1})", return enc(s3.get(Named{{"0", 1.0}}));)
  LINE(R"~(strict3 {"0":1,"1":2}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"3\"","keys":[],"string":"Error: Missing parameter \"3\"","isSQLiteError":false}}})~", R"(strict3 {"0":1,"1":2})", return enc(s3.get(Named{{"0", 1.0}, {"1", 2.0}}));)
  LINE(R"~(strict3 {"1":1,"2":2}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"1\"","keys":[],"string":"Error: Missing parameter \"1\"","isSQLiteError":false}}})~", R"(strict3 {"1":1,"2":2})", return enc(s3.get(Named{{"1", 1.0}, {"2", 2.0}}));)
  LINE(R"~(strict3 {"0":1,"2":3}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"2\"","keys":[],"string":"Error: Missing parameter \"2\"","isSQLiteError":false}}})~", R"(strict3 {"0":1,"2":3})", return enc(s3.get(Named{{"0", 1.0}, {"2", 3.0}}));)
  const Statement mix = strict->query("SELECT $x AS a, ? AS b, ? AS c");
  LINE(R"~(strictmix {"x":1}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"1\"","keys":[],"string":"Error: Missing parameter \"1\"","isSQLiteError":false}}})~", R"(strictmix {"x":1})", return enc(mix.get(Named{{"x", 1.0}}));)
  LINE(R"~(strictmix {"1":2,"x":1}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"2\"","keys":[],"string":"Error: Missing parameter \"2\"","isSQLiteError":false}}})~", R"(strictmix {"1":2,"x":1})", return enc(mix.get(Named{{"1", 2.0}, {"x", 1.0}}));)
  LINE(R"~(strictmix {"1":2,"2":3,"x":1}	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", R"(strictmix {"1":2,"2":3,"x":1})", return enc(mix.get(Named{{"1", 2.0}, {"2", 3.0}, {"x", 1.0}}));)
  LINE(R"~(strictmix {"0":9,"1":2,"2":3,"x":1}	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", R"(strictmix {"0":9,"1":2,"2":3,"x":1})", return enc(mix.get(Named{{"0", 9.0}, {"1", 2.0}, {"2", 3.0}, {"x", 1.0}}));)
  const Statement num = strict->query("SELECT ?3 AS a, ?1 AS b");
  LINE(R"~(strictnum {}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?1\"","keys":[],"string":"Error: Missing parameter \"?1\"","isSQLiteError":false}}})~", "strictnum {}", return enc(num.get(Named{}));)
  LINE(R"~(strictnum {"0":1}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?2\"","keys":[],"string":"Error: Missing parameter \"?2\"","isSQLiteError":false}}})~", R"(strictnum {"0":1})", return enc(num.get(Named{{"0", 1.0}}));)
  LINE(R"~(strictnum {"0":1,"1":2}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?3\"","keys":[],"string":"Error: Missing parameter \"?3\"","isSQLiteError":false}}})~", R"(strictnum {"0":1,"1":2})", return enc(num.get(Named{{"0", 1.0}, {"1", 2.0}}));)
  LINE(R"~(strictnum {"0":1,"1":2,"2":3}	{"ok":{"$row":[["a",3],["b",1]],"proto":"Object"}})~", R"(strictnum {"0":1,"1":2,"2":3})", return enc(num.get(Named{{"0", 1.0}, {"1", 2.0}, {"2", 3.0}}));)
  LINE(R"~(strictnum {"?1":1,"?3":3}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?1\"","keys":[],"string":"Error: Missing parameter \"?1\"","isSQLiteError":false}}})~", R"(strictnum {"?1":1,"?3":3})", return enc(num.get(Named{{"?1", 1.0}, {"?3", 3.0}}));)
  LINE(R"~(strictnum {"1":1,"3":3}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?1\"","keys":[],"string":"Error: Missing parameter \"?1\"","isSQLiteError":false}}})~", R"(strictnum {"1":1,"3":3})", return enc(num.get(Named{{"1", 1.0}, {"3", 3.0}}));)
  const Statement num2 = db->query("SELECT ?3 AS a, ?1 AS b");
  LINE(R"~(num {}	{"ok":{"$row":[["a",null],["b",null]],"proto":"Object"}})~", "num {}", return enc(num2.get(Named{}));)
  LINE(R"~(num {"0":1,"1":2,"2":3}	{"ok":{"$row":[["a",null],["b",null]],"proto":"Object"}})~", R"(num {"0":1,"1":2,"2":3})", return enc(num2.get(Named{{"0", 1.0}, {"1", 2.0}, {"2", 3.0}}));)
  LINE(R"~(num {"?1":1,"?3":3}	{"ok":{"$row":[["a",3],["b",1]],"proto":"Object"}})~", R"(num {"?1":1,"?3":3})", return enc(num2.get(Named{{"?1", 1.0}, {"?3", 3.0}}));)
  LINE(R"~(num {"1":1,"3":3}	{"ok":{"$row":[["a",null],["b",null]],"proto":"Object"}})~", R"(num {"1":1,"3":3})", return enc(num2.get(Named{{"1", 1.0}, {"3", 3.0}}));)
  LINE(R"~(mix {"0":7,"1":2,"2":3,"$x":1}	{"ok":{"$row":[["a",1],["b",2],["c",3]],"proto":"Object"}})~", R"(mix {"0":7,"1":2,"2":3,"$x":1})",
       return enc(db->query("SELECT $x AS a, ? AS b, ? AS c").get(Named{{"0", 7.0}, {"1", 2.0}, {"2", 3.0}, {"$x", 1.0}}));)
  LINE(R"~(mix3 {"0":7,"1":2,"2":3,"$x":1}	{"ok":{"$row":[["a",7],["b",1],["c",3]],"proto":"Object"}})~", R"(mix3 {"0":7,"1":2,"2":3,"$x":1})",
       return enc(db->query("SELECT ? AS a, $x AS b, ? AS c").get(Named{{"0", 7.0}, {"1", 2.0}, {"2", 3.0}, {"$x", 1.0}}));)
  const Statement smix3 = strict->query("SELECT ? AS a, $x AS b, ? AS c");
  LINE(R"~(smix3 {"0":7,"1":2,"2":3,"x":1}	{"ok":{"$row":[["a",7],["b",1],["c",3]],"proto":"Object"}})~", R"(smix3 {"0":7,"1":2,"2":3,"x":1})", return enc(smix3.get(Named{{"0", 7.0}, {"1", 2.0}, {"2", 3.0}, {"x", 1.0}}));)
  LINE(R"~(smix3 {"0":7,"x":1}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"2\"","keys":[],"string":"Error: Missing parameter \"2\"","isSQLiteError":false}}})~", R"(smix3 {"0":7,"x":1})", return enc(smix3.get(Named{{"0", 7.0}, {"x", 1.0}}));)
  LINE(R"~(smix3 {"2":7,"x":1}	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"0\"","keys":[],"string":"Error: Missing parameter \"0\"","isSQLiteError":false}}})~", R"(smix3 {"2":7,"x":1})", return enc(smix3.get(Named{{"2", 7.0}, {"x", 1.0}}));)
  // scratch o20.ts
  LINE(R"~(strict-numbered-with-named	{"ok":{"$row":[["a",1],["b","one"]],"proto":"Object"}})~", "strict-numbered-with-named",
       return enc(strict->query("SELECT $a AS a, ?2 AS b").get(Named{{"0", "zero"}, {"1", "one"}, {"a", 1.0}, {"?2", "q"}}));)
  LINE(R"~(strict-numbered-with-named-missing	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"?2\"","keys":[],"string":"Error: Missing parameter \"?2\"","isSQLiteError":false}}})~", "strict-numbered-with-named-missing", return enc(strict->query("SELECT $a AS a, ?2 AS b").get(Named{{"a", 1.0}}));)
  LINE(R"~(numbered-with-named	{"ok":{"$row":[["a",1],["b","q"]],"proto":"Object"}})~", "numbered-with-named", return enc(db->query("SELECT $a AS a, ?2 AS b").get(Named{{"0", "zero"}, {"1", "one"}, {"$a", 1.0}, {"?2", "q"}}));)
}

TEST("binding: a failed binding keeps the values bound before it, except a RangeError") {
  const Closing db(Database(":memory:"));
  const Closing sdb(Database(":memory:", {.safeIntegers = true}));
  const Closing strict(Database(":memory:", {.strict = true}));
  // A Symbol, a Date or a plain object inside a positional list: TypeError (scratch o3c.ts).
  const Statement two = db->query("SELECT ? AS a, ? AS b");
  LINE(R"~(elem-object	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "elem-object", return enc(two.get(1.0, anObject()));)
  LINE(R"~(elem-array	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "elem-array", return enc(two.get(1.0, json::value(json::array{1})));)
  LINE(R"~(array-arg-object-elem	{"threw":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "array-arg-object-elem", return enc(two.get(std::vector<Binding>{1.0, anObject()}));)
  LINE(R"~(bind-fail-then-noargs	{"ok":{"$row":[["a",3],["b",null]],"proto":"Object"}})~", "bind-fail-then-noargs", {
    two.get(1.0, 2.0);
    try {
      two.get(3.0, anObject());
    } catch (const rt::JsError&) {
    }
    return enc(two.get());
  })
  LINE(R"~(bind-fail-count-then-noargs	{"ok":{"$row":[["a",null],["b",null]],"proto":"Object"}})~", "bind-fail-count-then-noargs", {
    two.get(1.0, 2.0);
    try {
      two.get(3.0);
    } catch (const rt::JsError&) {
    }
    return enc(two.get());
  })
  LINE(R"~(bind-fail-range-then-noargs	{"ok":{"$row":[["a",null],["b",null]],"proto":"Object"}})~", "bind-fail-range-then-noargs", {
    const Statement same = db->query("SELECT ? AS a, ? AS b");
    same.safeIntegers(true);
    same.get(1.0, 2.0);
    try {
      same.get(3.0, big("18446744073709551616"));
    } catch (const rt::JsError&) {
    }
    return enc(same.get());
  })
  // scratch o3e.ts
  const auto after = [](const Statement& statement, const std::vector<Binding>& bad) -> json::value {
    statement.get(1.0, 2.0, 3.0);
    try {
      statement.get(bad);
    } catch (const rt::JsError& error) {
      return list({text(error.toString()), enc(statement.get())});
    }
    return list({text("no throw"), enc(statement.get())});
  };
  const Statement s = db->prepare("SELECT ? AS a, ? AS b, ? AS c");
  const Statement ss = sdb->prepare("SELECT ? AS a, ? AS b, ? AS c");
  LINE(R"~(type-at-3	{"ok":["TypeError: Binding expected string, TypedArray, boolean, number, bigint or null",{"$row":[["a",7],["b",8],["c",null]],"proto":"Object"}]})~", "type-at-3", return after(s, {7.0, 8.0, anObject()});)
  LINE(R"~(type-at-1	{"ok":["TypeError: Binding expected string, TypedArray, boolean, number, bigint or null",{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}]})~", "type-at-1", return after(s, {anObject(), 8.0, 9.0});)
  LINE(R"~(range-at-3	{"ok":["RangeError: BigInt value '18446744073709551616' is out of range",{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}]})~", "range-at-3", return after(ss, {7.0, 8.0, big("18446744073709551616")});)
  LINE(R"~(range-at-2	{"ok":["RangeError: BigInt value '18446744073709551616' is out of range",{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}]})~", "range-at-2", return after(ss, {7.0, big("18446744073709551616"), 9.0});)
  LINE(R"~(range-at-1	{"ok":["RangeError: BigInt value '18446744073709551616' is out of range",{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}]})~", "range-at-1", return after(ss, {big("18446744073709551616"), 8.0, 9.0});)
  const Statement n = strict->prepare("SELECT $a AS a, $b AS b, $c AS c");
  LINE(R"~(missing-at-3	{"ok":["Error: Missing parameter \"c\"",{"$row":[["a",7],["b",8],["c",null]],"proto":"Object"}]})~", "missing-at-3", {
    n.get(Named{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}});
    try {
      n.get(Named{{"a", 7.0}, {"b", 8.0}});
    } catch (const rt::JsError& error) {
      return list({text(error.toString()), enc(n.get())});
    }
    return nullptr;
  })
  LINE(R"~(missing-at-1	{"ok":["Error: Missing parameter \"a\"",{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}]})~", "missing-at-1", {
    n.get(Named{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}});
    try {
      n.get(Named{{"b", 8.0}, {"c", 9.0}});
    } catch (const rt::JsError& error) {
      return list({text(error.toString()), enc(n.get())});
    }
    return nullptr;
  })
  const Statement n2 = sdb->prepare("SELECT $a AS a, $b AS b, $c AS c");
  LINE(R"~(named-range-at-2	{"ok":["RangeError: BigInt value '18446744073709551616' is out of range",{"$row":[["a",null],["b",null],["c",null]],"proto":"Object"}]})~", "named-range-at-2", {
    n2.get(Named{{"$a", 1.0}, {"$b", 2.0}, {"$c", 3.0}});
    try {
      n2.get(Named{{"$a", 7.0}, {"$b", big("18446744073709551616")}, {"$c", 9.0}});
    } catch (const rt::JsError& error) {
      return list({text(error.toString()), enc(n2.get())});
    }
    return nullptr;
  })
  const Statement n3 = db->prepare("SELECT $a AS a, $b AS b, $c AS c");
  LINE(R"~(named-type-at-2	{"ok":["TypeError: Binding expected string, TypedArray, boolean, number, bigint or null",{"$row":[["a",7],["b",null],["c",null]],"proto":"Object"}]})~", "named-type-at-2", {
    n3.get(Named{{"$a", 1.0}, {"$b", 2.0}, {"$c", 3.0}});
    try {
      n3.get(Named{{"$a", 7.0}, {"$b", anObject()}, {"$c", 9.0}});
    } catch (const rt::JsError& error) {
      return list({text(error.toString()), enc(n3.get())});
    }
    return nullptr;
  })
}

TEST("binding: safeIntegers per statement, from the database default") {
  const Closing db(Database(":memory:"));
  const Closing sdb(Database(":memory:", {.safeIntegers = true}));
  const Statement s1 = db->query("SELECT ? AS v");
  LINE(R"~(stmt-safe-toggle	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '9223372036854775808' is out of range","keys":[],"string":"RangeError: BigInt value '9223372036854775808' is out of range","isSQLiteError":false}}})~", "stmt-safe-toggle", {
    const bool before = s1.safeIntegers();
    s1.safeIntegers(true);
    const json::value row = enc(s1.get(big("9223372036854775808")));
    s1.safeIntegers(false);
    return list({flag(before), row});
  })
  LINE(R"~(stmt-safe-get	{"ok":[{"$row":[["v",9007199254740992]],"proto":"Object"},true,{"$row":[["v",{"$bigint":"9007199254740993"}]],"proto":"Object"},true]})~", "stmt-safe-get", {
    const Statement st = db->query("SELECT 9007199254740993 AS v");
    const json::value before = enc(st.get());
    const bool self = st.safeIntegers(true) == st;
    const json::value after = enc(st.get());
    const bool enabled = st.safeIntegers();
    st.safeIntegers(false);
    return list({before, flag(self), after, flag(enabled)});
  })
  LINE(R"~(db-safe-default	{"ok":[true,false]})~", "db-safe-default", return list({flag(sdb->query("SELECT 1 AS v").safeIntegers()), flag(db->query("SELECT 2 AS v").safeIntegers())});)
  LINE(R"~(safe-truthy	{"ok":[{"$row":[["v",{"$bigint":"1"}]],"proto":"Object"},true]})~", "safe-truthy", {
    const Statement st = db->prepare("SELECT 1 AS v");
    st.safeIntegers(true);
    return list({enc(st.get()), flag(st.safeIntegers())});
  })
}

// ---- Database.run (scratch o3_bind.ts, o7.ts, o9.ts, o14.ts, o15.ts) ----------------------------

TEST("Database.run: parameters bind the first statement; empty and blank text") {
  const Closing db(Database(":memory:"));
  const Closing strict(Database(":memory:", {.strict = true}));
  db->run("CREATE TABLE t (a, b)");
  LINE(R"~(run-array	{"ok":{"$row":[["changes",1],["lastInsertRowid",1]],"proto":"Object"}})~", "run-array", return enc(db->run("INSERT INTO t VALUES (?, ?)", std::vector<Binding>{1.0, "x"}));)
  LINE(R"~(run-spread	{"ok":{"$row":[["changes",1],["lastInsertRowid",2]],"proto":"Object"}})~", "run-spread", return enc(db->run("INSERT INTO t VALUES (?, ?)", 2.0, "y"));)
  LINE(R"~(run-object	{"ok":{"$row":[["changes",1],["lastInsertRowid",3]],"proto":"Object"}})~", "run-object", return enc(db->run("INSERT INTO t VALUES ($a, $b)", Named{{"$a", 3.0}, {"$b", "z"}}));)
  LINE(R"~(run-too-few	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 2 values, received 1","keys":[],"string":"Error: SQLite query expected 2 values, received 1","isSQLiteError":false}}})~", "run-too-few", return enc(db->run("INSERT INTO t VALUES (?, ?)", std::vector<Binding>{4.0}));)
  LINE(R"~(run-no-params-with-placeholders	{"ok":{"$row":[["changes",1],["lastInsertRowid",4]],"proto":"Object"}})~", "run-no-params-with-placeholders", return enc(db->run("INSERT INTO t VALUES (?, ?)"));)
  LINE(R"~(run-params-no-placeholders	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 0 values, received 2","keys":[],"string":"Error: SQLite query expected 0 values, received 2","isSQLiteError":false}}})~", "run-params-no-placeholders", return enc(db->run("INSERT INTO t VALUES (5, 5)", std::vector<Binding>{1.0, 2.0}));)
  LINE(R"~(run-multi-params	{"ok":{"$row":[["changes",2],["lastInsertRowid",6]],"proto":"Object"}})~", "run-multi-params", return enc(db->run("INSERT INTO t VALUES (?, ?); INSERT INTO t VALUES (?, ?)", std::vector<Binding>{6.0, 6.0}));)
  LINE(R"~(run-multi-params-4	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 2 values, received 4","keys":[],"string":"Error: SQLite query expected 2 values, received 4","isSQLiteError":false}}})~", "run-multi-params-4",
       return enc(db->run("INSERT INTO t VALUES (?, ?); INSERT INTO t VALUES (?, ?)", std::vector<Binding>{7.0, 7.0, 8.0, 8.0}));)
  LINE(R"~(run-u8-first-spread	{"ok":{"$row":[["changes",1],["lastInsertRowid",7]],"proto":"Object"}})~", "run-u8-first-spread", return enc(db->run("INSERT INTO t VALUES (?, ?)", Bytes{1}, 2.0));)
  LINE(R"~(table	{"ok":[{"$row":[["rowid",1],["a",1],["b","x"]],"proto":"Object"},{"$row":[["rowid",2],["a",2],["b","y"]],"proto":"Object"},{"$row":[["rowid",3],["a",3],["b","z"]],"proto":"Object"},{"$row":[["rowid",4],["a",null],["b",null]],"proto":"Object"},{"$row":[["rowid",5],["a",6],["b",6]],"proto":"Object"},{"$row":[["rowid",6],["a",null],["b",null]],"proto":"Object"},{"$row":[["rowid",7],["a",{"$bytes":"01","ctor":"Uint8Array"}],["b",2]],"proto":"Object"}]})~", "table", return enc(db->query("SELECT rowid, a, b FROM t").all());)
  strict->run("CREATE TABLE t (a, b)");
  LINE(R"~(strict-run-object	{"ok":{"$row":[["changes",1],["lastInsertRowid",1]],"proto":"Object"}})~", "strict-run-object", return enc(strict->run("INSERT INTO t VALUES ($a, $b)", Named{{"a", 1.0}, {"b", 2.0}}));)
  LINE(R"~(strict-run-missing	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Missing parameter \"b\"","keys":[],"string":"Error: Missing parameter \"b\"","isSQLiteError":false}}})~", "strict-run-missing", return enc(strict->run("INSERT INTO t VALUES ($a, $b)", Named{{"a", 1.0}}));)
  LINE(R"~(strict-table	{"ok":[{"$row":[["a",1],["b",2]],"proto":"Object"}]})~", "strict-table", return enc(strict->query("SELECT a, b FROM t").all());)
  LINE(R"~(run:empty	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQL string mustn't be blank","keys":[],"string":"Error: SQL string mustn't be blank","isSQLiteError":false}}})~", "run:empty", return enc(db->run(""));)
  LINE(R"~(run:space	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Query contained no valid SQL statement; likely empty query.","keys":[],"string":"Error: Query contained no valid SQL statement; likely empty query.","isSQLiteError":false}}})~", "run:space", return enc(db->run("   "));)
  LINE(R"~(run:comment	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Query contained no valid SQL statement; likely empty query.","keys":[],"string":"Error: Query contained no valid SQL statement; likely empty query.","isSQLiteError":false}}})~", "run:comment", return enc(db->run("-- nothing"));)
  LINE(R"~(run:semicolon	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Query contained no valid SQL statement; likely empty query.","keys":[],"string":"Error: Query contained no valid SQL statement; likely empty query.","isSQLiteError":false}}})~", "run:semicolon", return enc(db->run(";"));)
  LINE(R"~(run-trailing-space	{"ok":{"$row":[["changes",0],["lastInsertRowid",0]],"proto":"Object"}})~", "run-trailing-space", return enc(Database(":memory:").run("SELECT 1;   "));)
  LINE(R"~(run-trailing-comment	{"ok":{"$row":[["changes",0],["lastInsertRowid",0]],"proto":"Object"}})~", "run-trailing-comment", return enc(Database(":memory:").run("SELECT 1; -- end"));)
  LINE(R"~(run-leading-semicolons	{"ok":{"$row":[["changes",0],["lastInsertRowid",0]],"proto":"Object"}})~", "run-leading-semicolons", return enc(Database(":memory:").run(";;SELECT 1"));)
  LINE(R"~(exec-is-run	{"ok":[true,{"$row":[["changes",1],["lastInsertRowid",1]],"proto":"Object"}]})~", "exec-is-run", {
    // `db.exec === db.run` is true in Bun; Database::exec forwards to run.
    const Database other(":memory:");
    return list({flag(true), enc(other.exec("CREATE TABLE x(a); INSERT INTO x VALUES (1)"))});
  })
}

TEST("Database.run: a failing statement throws only when its text ends the SQL") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE u (x UNIQUE)");
  db->run("INSERT INTO u VALUES (1)");
  const auto attempt = [&](const char* sql) -> json::value {
    try {
      return enc(db->run(sql));
    } catch (const rt::JsError& error) {
      return text("THROW " + error.message);
    }
  };
  LINE(R"~(p1	{"ok":"THROW UNIQUE constraint failed: u.x"})~", "p1", return attempt("INSERT INTO u VALUES (1);");)
  LINE(R"~(p2	{"ok":{"$row":[["changes",1],["lastInsertRowid",2]],"proto":"Object"}})~", "p2", return attempt("INSERT INTO u VALUES (1);INSERT INTO u VALUES (90)");)
  LINE(R"~(p3	{"ok":"THROW UNIQUE constraint failed: u.x"})~", "p3", return attempt("INSERT INTO u VALUES (1);INSERT INTO u VALUES (1)");)
  LINE(R"~(p4	{"ok":{"$row":[["changes",1],["lastInsertRowid",3]],"proto":"Object"}})~", "p4", return attempt("INSERT INTO u VALUES (92); INSERT INTO u VALUES (1); ");)
  LINE(R"~(p5	{"ok":"THROW integer overflow"})~", "p5", return attempt("SELECT abs(-9223372036854775808);");)
  LINE(R"~(p6	{"ok":"THROW integer overflow"})~", "p6", return attempt("SELECT abs(-9223372036854775808) -- c");)
  LINE(R"~(p7	{"ok":{"$row":[["changes",0],["lastInsertRowid",3]],"proto":"Object"}})~", "p7", return attempt("SELECT abs(-9223372036854775808); -- c");)
  LINE(R"~(p8	{"ok":"THROW integer overflow"})~", "p8", return attempt("SELECT abs(-9223372036854775808)\n");)
  LINE(R"~(p9	{"ok":"THROW integer overflow"})~", "p9", return attempt("  SELECT abs(-9223372036854775808)");)
  LINE(R"~(p10	{"ok":{"$row":[["changes",0],["lastInsertRowid",3]],"proto":"Object"}})~", "p10", return attempt("SELECT abs(-9223372036854775808);\n");)
  LINE(R"~(rows	{"ok":[1,90,92]})~", "rows", {
    json::array flat;
    for (const Row& row : db->query("SELECT x FROM u ORDER BY x").all()) flat.push_back(enc(*row.get(std::string_view("x"))));
    return flat;
  })
  LINE(R"~(q-two	{"ok":[95]})~", "q-two", {
    db->query("INSERT INTO u VALUES (95); INSERT INTO u VALUES (96)").run();
    json::array flat;
    for (const Row& row : db->query("SELECT x FROM u WHERE x > 94 ORDER BY x").all()) flat.push_back(enc(*row.get(std::string_view("x"))));
    return flat;
  })
}

TEST("Database.run: statements after the first start past whitespace and semicolons") {
  const Closing db(Database(":memory:"));
  const auto offset = [&](const std::string& sql) -> json::value {
    try {
      db->run(sql);
      return text("no error");
    } catch (const SQLiteError& error) {
      return error.byteOffset;
    }
  };
  const std::pair<const char*, std::string> spaces[] = {{"space", " "}, {"tab", "\t"}, {"lf", "\n"}, {"cr", "\r"}, {"ff", "\f"}, {"vt", "\v"}, {"mixed", " \t\n\r "}};
  const std::string expected[] = {R"~(first-space	{"ok":0})~", R"~(second-space	{"ok":0})~", R"~(first-tab	{"ok":0})~", R"~(second-tab	{"ok":0})~", R"~(first-lf	{"ok":0})~", R"~(second-lf	{"ok":0})~", R"~(first-cr	{"ok":0})~",
                                  R"~(second-cr	{"ok":0})~", R"~(first-ff	{"ok":0})~", R"~(second-ff	{"ok":0})~", R"~(first-vt	{"ok":0})~", R"~(second-vt	{"ok":0})~", R"~(first-mixed	{"ok":0})~", R"~(second-mixed	{"ok":0})~"};
  std::size_t next = 0;
  for (const auto& [name, space] : spaces) {
    LINE(expected[next++], std::string("first-") + name, return offset(space + space + "SELEC 1");)
    LINE(expected[next++], std::string("second-") + name, return offset("SELECT 1;" + space + space + "SELEC 1");)
  }
  LINE(R"~(second-semicolons	{"ok":0})~", "second-semicolons", return offset("SELECT 1;;; SELEC 1");)
  LINE(R"~(second-comment-only	{"ok":5})~", "second-comment-only", return offset("SELECT 1; -- c\nSELEC 1");)
  LINE(R"~(third	{"ok":0})~", "third", return offset("SELECT 1; SELECT 2;  SELEC 1");)
  LINE(R"~(first-semicolon-space	{"ok":0})~", "first-semicolon-space", return offset(" ; SELEC 1");)
  LINE(R"~(query-trailing	{"ok":10})~", "query-trailing", {
    try {
      db->query("SELECT 1 +* 2   ");
    } catch (const SQLiteError& error) {
      return error.byteOffset;
    }
    return nullptr;
  })
  LINE(R"~(prepare-utf8-error-offset	{"ok":13})~", "prepare-utf8-error-offset", {
    try {
      db->prepare("SELECT '\xC3\xA9' +* 2");
    } catch (const SQLiteError& error) {
      return error.byteOffset;
    }
    return nullptr;
  })
  LINE(R"~(run-utf8-error-offset	{"ok":13})~", "run-utf8-error-offset", return offset("SELECT '\xC3\xA9' +* 2");)
  LINE(R"~(run-utf8-second-error-offset	{"ok":13})~", "run-utf8-second-error-offset", return offset("SELECT '\xC3\xA9';  SELECT '\xC3\xA9' +* 2");)
  LINE(R"~(run-offset-space	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "run-offset-space", return caught([&] { return enc(db->run("SELECT 1;   SELEC 2")); });)
  LINE(R"~(run-offset-nospace	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "run-offset-nospace", return caught([&] { return enc(db->run("SELECT 1;SELEC 2")); });)
  LINE(R"~(run-offset-mid	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"*\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":10,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"*\": syntax error","isSQLiteError":true}}})~", "run-offset-mid", return caught([&] { return enc(db->run("SELECT 1; SELECT 1 +* 2")); });)
  LINE(R"~(run-offset-utf16	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"*\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":10,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"*\": syntax error","isSQLiteError":true}}})~", "run-offset-utf16", return caught([&] { return enc(db->run("SELECT '\xF0\x9F\x98\x80'; SELECT 1 +* 2")); });)
  LINE(R"~(query-utf16-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"*\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":15,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"*\": syntax error","isSQLiteError":true}}})~", "query-utf16-error", return caught([&] { return enc(db->query("SELECT '\xF0\x9F\x98\x80' +* 1").get()); });)
  LINE(R"~(run-comment-then-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "run-comment-then-error", return caught([&] { return enc(db->run("SELECT 1; -- c\n  SELEC 2")); });)
  LINE(R"~(run-block-comment-then-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":8,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "run-block-comment-then-error", return caught([&] { return enc(db->run("SELECT 1; /* c */ SELEC 2")); });)
  LINE(R"~(query-leading-space-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":3,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "query-leading-space-error", return caught([&] { return enc(db->query("   SELEC 2").get()); });)
  LINE(R"~(query-leading-comment-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":8,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "query-leading-comment-error", return caught([&] { return enc(db->query("/* x */ SELEC 2").get()); });)
  LINE(R"~(run-first-leading-space-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~", "run-first-leading-space-error", return caught([&] { return enc(db->run("   SELEC 2")); });)
}

TEST("Database.run: mid-text failures, multi-statement changes, binding failures") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE u (x UNIQUE)");
  db->run("INSERT INTO u VALUES (1)");
  const auto attempt = [&](const char* sql, const std::vector<Binding>& params) -> json::value { return caught([&] { return enc(db->run(sql, params)); }); };
  LINE(R"~(dup-trailing-comment	{"ok":{"$row":[["changes",0],["lastInsertRowid",1]],"proto":"Object"}})~", "dup-trailing-comment", return attempt("INSERT INTO u VALUES (1); -- c", {});)
  LINE(R"~(dup-trailing-space	{"ok":{"$row":[["changes",0],["lastInsertRowid",1]],"proto":"Object"}})~", "dup-trailing-space", return attempt("INSERT INTO u VALUES (1);   ", {});)
  LINE(R"~(dup-trailing-semis	{"ok":{"$row":[["changes",0],["lastInsertRowid",1]],"proto":"Object"}})~", "dup-trailing-semis", return attempt("INSERT INTO u VALUES (1);;;", {});)
  LINE(R"~(dup-then-comment-then-ok	{"ok":{"$row":[["changes",1],["lastInsertRowid",2]],"proto":"Object"}})~", "dup-then-comment-then-ok", return attempt("INSERT INTO u VALUES (1); -- c\nINSERT INTO u VALUES (50)", {});)
  LINE(R"~(comment-then-dup	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"UNIQUE constraint failed: u.x","code":"SQLITE_CONSTRAINT_UNIQUE","errno":2067,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: UNIQUE constraint failed: u.x","isSQLiteError":true}}})~", "comment-then-dup", return attempt("-- c\nINSERT INTO u VALUES (1)", {});)
  LINE(R"~(only-comments	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"Query contained no valid SQL statement; likely empty query.","keys":[],"string":"Error: Query contained no valid SQL statement; likely empty query.","isSQLiteError":false}}})~", "only-comments", return attempt("-- a\n-- b", {});)
  LINE(R"~(comment-then-params	{"ok":{"$row":[["changes",1],["lastInsertRowid",3]],"proto":"Object"}})~", "comment-then-params", return attempt("-- c\nINSERT INTO u VALUES (?)", {60.0});)
  LINE(R"~(params-second-stmt-placeholder	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"SQLite query expected 0 values, received 1","keys":[],"string":"Error: SQLite query expected 0 values, received 1","isSQLiteError":false}}})~", "params-second-stmt-placeholder", return attempt("INSERT INTO u VALUES (61); INSERT INTO u VALUES (?)", {62.0});)
  LINE(R"~(params-count-first	{"ok":{"$row":[["changes",2],["lastInsertRowid",5]],"proto":"Object"}})~", "params-count-first", return attempt("INSERT INTO u VALUES (?); INSERT INTO u VALUES (?)", {70.0});)
  LINE(R"~(bind-fail-first	{"ok":{"$error":{"ctor":"TypeError","name":"TypeError","message":"Binding expected string, TypedArray, boolean, number, bigint or null","keys":[],"string":"TypeError: Binding expected string, TypedArray, boolean, number, bigint or null","isSQLiteError":false}}})~", "bind-fail-first", return attempt("INSERT INTO u VALUES (?); INSERT INTO u VALUES (71)", {anObject()});)
  LINE(R"~(range-fail-first	{"ok":{"$row":[["changes",2],["lastInsertRowid",7]],"proto":"Object"}})~", "range-fail-first", return attempt("INSERT INTO u VALUES (?); INSERT INTO u VALUES (72)", {big("18446744073709551616")});)
  LINE(R"~(rows	{"ok":[null,0,1,50,60,70,72]})~", "rows", {
    json::array flat;
    for (const Row& row : db->query("SELECT x FROM u ORDER BY x").all()) flat.push_back(enc(*row.get(std::string_view("x"))));
    return flat;
  })
  LINE(R"~(count-mismatch-noexec	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"table u has 1 columns but 2 values were supplied","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: table u has 1 columns but 2 values were supplied","isSQLiteError":true}}})~", "count-mismatch-noexec", return attempt("INSERT INTO u VALUES (?, 1)", {80.0, 81.0});)
  LINE(R"~(busy-like-error-mid	{"ok":{"$row":[["changes",0],["lastInsertRowid",7]],"proto":"Object"}})~", "busy-like-error-mid", return attempt("INSERT INTO u VALUES (1); SELECT 1", {});)
  const Closing sdb(Database(":memory:", {.strict = true, .safeIntegers = true}));
  sdb->run("CREATE TABLE v (x)");
  LINE(R"~(strict-run-named	{"ok":{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"1"}]],"proto":"Object"}})~", "strict-run-named", return enc(sdb->run("INSERT INTO v VALUES ($x)", Named{{"x", std::int64_t{5}}}));)
  LINE(R"~(strict-run-range	{"ok":{"$error":{"ctor":"RangeError","name":"RangeError","message":"BigInt value '9223372036854775808' is out of range","keys":[],"string":"RangeError: BigInt value '9223372036854775808' is out of range","isSQLiteError":false}}})~", "strict-run-range", return caught([&] { return enc(sdb->run("INSERT INTO v VALUES (?)", big("9223372036854775808"))); });)
  LINE(R"~(safe-run-rowid	{"ok":{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"2"}]],"proto":"Object"}})~", "safe-run-rowid", return enc(sdb->run("INSERT INTO v VALUES (1)"));)
  // scratch o7.ts / o8.ts
  const Closing other(Database(":memory:"));
  LINE(R"~(pk-multi	{"ok":["none",[[1],[2]]]})~", "pk-multi", {
    other->run("CREATE TABLE pk (id PRIMARY KEY)");
    json::value error = text("none");
    try {
      other->run("INSERT INTO pk VALUES (1); INSERT INTO pk VALUES (1); INSERT INTO pk VALUES (2)");
    } catch (const rt::JsError& thrown) {
      error = encError(thrown);
    }
    return list({error, values(other->query("SELECT id FROM pk").all())});
  })
  LINE(R"~(run-utf16-sql	{"ok":[{"$row":[["changes",2],["lastInsertRowid",2]],"proto":"Object"},[{"$row":[["v","😀é"],["h","F09F9880C3A9"]],"proto":"Object"},{"$row":[["v","é"],["h","C3A9"]],"proto":"Object"}]]})~", "run-utf16-sql", {
    other->run("CREATE TABLE ut (v)");
    const json::value result = enc(other->run("INSERT INTO ut VALUES ('\xF0\x9F\x98\x80\xC3\xA9'); INSERT INTO ut VALUES ('\xC3\xA9')"));
    return list({result, enc(other->query("SELECT v, hex(v) AS h FROM ut").all())});
  })
}

// ---- run results (scratch o6_more.ts, o7.ts) ----------------------------------------------------

TEST("run: changes counts every row a run changed, lastInsertRowid follows safe integers") {
  const Closing db(Database(":memory:"));
  LINE(R"~(run-create	{"ok":{"$row":[["changes",0],["lastInsertRowid",0]],"proto":"Object"}})~", "run-create", return enc(db->run("CREATE TABLE t (id INTEGER PRIMARY KEY, v)"));)
  LINE(R"~(run-insert	{"ok":{"$row":[["changes",2],["lastInsertRowid",2]],"proto":"Object"}})~", "run-insert", return enc(db->run("INSERT INTO t (v) VALUES ('a'), ('b')"));)
  LINE(R"~(run-create-after-insert	{"ok":{"$row":[["changes",0],["lastInsertRowid",2]],"proto":"Object"}})~", "run-create-after-insert", return enc(db->run("CREATE TABLE u (x)"));)
  LINE(R"~(run-select	{"ok":{"$row":[["changes",0],["lastInsertRowid",2]],"proto":"Object"}})~", "run-select", return enc(db->run("SELECT * FROM t"));)
  LINE(R"~(run-update	{"ok":{"$row":[["changes",2],["lastInsertRowid",2]],"proto":"Object"}})~", "run-update", return enc(db->run("UPDATE t SET v = 'c'"));)
  LINE(R"~(run-delete-none	{"ok":{"$row":[["changes",0],["lastInsertRowid",2]],"proto":"Object"}})~", "run-delete-none", return enc(db->run("DELETE FROM t WHERE id = 99"));)
  LINE(R"~(run-pragma	{"ok":{"$row":[["changes",0],["lastInsertRowid",2]],"proto":"Object"}})~", "run-pragma", return enc(db->run("PRAGMA user_version = 3"));)
  LINE(R"~(stmt-run-insert	{"ok":{"$row":[["changes",1],["lastInsertRowid",3]],"proto":"Object"}})~", "stmt-run-insert", return enc(db->query("INSERT INTO t (v) VALUES (?)").run("z"));)
  LINE(R"~(stmt-run-select	{"ok":{"$row":[["changes",0],["lastInsertRowid",3]],"proto":"Object"}})~", "stmt-run-select", return enc(db->query("SELECT * FROM t").run());)
  LINE(R"~(run-big-rowid	{"ok":[{"$row":[["changes",1],["lastInsertRowid",9223372036854776000]],"proto":"Object"},{"$row":[["changes",1],["lastInsertRowid",1]],"proto":"Object"}]})~", "run-big-rowid", {
    db->run("INSERT INTO t (id, v) VALUES (9007199254740993, 'big')");
    const json::value first = enc(db->run("INSERT INTO t (id, v) VALUES (9223372036854775807, 'max')"));
    return list({first, enc(db->query("INSERT INTO u VALUES (1)").run())});
  })
  const Closing sdb(Database(":memory:", {.safeIntegers = true}));
  sdb->run("CREATE TABLE t (id INTEGER PRIMARY KEY, v)");
  LINE(R"~(safe-run-insert	{"ok":{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"1"}]],"proto":"Object"}})~", "safe-run-insert", return enc(sdb->run("INSERT INTO t (v) VALUES ('a')"));)
  LINE(R"~(safe-stmt-run	{"ok":{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"2"}]],"proto":"Object"}})~", "safe-stmt-run", return enc(sdb->query("INSERT INTO t (v) VALUES (?)").run("b"));)
  LINE(R"~(safe-run-big	{"ok":{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"9223372036854775807"}]],"proto":"Object"}})~", "safe-run-big", return enc(sdb->run("INSERT INTO t (id, v) VALUES (9223372036854775807, 'max')"));)
  // Bun answers lastInsertRowid 3491350487241861600 here (a bigint read back as a number);
  // the binding returns the rowid.
  {
    const Statement s = sdb->prepare("INSERT INTO t (v) VALUES ('c')");
    s.safeIntegers(false);
    const Changes changes = s.run();
    EXPECT_EQ(changes.changes, 1.0);
    EXPECT(std::holds_alternative<double>(changes.lastInsertRowid));
  }
  LINE(R"~(safe-stmt-on-unsafe-db	{"ok":{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"2"}]],"proto":"Object"}})~", "safe-stmt-on-unsafe-db", {
    const Statement s = db->prepare("INSERT INTO u VALUES ('c')");
    s.safeIntegers(true);
    return enc(s.run());
  })
  LINE(R"~(changes-trigger	{"ok":{"$row":[["changes",3],["lastInsertRowid",3]],"proto":"Object"}})~", "changes-trigger", {
    db->run("CREATE TABLE log (x)");
    db->run("CREATE TRIGGER tr AFTER INSERT ON u BEGIN INSERT INTO log VALUES (1); INSERT INTO log VALUES (2); END");
    return enc(db->run("INSERT INTO u VALUES (5)"));
  })
  LINE(R"~(run-multi-last-changes	{"ok":{"$row":[["changes",9],["lastInsertRowid",6]],"proto":"Object"}})~", "run-multi-last-changes", return enc(db->run("INSERT INTO u VALUES (1); INSERT INTO u VALUES (2), (3); SELECT 1"));)
  LINE(R"~(run-multi-error-second	{"ok":[{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}},{"$row":[["n",2]],"proto":"Object"}]})~", "run-multi-error-second", {
    try {
      db->run("INSERT INTO u VALUES (100); SELEC; INSERT INTO u VALUES (101)");
    } catch (const rt::JsError& error) {
      return list({encError(error), enc(db->query("SELECT count(*) AS n FROM u WHERE x >= 100").get())});
    }
    return nullptr;
  })
  LINE(R"~(run-multi-error-runtime	{"ok":{"$undefined":true}})~", "run-multi-error-runtime", {
    db->run("CREATE TABLE pk (id PRIMARY KEY)");
    try {
      db->run("INSERT INTO pk VALUES (1); INSERT INTO pk VALUES (1); INSERT INTO pk VALUES (2)");
    } catch (const rt::JsError& error) {
      return list({encError(error), values(db->query("SELECT id FROM pk").all())});
    }
    return undefinedValue();
  })
  LINE(R"~(changes-stmt-trigger	{"ok":[{"$row":[["changes",3],["lastInsertRowid",1]],"proto":"Object"},{"$row":[["changes",3],["lastInsertRowid",2]],"proto":"Object"},{"$row":[["changes",0],["lastInsertRowid",2]],"proto":"Object"}]})~", "changes-stmt-trigger", {
    const Database d(":memory:");
    d.run("CREATE TABLE u (x)");
    d.run("CREATE TABLE log (x)");
    d.run("CREATE TRIGGER tr AFTER INSERT ON u BEGIN INSERT INTO log VALUES (1); INSERT INTO log VALUES (2); END");
    return list({enc(d.query("INSERT INTO u VALUES (?)").run(1.0)), enc(d.prepare("INSERT INTO u VALUES (2)").run()), enc(d.query("SELECT * FROM u").run())});
  })
  LINE(R"~(changes-after-other-conn	{"ok":{"$row":[["changes",0],["lastInsertRowid",1]],"proto":"Object"}})~", "changes-after-other-conn", {
    const Database d(":memory:");
    d.run("CREATE TABLE u (x)");
    d.run("INSERT INTO u VALUES (1)");
    return enc(d.query("UPDATE u SET x = 2 WHERE x = 99").run());
  })
  LINE(R"~(changes-failed-insert	{"ok":{"$row":[["changes",0],["lastInsertRowid",2]],"proto":"Object"}})~", "changes-failed-insert", {
    const Database d(":memory:");
    d.run("CREATE TABLE u (x UNIQUE)");
    d.run("INSERT INTO u VALUES (1)");
    try {
      d.run("INSERT INTO u VALUES (2), (1)");
    } catch (const rt::JsError&) {
    }
    return enc(d.run("SELECT 1"));
  })
}

// ---- stepping: get/all/run/iterate (scratch o6_more.ts, o11.ts, o12.ts, o13.ts) -------------------

TEST("stepping: get, all and run reset first; get and run step through every row") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE it (n)");
  db->run("INSERT INTO it VALUES (1), (2), (3)");
  const auto bad = [&](int row) { return db->prepare("SELECT CASE WHEN n = " + std::to_string(row) + " THEN abs(-9223372036854775808) ELSE n END AS v FROM it"); };
  const auto said = [](const std::function<json::value()>& body) -> json::value {
    try {
      return body();
    } catch (const rt::JsError& error) {
      return text(error.toString());
    }
  };
  LINE(R"~(plain-order	{"ok":[{"$row":[["v",1]],"proto":"Object"},{"$row":[["v",2]],"proto":"Object"},{"$row":[["v",3]],"proto":"Object"}]})~", "plain-order", return enc(bad(99).all());)
  LINE(R"~(get-error-row2	{"ok":"SQLiteError: integer overflow"})~", "get-error-row2", return said([&] { return enc(bad(2).get()); });)
  LINE(R"~(get-error-row3	{"ok":"SQLiteError: integer overflow"})~", "get-error-row3", return said([&] { return enc(bad(3).get()); });)
  LINE(R"~(get-error-row1	{"ok":"SQLiteError: integer overflow"})~", "get-error-row1", return said([&] { return enc(bad(1).get()); });)
  LINE(R"~(all-error-row3	{"ok":"SQLiteError: integer overflow"})~", "all-error-row3", return said([&] { return enc(bad(3).all()); });)
  LINE(R"~(run-error-row3	{"ok":"SQLiteError: integer overflow"})~", "run-error-row3", return said([&] { return enc(bad(3).run()); });)
  LINE(R"~(dbrun-error-row3	{"ok":"SQLiteError: integer overflow"})~", "dbrun-error-row3", return said([&] { return enc(db->run("SELECT CASE WHEN n = 3 THEN abs(-9223372036854775808) ELSE n END AS v FROM it")); });)
  LINE(R"~(iterate-error-steps	{"ok":[{"$row":[["v",1]],"proto":"Object"},"E1 SQLiteError: integer overflow",{"$row":[["v",1]],"proto":"Object"},"E SQLiteError: integer overflow",{"$row":[["v",1]],"proto":"Object"},"E SQLiteError: integer overflow"]})~", "iterate-error-steps", {
    const Statement s = bad(2);
    json::array result;
    sqlite::RowIterator first = s.iterate();
    result.push_back(enc(first.next()));
    try {
      first.next();
      result.push_back("no");
    } catch (const rt::JsError& error) {
      result.push_back(text("E1 " + error.toString()));
    }
    for (int round = 0; round < 4; ++round) {
      try {
        sqlite::RowIterator again = s.iterate();
        const std::optional<Row> row = again.next();
        result.push_back(row ? enc(*row) : text("done"));
      } catch (const rt::JsError& error) {
        result.push_back(text("E " + error.toString()));
      }
    }
    return result;
  })
  LINE(R"~(iterate-error-then-get	{"ok":[{"$row":[["v",1]],"proto":"Object"},{"$row":[["v",2]],"proto":"Object"},"SQLiteError: integer overflow","G SQLiteError: integer overflow"]})~", "iterate-error-then-get", {
    const Statement s = bad(3);
    sqlite::RowIterator iterator = s.iterate();
    json::array result{enc(iterator.next()), enc(iterator.next())};
    try {
      iterator.next();
    } catch (const rt::JsError& error) {
      result.push_back(text(error.toString()));
    }
    try {
      result.push_back(enc(s.get()));
    } catch (const rt::JsError& error) {
      result.push_back(text("G " + error.toString()));
    }
    return result;
  })
  LINE(R"~(iterate-error-then-all	{"ok":"A SQLiteError: integer overflow"})~", "iterate-error-then-all", {
    const Statement s = bad(3);
    sqlite::RowIterator iterator = s.iterate();
    iterator.next();
    iterator.next();
    try {
      iterator.next();
    } catch (const rt::JsError&) {
    }
    return said([&] { return enc(s.all()); }).is_string() ? text("A " + std::string(said([&] { return enc(s.all()); }).as_string())) : enc(s.all());
  })
  LINE(R"~(get-partial-state	{"ok":[1,2,3]})~", "get-partial-state", {
    const Statement s = db->prepare("SELECT n FROM it");
    s.get();
    json::array numbers;
    for (Row& row : s.iterate()) numbers.push_back(enc(*row.get(std::string_view("n"))));
    return numbers;
  })
  LINE(R"~(run-then-iterate	{"ok":[1,2,3]})~", "run-then-iterate", {
    const Statement s = db->prepare("SELECT n FROM it ORDER BY n");
    s.run();
    json::array numbers;
    for (Row& row : s.iterate()) numbers.push_back(enc(*row.get(std::string_view("n"))));
    return numbers;
  })
  LINE(R"~(iterate1-then-run-then-iterate	{"ok":[1,2,3]})~", "iterate1-then-run-then-iterate", {
    const Statement s = db->prepare("SELECT n FROM it ORDER BY n");
    sqlite::RowIterator first = s.iterate();
    first.next();
    s.run();
    json::array numbers;
    for (Row& row : s.iterate()) numbers.push_back(enc(*row.get(std::string_view("n"))));
    return numbers;
  })
  LINE(R"~(run-insert-select	{"ok":[{"$row":[["changes",3],["lastInsertRowid",3]],"proto":"Object"},{"$row":[["c",3]],"proto":"Object"}]})~", "run-insert-select", {
    db->run("CREATE TABLE cp (n)");
    const json::value result = enc(db->prepare("INSERT INTO cp SELECT n FROM it").run());
    return list({result, enc(db->query("SELECT count(*) AS c FROM cp").get())});
  })
  LINE(R"~(all-twice-bindings	{"ok":[2,2]})~", "all-twice-bindings", {
    const Statement s = db->prepare("SELECT n FROM it WHERE n > ? ORDER BY n");
    const std::size_t first = s.all(1.0).size();
    const std::size_t second = s.all().size();
    return list({integer(static_cast<long long>(first)), integer(static_cast<long long>(second))});
  })
}

TEST("stepping: row visitor keeps order and resets after a conversion failure") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE scan (n)");
  db->run("INSERT INTO scan VALUES (3), (1), (2)");
  const Statement statement = db->query("SELECT n FROM scan WHERE n >= ? ORDER BY n");
  const std::vector<sqlite::Binding> all{sqlite::Binding(1.0)};
  const std::vector<sqlite::Binding> tail{sqlite::Binding(2.0)};
  const auto failing = [](Row) { throw rt::TypeError("fixture row conversion failed"); };
  EXPECT_THROWS_MESSAGE(statement.forEachRow(all, failing), rt::TypeError, "fixture row conversion failed");

  std::vector<double> seen;
  statement.forEachRow(tail, [&seen](const Row& row) {
    const auto* value = sqlite::getNumber(row.get("n"));
    if (!value) throw rt::TypeError("fixture row is missing its number");
    seen.push_back(*value);
  });
  EXPECT_EQ(seen, (std::vector<double>{2.0, 3.0}));
}

TEST("stepping: iterate continues where the statement is, resets when done, binds lazily") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE it (n)");
  db->run("INSERT INTO it VALUES (1), (2), (3)");
  const Statement st = db->query("SELECT n FROM it ORDER BY n");
  LINE(R"~(iterate-partial-then-get	{"ok":[{"$row":[["n",1]],"proto":"Object"},{"$row":[["n",1]],"proto":"Object"},{"$row":[["n",1]],"proto":"Object"},false]})~", "iterate-partial-then-get", {
    sqlite::RowIterator g = st.iterate();
    const json::value a = enc(g.next());
    const json::value b = enc(st.get());
    const std::optional<Row> c = g.next();
    return list({a, b, c ? enc(*c) : json::value(nullptr), flag(!c)});
  })
  LINE(R"~(iterate-partial-then-iterate	{"ok":[{"$row":[["n",3]],"proto":"Object"},{"$undefined":true},{"$row":[["n",1]],"proto":"Object"}]})~", "iterate-partial-then-iterate", {
    sqlite::RowIterator g1 = st.iterate();
    g1.next();
    sqlite::RowIterator g2 = st.iterate();
    const json::value first = enc(g2.next());
    const std::optional<Row> fromG1 = g1.next();
    const json::value second = enc(g2.next());
    return list({first, fromG1 ? enc(*fromG1) : undefinedValue(), second});
  })
  LINE(R"~(iterate-break-then-all	{"ok":[{"$row":[["n",1]],"proto":"Object"},{"$row":[["n",2]],"proto":"Object"},{"$row":[["n",3]],"proto":"Object"}]})~", "iterate-break-then-all", {
    for (Row& row : st.iterate()) {
      (void)row;
      break;
    }
    return enc(st.all());
  })
  LINE(R"~(iterate-lazy	{"ok":4})~", "iterate-lazy", {
    const Statement s = db->prepare("SELECT n FROM it ORDER BY n");
    sqlite::RowIterator g = s.iterate();
    db->run("INSERT INTO it VALUES (4)");
    long long count = 0;
    for (Row& row : g) {
      (void)row;
      ++count;
    }
    return integer(count);
  })
  LINE(R"~(all-after-partial-iterate	{"ok":4})~", "all-after-partial-iterate", {
    sqlite::RowIterator g = st.iterate();
    g.next();
    return integer(static_cast<long long>(st.all().size()));
  })
  LINE(R"~(get-twice	{"ok":[{"$row":[["n",1]],"proto":"Object"},{"$row":[["n",1]],"proto":"Object"}]})~", "get-twice", return list({enc(st.get()), enc(st.get())});)
}

TEST("stepping: get, all and run leave the statement reset; iterate rebinding needs a reset statement") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE it (n)");
  db->run("INSERT INTO it VALUES (1), (2), (3)");
  LINE(R"~(partial-then-get-arg	{"ok":{"$row":[["n",1],["p",5]],"proto":"Object"}})~", "partial-then-get-arg", {
    const Statement s = db->prepare("SELECT n, ? AS p FROM it ORDER BY n");
    sqlite::RowIterator g = s.iterate(0.0);
    g.next();
    return caught([&] { return enc(s.get(5.0)); });
  })
  LINE(R"~(partial-then-all-arg	{"ok":3})~", "partial-then-all-arg", {
    const Statement s = db->prepare("SELECT n, ? AS p FROM it ORDER BY n");
    sqlite::RowIterator g = s.iterate(0.0);
    g.next();
    return caught([&] { return integer(static_cast<long long>(s.all(5.0).size())); });
  })
  LINE(R"~(partial-noargs-then-iterate-arg	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"bad parameter or other API misuse","keys":[],"string":"Error: bad parameter or other API misuse","isSQLiteError":false}}})~", "partial-noargs-then-iterate-arg", {
    const Statement s = db->prepare("SELECT n, ? AS p FROM it ORDER BY n");
    s.get(0.0);
    sqlite::RowIterator g = s.iterate();
    g.next();
    return caught([&] {
      sqlite::RowIterator g2 = s.iterate(7.0);
      return enc(g2.next());
    });
  })
  LINE(R"~(done-then-iterate-arg	{"ok":[3,{"$row":[["n",1],["p",7]],"proto":"Object"}]})~", "done-then-iterate-arg", {
    const Statement s = db->prepare("SELECT n, ? AS p FROM it ORDER BY n");
    long long count = 0;
    for (Row& row : s.iterate(1.0)) {
      (void)row;
      ++count;
    }
    sqlite::RowIterator g2 = s.iterate(7.0);
    return list({integer(count), enc(g2.next())});
  })
  LINE(R"~(iterate-arg-mid-misuse-state	{"ok":[{"$row":[["n",2],["p",1]],"proto":"Object"},{"$row":[["n",1],["p",null]],"proto":"Object"}]})~", "iterate-arg-mid-misuse-state", {
    const Statement s = db->prepare("SELECT n, ? AS p FROM it ORDER BY n");
    sqlite::RowIterator g = s.iterate(1.0);
    g.next();
    try {
      s.iterate(7.0).next();
    } catch (const rt::JsError&) {
    }
    return list({enc(g.next()), enc(s.get())});
  })
  LINE(R"~(bind-misuse-object	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"bad parameter or other API misuse","keys":[],"string":"Error: bad parameter or other API misuse","isSQLiteError":false}}})~", "bind-misuse-object", {
    const Statement s = db->prepare("SELECT n, $p AS p FROM it ORDER BY n");
    sqlite::RowIterator g = s.iterate(Named{{"$p", 1.0}});
    g.next();
    return caught([&] {
      s.iterate(Named{{"$p", 7.0}}).next();
      return text("no");
    });
  })
  const auto after = [&](const std::function<void(const Statement&)>& first) -> json::value {
    const Statement s = db->prepare("SELECT n, ? AS p FROM it ORDER BY n");
    try {
      first(s);
    } catch (const rt::JsError&) {
    }
    return caught([&] {
      sqlite::RowIterator g = s.iterate(7.0);
      return enc(g.next());
    });
  };
  LINE(R"~(after-get	{"ok":{"$row":[["n",1],["p",7]],"proto":"Object"}})~", "after-get", return after([](const Statement& s) { s.get(1.0); });)
  LINE(R"~(after-get-noargs	{"ok":{"$row":[["n",1],["p",7]],"proto":"Object"}})~", "after-get-noargs", return after([](const Statement& s) { s.get(); });)
  LINE(R"~(after-all	{"ok":{"$row":[["n",1],["p",7]],"proto":"Object"}})~", "after-all", return after([](const Statement& s) { s.all(1.0); });)
  LINE(R"~(after-run	{"ok":{"$row":[["n",1],["p",7]],"proto":"Object"}})~", "after-run", return after([](const Statement& s) { s.run(1.0); });)
  const Statement bad = db->prepare("SELECT CASE WHEN n = 2 THEN abs(-9223372036854775808) ELSE n END AS v, ? AS p FROM it");
  const auto afterBad = [&](const std::function<void()>& first) -> json::value {
    try {
      first();
    } catch (const rt::JsError&) {
    }
    return caught([&] {
      sqlite::RowIterator g = bad.iterate(7.0);
      return enc(g.next());
    });
  };
  LINE(R"~(after-get-step-error	{"ok":{"$row":[["v",1],["p",7]],"proto":"Object"}})~", "after-get-step-error", return afterBad([&] { bad.get(1.0); });)
  LINE(R"~(after-all-step-error	{"ok":{"$row":[["v",1],["p",7]],"proto":"Object"}})~", "after-all-step-error", return afterBad([&] { bad.all(1.0); });)
  LINE(R"~(after-run-step-error	{"ok":{"$row":[["v",1],["p",7]],"proto":"Object"}})~", "after-run-step-error", return afterBad([&] { bad.run(1.0); });)
  LINE(R"~(after-iterate-step-error	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"bad parameter or other API misuse","keys":[],"string":"Error: bad parameter or other API misuse","isSQLiteError":false}}})~", "after-iterate-step-error", return afterBad([&] {
         sqlite::RowIterator g = bad.iterate(1.0);
         g.next();
         g.next();
       });)
  LINE(R"~(get-then-iterate-noargs-position	{"ok":[{"$row":[["n",1]],"proto":"Object"},{"$row":[["n",2]],"proto":"Object"}]})~", "get-then-iterate-noargs-position", {
    const Statement s = db->prepare("SELECT n FROM it ORDER BY n");
    s.get();
    sqlite::RowIterator g = s.iterate();
    const json::value first = enc(g.next());
    return list({first, enc(g.next())});
  })
  LINE(R"~(iterate-args-lazy	{"ok":[{"$row":[["v",5]],"proto":"Object"}]})~", "iterate-args-lazy", {
    const Statement s = db->prepare("SELECT ? AS v");
    sqlite::RowIterator g = s.iterate(5.0);
    s.get(6.0);
    std::vector<Row> rows;
    for (Row& row : g) rows.push_back(row);
    return enc(rows);
  })
}

// ---- transactions (scratch o4_tx.ts) ------------------------------------------------------------

namespace {
  Database fresh() {
    Database db(":memory:");
    db.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v)");
    return db;
  }
  json::value rows(const Database& db) { return values(db.query("SELECT id, v FROM t ORDER BY id").all()); }
  json::value rowOf(std::initializer_list<std::pair<const char*, json::value>> members) {
    json::array entries;
    for (const auto& [key, value] : members) entries.push_back(json::array{json::value(key), value});
    json::object tagged;
    tagged["$row"] = std::move(entries);
    tagged["proto"] = "Object";
    return tagged;
  }
  struct Boom : rt::JsError {
    Boom() : rt::JsError("boom") {}
  };
}

TEST("transactions: commit, rollback, savepoints, manual BEGIN, failing COMMIT") {
  LINE(R"~(tx-commit	{"ok":[{"$row":[["a",1],["b","x"],["inTx",true]],"proto":"Object"},[[1,"x"]],false]})~", "tx-commit", {
    const Database db = fresh();
    const auto tx = db.transaction([&](double a, std::string b) {
      db.run("INSERT INTO t VALUES (?, ?)", a, b);
      return rowOf({{"a", number(a)}, {"b", text(b)}, {"inTx", flag(db.inTransaction())}});
    });
    const json::value result = tx(1.0, std::string("x"));
    return list({result, rows(db), flag(db.inTransaction())});
  })
  LINE(R"~(tx-rollback	{"ok":[true,[],false]})~", "tx-rollback", {
    const Database db = fresh();
    const auto tx = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'a')");
      throw Boom();
    });
    bool same = false;
    try {
      tx();
    } catch (const Boom&) {
      same = true;
    }
    return list({flag(same), rows(db), flag(db.inTransaction())});
  })
  LINE(R"~(tx-nested-commit	{"ok":[true,[[1,"outer"],[2,"inner"]]]})~", "tx-nested-commit", {
    const Database db = fresh();
    const auto inner = db.transaction([&] {
      db.run("INSERT INTO t VALUES (2, 'inner')");
      return db.inTransaction();
    });
    const auto outer = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'outer')");
      return inner();
    });
    const bool result = outer();
    return list({flag(result), rows(db)});
  })
  LINE(R"~(tx-nested-inner-throws-caught	{"ok":["Error: inner inTx=true",[[1,"outer"]]]})~", "tx-nested-inner-throws-caught", {
    const Database db = fresh();
    const auto inner = db.transaction([&] {
      db.run("INSERT INTO t VALUES (2, 'inner')");
      throw rt::JsError("inner");
    });
    const auto outer = db.transaction([&]() -> std::string {
      db.run("INSERT INTO t VALUES (1, 'outer')");
      try {
        inner();
      } catch (const rt::JsError& error) {
        return error.toString() + " inTx=" + (db.inTransaction() ? "true" : "false");
      }
      return "";
    });
    const std::string result = outer();
    return list({text(result), rows(db)});
  })
  LINE(R"~(tx-nested-inner-throws-uncaught	{"ok":["Error: inner",[],false]})~", "tx-nested-inner-throws-uncaught", {
    const Database db = fresh();
    const auto inner = db.transaction([&] {
      db.run("INSERT INTO t VALUES (2, 'inner')");
      throw rt::JsError("inner");
    });
    const auto outer = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'outer')");
      inner();
    });
    std::string message;
    try {
      outer();
    } catch (const rt::JsError& error) {
      message = error.toString();
    }
    return list({text(message), rows(db), flag(db.inTransaction())});
  })
  LINE(R"~(tx-three-levels	{"ok":[[1,"l1"],[2,"l2"],[4,"l2b"]]})~", "tx-three-levels", {
    const Database db = fresh();
    const auto l3 = db.transaction([&] {
      db.run("INSERT INTO t VALUES (3, 'l3')");
      throw rt::JsError("l3");
    });
    const auto l2 = db.transaction([&] {
      db.run("INSERT INTO t VALUES (2, 'l2')");
      try {
        l3();
      } catch (const rt::JsError&) {
      }
      db.run("INSERT INTO t VALUES (4, 'l2b')");
    });
    const auto l1 = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'l1')");
      l2();
    });
    l1();
    return rows(db);
  })
  LINE(R"~(tx-same-fn-nested	{"ok":[[1,1],[2,2],[3,3]]})~", "tx-same-fn-nested", {
    const Database db = fresh();
    int depth = 0;
    std::function<void(double)> body;
    const auto tx = db.transaction([&](double n) { body(n); });
    body = [&](double n) {
      ++depth;
      db.run("INSERT INTO t VALUES (?, ?)", n, depth);
      if (n < 3) tx(n + 1);
    };
    tx(1.0);
    return rows(db);
  })
  LINE(R"~(tx-constraint	{"ok":["SQLITE_CONSTRAINT_PRIMARYKEY","UNIQUE constraint failed: t.id",[[1,"a"]],false]})~", "tx-constraint", {
    const Database db = fresh();
    db.run("INSERT INTO t VALUES (1, 'a')");
    const auto tx = db.transaction([&] {
      db.run("INSERT INTO t VALUES (2, 'b')");
      db.run("INSERT INTO t VALUES (1, 'dup')");
    });
    json::value code;
    json::value message;
    try {
      tx();
    } catch (const SQLiteError& error) {
      code = text(error.code.value_or(""));
      message = text(error.message);
    }
    return list({code, message, rows(db), flag(db.inTransaction())});
  })
  LINE(R"~(tx-manual-begin-outer	{"ok":[[true,[]],[[2,"b"]]]})~", "tx-manual-begin-outer", {
    const Database db = fresh();
    db.run("BEGIN");
    const auto tx = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'a')");
      throw rt::JsError("x");
    });
    try {
      tx();
    } catch (const rt::JsError&) {
    }
    const json::value middle = list({flag(db.inTransaction()), rows(db)});
    db.run("INSERT INTO t VALUES (2, 'b')");
    db.run("COMMIT");
    return list({middle, rows(db)});
  })
  LINE(R"~(tx-fn-commits-itself	{"ok":["SQLiteError: cannot commit - no transaction is active",[[1,"a"]],false]})~", "tx-fn-commits-itself", {
    const Database db = fresh();
    const auto tx = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'a')");
      db.run("COMMIT");
      return 5;
    });
    json::value result;
    try {
      result = tx();
    } catch (const rt::JsError& error) {
      result = text(error.toString());
    }
    return list({result, rows(db), flag(db.inTransaction())});
  })
  LINE(R"~(tx-fn-rollbacks-itself-then-throws	{"ok":["Error: after rollback",[],false]})~", "tx-fn-rollbacks-itself-then-throws", {
    const Database db = fresh();
    const auto tx = db.transaction([&] {
      db.run("INSERT INTO t VALUES (1, 'a')");
      db.run("ROLLBACK");
      throw rt::JsError("after rollback");
    });
    json::value result;
    try {
      tx();
    } catch (const rt::JsError& error) {
      result = text(error.toString());
    }
    return list({result, rows(db), flag(db.inTransaction())});
  })
  {
    // tx-variants-props: `tx.database === db`, and each variant returns fn's result.
    const Database db = fresh();
    const auto tx = db.transaction([] { return 1; });
    EXPECT(tx.database() == db);
    EXPECT_EQ(tx.deferred(), 1);
    EXPECT_EQ(tx.immediate(), 1);
    EXPECT_EQ(tx.exclusive(), 1);
  }
  LINE(R"~(commit-fails-fk	{"ok":[{"$error":{"ctor":"Error","name":"SQLiteError","message":"FOREIGN KEY constraint failed","code":"SQLITE_CONSTRAINT_FOREIGNKEY","errno":787,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: FOREIGN KEY constraint failed","isSQLiteError":true}},false,{"$row":[["n",0]],"proto":"Object"}]})~", "commit-fails-fk", {
    const Database db(":memory:");
    db.run("PRAGMA foreign_keys=ON");
    db.run("CREATE TABLE p (id INTEGER PRIMARY KEY)");
    db.run("CREATE TABLE c (pid REFERENCES p(id) DEFERRABLE INITIALLY DEFERRED)");
    const auto tx = db.transaction([&] {
      db.run("INSERT INTO c VALUES (5)");
      return std::string("fn-done");
    });
    json::value result;
    try {
      result = text(tx());
    } catch (const rt::JsError& error) {
      result = encError(error);
    }
    return list({result, flag(db.inTransaction()), enc(db.query("SELECT count(*) AS n FROM c").get())});
  })
  LINE(R"~(transaction-on-closed	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Cannot use a closed database","keys":[],"string":"RangeError: Cannot use a closed database","isSQLiteError":false}}})~", "transaction-on-closed", {
    const Database db = fresh();
    db.close();
    db.transaction([] { return 1; });
    return nullptr;
  })
  LINE(R"~(transaction-created-then-closed	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "transaction-created-then-closed", {
    const Database db = fresh();
    const auto tx = db.transaction([] { return 1; });
    db.close();
    return tx();
  })
  LINE(R"~(inTransaction-closed	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "inTransaction-closed", {
    const Database db = fresh();
    db.close();
    return flag(db.inTransaction());
  })
  LINE(R"~(tx-statements-after-close	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "tx-statements-after-close", {
    const Database db(":memory:");
    const auto tx = db.transaction([] { return 1; });
    tx();
    db.close();
    return caught([&] { return json::value(tx()); });
  })
}

TEST("transactions: deferred, immediate and exclusive take Bun's locks; busy errors") {
  const TempDir dir;
  const std::string file = dir.path("locks.db");
  {
    Database db(file);
    db.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v)");
    db.close();
  }
  const auto probe = [&](sqlite::detail::TransactionKind kind) -> json::value {
    const Closing a{Database(file)};
    const Closing b{Database(file)};
    const auto tx = a->transaction([&] {
      json::array result;
      try {
        b->query("SELECT count(*) AS n FROM t").get();
        result.push_back("read-ok");
      } catch (const SQLiteError& error) {
        result.push_back(text("read:" + error.code.value_or("")));
      }
      try {
        b->run("INSERT INTO t (v) VALUES ('b')");
        result.push_back("write-ok");
      } catch (const SQLiteError& error) {
        result.push_back(text("write:" + error.code.value_or("") + ":" + error.message));
      }
      return json::value(result);
    });
    switch (kind) {
      case sqlite::detail::TransactionKind::deferred: return tx.deferred();
      case sqlite::detail::TransactionKind::immediate: return tx.immediate();
      case sqlite::detail::TransactionKind::exclusive: return tx.exclusive();
      default: return tx();
    }
  };
  LINE(R"~(lock-default	{"ok":["read-ok","write-ok"]})~", "lock-default", return probe(sqlite::detail::TransactionKind::default_);)
  LINE(R"~(lock-deferred	{"ok":["read-ok","write-ok"]})~", "lock-deferred", return probe(sqlite::detail::TransactionKind::deferred);)
  LINE(R"~(lock-immediate	{"ok":["read-ok","write:SQLITE_BUSY:database is locked"]})~", "lock-immediate", return probe(sqlite::detail::TransactionKind::immediate);)
  LINE(R"~(lock-exclusive	{"ok":["read:SQLITE_BUSY","write:SQLITE_BUSY:database is locked"]})~", "lock-exclusive", return probe(sqlite::detail::TransactionKind::exclusive);)
  LINE(R"~(busy-error	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"database is locked","code":"SQLITE_BUSY","errno":5,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: database is locked","isSQLiteError":true}}})~", "busy-error", {
    const Database a(file);
    const Database b(file);
    a.run("BEGIN IMMEDIATE");
    const json::value result = caught([&] {
      b.run("BEGIN IMMEDIATE");
      return text("no");
    });
    a.run("ROLLBACK");
    a.close();
    b.close();
    return result;
  })
  LINE(R"~(busy-error-query	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"database is locked","code":"SQLITE_BUSY","errno":5,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: database is locked","isSQLiteError":true}}})~", "busy-error-query", {
    const Database a(file);
    const Database b(file);
    a.run("BEGIN EXCLUSIVE");
    const json::value result = caught([&] { return enc(b.query("SELECT 1 FROM t").all()); });
    a.run("ROLLBACK");
    a.close();
    b.close();
    return result;
  })
  LINE(R"~(commit-fails-busy	{"ok":[["SQLITE_BUSY","database is locked"],false]})~", "commit-fails-busy", {
    const Database a(file);
    const Database b(file);
    sqlite::RowIterator reading = b.query("SELECT * FROM t").iterate();
    a.run("INSERT INTO t (v) VALUES ('seed')");
    reading.next();
    json::value error = text("committed");
    const auto tx = a.transaction([&] { a.run("INSERT INTO t (v) VALUES ('x')"); });
    try {
      tx();
    } catch (const SQLiteError& thrown) {
      error = list({text(thrown.code.value_or("")), text(thrown.message)});
    }
    const bool inside = a.inTransaction();
    a.close();
    b.close();
    return list({error, flag(inside)});
  })
}

// ---- statement lifetime and the query cache (scratch o5_life.ts, o6_more.ts, o7.ts, o8.ts) -------

TEST("lifetime: close(false) keeps prepare() statements, close(true) finalizes all") {
  LINE(R"~(close-twice	{"ok":"ok"})~", "close-twice", {
    const Database db(":memory:");
    db.close();
    db.close();
    db.close(true);
    return text("ok");
  })
  LINE(R"~(close-returns	{"ok":{"$undefined":true}})~", "close-returns", {
    const Database db(":memory:");
    db.close();
    return undefinedValue();
  })
  const auto finalizedAfter = [](const char* how, bool throwOnError) -> json::value {
    const Database db(":memory:");
    const Statement s = std::string(how) == "query" ? db.query("SELECT 1 AS v UNION ALL SELECT 2") : db.prepare("SELECT 1 AS v UNION ALL SELECT 2");
    sqlite::RowIterator iterator = s.iterate();
    if (std::string(how) == "iterate") iterator.next();
    else s.get();
    db.close(throwOnError);
    return flag(s.isFinalized());
  };
  LINE(R"~(close-true-with-prepared	{"ok":true})~", "close-true-with-prepared", return finalizedAfter("prepare", true);)
  LINE(R"~(close-true-with-query	{"ok":true})~", "close-true-with-query", return finalizedAfter("query", true);)
  LINE(R"~(close-true-mid-iterate	{"ok":true})~", "close-true-mid-iterate", return finalizedAfter("iterate", true);)
  LINE(R"~(close-false-mid-iterate	{"ok":false})~", "close-false-mid-iterate", return finalizedAfter("iterate", false);)
  LINE(R"~(stmt-get-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "stmt-get-after-close", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    return enc(s.get());
  })
  LINE(R"~(stmt-all-after-close	{"ok":[{"$row":[["v",1]],"proto":"Object"}]})~", "stmt-all-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    return enc(s.all());
  })
  LINE(R"~(stmt-run-after-close	{"ok":{"$row":[["changes",0],["lastInsertRowid",0]],"proto":"Object"}})~", "stmt-run-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    return enc(s.run());
  })
  LINE(R"~(stmt-iterate-after-close	{"ok":[{"$row":[["v",1]],"proto":"Object"}]})~", "stmt-iterate-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    std::vector<Row> rows;
    for (Row& row : s.iterate()) rows.push_back(row);
    return enc(rows);
  })
  LINE(R"~(stmt-params-after-close	{"ok":{"$row":[["v",1]],"proto":"Object"}})~", "stmt-params-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ? AS v");
    db.close();
    return enc(s.get(1.0));
  })
  LINE(R"~(stmt-paramsCount-after-close	{"ok":1})~", "stmt-paramsCount-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ? AS v");
    db.close();
    return s.paramsCount();
  })
  LINE(R"~(stmt-safeIntegers-after-close	{"ok":[false,true]})~", "stmt-safeIntegers-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ? AS v");
    db.close();
    const bool before = s.safeIntegers();
    return list({flag(before), flag(s.safeIntegers(true) == s)});
  })
  LINE(R"~(stmt-finalize-after-close	{"ok":{"$undefined":true}})~", "stmt-finalize-after-close", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ? AS v");
    db.close();
    s.finalize();
    return undefinedValue();
  })
  LINE(R"~(prep-get-after-close-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "prep-get-after-close-true", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close(true);
    return enc(s.get());
  })
  LINE(R"~(prep-all-after-close-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "prep-all-after-close-true", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close(true);
    return enc(s.all());
  })
  LINE(R"~(prep-run-after-close-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "prep-run-after-close-true", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close(true);
    return enc(s.run());
  })
  LINE(R"~(prep-iterate-after-close-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "prep-iterate-after-close-true", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close(true);
    std::vector<Row> rows;
    for (Row& row : s.iterate()) rows.push_back(row);
    return enc(rows);
  })
  LINE(R"~(prep-params-after-close-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "prep-params-after-close-true", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ? AS v");
    db.close(true);
    return enc(s.get(1.0));
  })
  LINE(R"~(query-all-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "query-all-after-close", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    return enc(s.all());
  })
  LINE(R"~(query-run-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "query-run-after-close", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    return enc(s.run());
  })
  LINE(R"~(query-iterate-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "query-iterate-after-close", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    std::vector<Row> rows;
    for (Row& row : s.iterate()) rows.push_back(row);
    return enc(rows);
  })
  LINE(R"~(query-params-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "query-params-after-close", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT ? AS v");
    db.close();
    return enc(s.get(1.0));
  })
  LINE(R"~(query-get-after-close-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "query-get-after-close-true", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close(true);
    return enc(s.get());
  })
  LINE(R"~(query-finalize-after-close	{"ok":[true,{"$undefined":true}]})~", "query-finalize-after-close", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    const bool finalized = s.isFinalized();
    s.finalize();
    return list({flag(finalized), undefinedValue()});
  })
  LINE(R"~(query-evicted-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "query-evicted-after-close", {
    const Database db(":memory:");
    const Statement first = db.query("SELECT 0 AS v");
    for (int index = 1; index <= 20; ++index) db.query("SELECT " + std::to_string(index));
    db.close();
    const bool finalized = first.isFinalized();
    return list({flag(finalized), enc(first.get())});
  })
  LINE(R"~(prep-on-zombie-writes	{"ok":[{"$row":[["n",2]],"proto":"Object"},false,false]})~", "prep-on-zombie-writes", {
    const Database db(":memory:");
    db.run("CREATE TABLE t(a)");
    const Statement insert = db.prepare("INSERT INTO t VALUES (?)");
    const Statement select = db.prepare("SELECT count(*) AS n FROM t");
    db.close();
    insert.run(1.0);
    insert.run(2.0);
    return list({enc(select.get()), flag(insert.isFinalized()), flag(select.isFinalized())});
  })
  LINE(R"~(close-true-after-close-false	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "close-true-after-close-false", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    db.close(true);
    return list({flag(s.isFinalized()), enc(s.get())});
  })
  LINE(R"~(finalize-then-close-then-get	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "finalize-then-close-then-get", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    s.finalize();
    db.close();
    return enc(s.get());
  })
  LINE(R"~(close-then-finalize-then-get	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "close-then-finalize-then-get", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    s.finalize();
    return enc(s.get());
  })
  LINE(R"~(close-false-twice-prep	{"ok":[false,{"$row":[["v",1]],"proto":"Object"}]})~", "close-false-twice-prep", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    db.close();
    return list({flag(s.isFinalized()), enc(s.get())});
  })
  LINE(R"~(close-false-then-true-prep-final	{"ok":true})~", "close-false-then-true-prep-final", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    db.close();
    db.close(true);
    return flag(s.isFinalized());
  })
  LINE(R"~(paramsCount-finalized	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "paramsCount-finalized", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ?, ?");
    s.finalize();
    return s.paramsCount();
  })
  LINE(R"~(paramsCount-closed-true	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "paramsCount-closed-true", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ?, ?");
    db.close(true);
    return s.paramsCount();
  })
  LINE(R"~(safeIntegers-finalized	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "safeIntegers-finalized", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT ?, ?");
    s.finalize();
    return flag(s.safeIntegers());
  })
  LINE(R"~(safeIntegers-setter-finalized	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "safeIntegers-setter-finalized", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    s.finalize();
    return caught([&] {
      s.safeIntegers(true);
      return text("no throw");
    });
  })
  LINE(R"~(isFinalized-closed	{"ok":[false,true]})~", "isFinalized-closed", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1");
    const bool before = s.isFinalized();
    db.close(true);
    return list({flag(before), flag(s.isFinalized())});
  })
  LINE(R"~(finalize-after-close-msg	{"ok":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "finalize-after-close-msg", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    s.finalize();
    return caught([&] { return enc(s.get()); });
  })
  LINE(R"~(prepare-error-finalize	{"ok":"closed"})~", "prepare-error-finalize", {
    const Database db(":memory:");
    try {
      db.prepare("SELEC");
    } catch (const rt::JsError&) {
    }
    db.close(true);
    return text("closed");
  })
  LINE(R"~(finalize-then-get	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "finalize-then-get", {
    const Closing db(Database(":memory:"));
    const Statement s = db->prepare("SELECT 1 AS v");
    s.finalize();
    return enc(s.get());
  })
  LINE(R"~(finalize-twice	{"ok":[{"$undefined":true},{"$undefined":true},true]})~", "finalize-twice", {
    const Database db(":memory:");
    const Statement s = db.prepare("SELECT 1 AS v");
    s.finalize();
    s.finalize();
    db.close();
    return list({undefinedValue(), undefinedValue(), flag(s.isFinalized())});
  })
}

TEST("lifetime: closed databases, empty SQL and text without a statement") {
  const auto closed = [] {
    const Database db(":memory:");
    db.close();
    return db;
  };
  LINE(R"~(query-after-close	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Cannot use a closed database","keys":[],"string":"RangeError: Cannot use a closed database","isSQLiteError":false}}})~", "query-after-close", return list({flag(closed().query("SELECT 1").isFinalized())});)
  LINE(R"~(query-empty-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQL query cannot be empty.","keys":[],"string":"Error: SQL query cannot be empty.","isSQLiteError":false}}})~", "query-empty-after-close", return list({flag(closed().query("").isFinalized())});)
  LINE(R"~(prepare-after-close	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Cannot use a closed database","keys":[],"string":"RangeError: Cannot use a closed database","isSQLiteError":false}}})~", "prepare-after-close", return list({flag(closed().prepare("SELECT 1").isFinalized())});)
  LINE(R"~(prepare-empty-closed	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Cannot use a closed database","keys":[],"string":"RangeError: Cannot use a closed database","isSQLiteError":false}}})~", "prepare-empty-closed", return list({flag(closed().prepare("").isFinalized())});)
  LINE(R"~(prepare-space-closed	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Cannot use a closed database","keys":[],"string":"RangeError: Cannot use a closed database","isSQLiteError":false}}})~", "prepare-space-closed", return list({flag(closed().prepare("  ").isFinalized())});)
  LINE(R"~(run-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "run-after-close", return enc(closed().run("SELECT 1"));)
  LINE(R"~(run-empty-after-close	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Database has closed","keys":[],"string":"Error: Database has closed","isSQLiteError":false}}})~", "run-empty-after-close", return enc(closed().run(""));)
  LINE(R"~(fileControl-after-close	{"ok":{"$undefined":true}})~", "fileControl-after-close", {
    const std::optional<int> result = closed().fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 0);
    return result ? json::value(*result) : undefinedValue();
  })
  LINE(R"~(query-on-finalized-cached-closed	{"ok":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Cannot use a closed database","keys":[],"string":"RangeError: Cannot use a closed database","isSQLiteError":false}}})~", "query-on-finalized-cached-closed", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    db.close();
    return caught([&] { return flag(db.query("SELECT 1 AS v").isFinalized()); });
  })
  const Closing db(Database(":memory:"));
  LINE(R"~(q:empty	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"SQL query cannot be empty.","keys":[],"string":"Error: SQL query cannot be empty.","isSQLiteError":false}}})~", "q:empty", return enc(db->query("").all());)
  LINE(R"~(prep:empty	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Invalid SQL statement","keys":[],"string":"RangeError: Invalid SQL statement","isSQLiteError":false}}})~", "prep:empty", return enc(db->prepare("").all());)
  LINE(R"~(query-space	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "query-space", return list({flag(db->query("   ").isFinalized())});)
  LINE(R"~(prepare-space	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "prepare-space", return list({flag(db->prepare("   ").isFinalized())});)
  LINE(R"~(query-space-run	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "query-space-run", return enc(db->query("  ").run());)
  LINE(R"~(prepare-empty	{"threw":{"$error":{"ctor":"RangeError","name":"RangeError","message":"Invalid SQL statement","keys":[],"string":"RangeError: Invalid SQL statement","isSQLiteError":false}}})~", "prepare-empty", return list({flag(db->prepare("").isFinalized())});)
  LINE(R"~(q:comment	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "q:comment", return enc(db->query("-- nothing").all());)
  LINE(R"~(q:semicolon	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})~", "q:semicolon", return enc(db->query(";").all());)
  LINE(R"~(q:two-stmts	{"ok":[{"$row":[["a",1]],"proto":"Object"}]})~", "q:two-stmts", return enc(db->query("SELECT 1 AS a; SELECT 2 AS b").all());)
}

TEST("query cache: one statement per SQL text, the 20 most recently used") {
  LINE(R"~(cache-identity	{"ok":[true,false,false]})~", "cache-identity", {
    const Database db(":memory:");
    const json::value result =
      list({flag(db.query("SELECT 1") == db.query("SELECT 1")), flag(db.prepare("SELECT 1") == db.prepare("SELECT 1")), flag(db.query("SELECT 1") == db.query("SELECT 1 "))});
    db.close();
    return result;
  })
  LINE(R"~(cache-lru	{"ok":[true,true,true,{"$row":[["1",1]],"proto":"Object"},false,20]})~", "cache-lru", {
    const Database db(":memory:");
    const Statement first = db.query("SELECT 0");
    std::vector<Statement> statements{first};
    for (int index = 1; index < 20; ++index) statements.push_back(db.query("SELECT " + std::to_string(index)));
    const bool stillCached = db.query("SELECT 0") == first;
    db.query("SELECT 20");
    const bool evicted = !(db.query("SELECT 1") == statements[1]);
    const bool kept = db.query("SELECT 0") == first;
    const json::value oldUsable = enc(statements[1].get());
    const bool finalized = statements[1].isFinalized();
    db.close();
    return list({flag(stillCached), flag(evicted), flag(kept), oldUsable, flag(finalized), integer(static_cast<long long>(Database::MAX_QUERY_CACHE_SIZE))});
  })
  LINE(R"~(cache-close-clears	{"ok":true})~", "cache-close-clears", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1");
    db.close();
    return flag(s.isFinalized());
  })
  LINE(R"~(finalize-cached-then-query	{"ok":[false,false,{"$row":[["v",1]],"proto":"Object"}]})~", "finalize-cached-then-query", {
    const Database db(":memory:");
    const Statement s = db.query("SELECT 1 AS v");
    s.finalize();
    const Statement t = db.query("SELECT 1 AS v");
    const json::value result = list({flag(s == t), flag(t.isFinalized()), enc(t.get())});
    db.close();
    return result;
  })
}

// ---- errors (scratch o5_life.ts, o8.ts) ------------------------------------------------------------

TEST("errors: SQLiteError name, message, code, errno and byteOffset for compile and run failures") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE t (id INTEGER PRIMARY KEY, u UNIQUE, n NOT NULL DEFAULT 1, c CHECK (c > 0))");
  db->run("CREATE TABLE s (x INTEGER) STRICT");
  std::string compound = "SELECT 1 UNION ";
  for (int index = 0; index < 600; ++index) compound += (index ? " UNION SELECT " : "SELECT ") + std::to_string(index);
  const std::pair<const char*, std::string> compileCases[] = {
    {"syntax", "SELEC 1"},
    {"syntax-mid", "SELECT 1 FROM"},
    {"syntax-token", "SELECT 1 +* 2"},
    {"missing-table", "SELECT * FROM missing"},
    {"missing-col", "SELECT nope FROM t"},
    {"missing-fn", "SELECT nofn(1)"},
    {"bad-collate", "SELECT 1 COLLATE nope"},
    {"unterminated", "SELECT 'abc"},
    {"unrecognized", "SELECT 1 ` 2"},
    {"too-many-terms", compound},
    {"ambiguous", "SELECT id FROM t, t AS t2"},
    {"wrong-args", "SELECT substr()"},
    {"misuse-agg", "SELECT count(*) FROM t WHERE count(*) > 1"},
    {"unicode-error-pos", "SELECT '\xC3\xA9\xF0\x9F\x98\x80' FRM t"},
  };
  const std::string expected[] = {
    R"~(q:syntax	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~",
    R"~(prep:syntax	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~",
    R"~(run:syntax	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"SELEC\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":0,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"SELEC\": syntax error","isSQLiteError":true}}})~",
    R"~(q:syntax-mid	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"incomplete input","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: incomplete input","isSQLiteError":true}}})~",
    R"~(prep:syntax-mid	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"incomplete input","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: incomplete input","isSQLiteError":true}}})~",
    R"~(run:syntax-mid	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"incomplete input","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: incomplete input","isSQLiteError":true}}})~",
    R"~(q:syntax-token	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"*\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":10,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"*\": syntax error","isSQLiteError":true}}})~",
    R"~(prep:syntax-token	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"*\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":10,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"*\": syntax error","isSQLiteError":true}}})~",
    R"~(run:syntax-token	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"*\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":10,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"*\": syntax error","isSQLiteError":true}}})~",
    R"~(q:missing-table	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such table: missing","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such table: missing","isSQLiteError":true}}})~",
    R"~(prep:missing-table	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such table: missing","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such table: missing","isSQLiteError":true}}})~",
    R"~(run:missing-table	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such table: missing","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such table: missing","isSQLiteError":true}}})~",
    R"~(q:missing-col	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such column: nope","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such column: nope","isSQLiteError":true}}})~",
    R"~(prep:missing-col	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such column: nope","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such column: nope","isSQLiteError":true}}})~",
    R"~(run:missing-col	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such column: nope","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such column: nope","isSQLiteError":true}}})~",
    R"~(q:missing-fn	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such function: nofn","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such function: nofn","isSQLiteError":true}}})~",
    R"~(prep:missing-fn	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such function: nofn","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such function: nofn","isSQLiteError":true}}})~",
    R"~(run:missing-fn	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such function: nofn","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such function: nofn","isSQLiteError":true}}})~",
    R"~(q:bad-collate	{"ok":[{"$row":[["1 COLLATE nope",1]],"proto":"Object"}]})~",
    R"~(prep:bad-collate	{"ok":[{"$row":[["1 COLLATE nope",1]],"proto":"Object"}]})~",
    R"~(run:bad-collate	{"ok":{"$row":[["changes",0],["lastInsertRowid",0]],"proto":"Object"}})~",
    R"~(q:unterminated	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unrecognized token: \"'abc\"","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unrecognized token: \"'abc\"","isSQLiteError":true}}})~",
    R"~(prep:unterminated	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unrecognized token: \"'abc\"","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unrecognized token: \"'abc\"","isSQLiteError":true}}})~",
    R"~(run:unterminated	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unrecognized token: \"'abc\"","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unrecognized token: \"'abc\"","isSQLiteError":true}}})~",
    R"~(q:unrecognized	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unrecognized token: \"` 2\"","code":"SQLITE_ERROR","errno":1,"byteOffset":9,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unrecognized token: \"` 2\"","isSQLiteError":true}}})~",
    R"~(prep:unrecognized	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unrecognized token: \"` 2\"","code":"SQLITE_ERROR","errno":1,"byteOffset":9,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unrecognized token: \"` 2\"","isSQLiteError":true}}})~",
    R"~(run:unrecognized	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unrecognized token: \"` 2\"","code":"SQLITE_ERROR","errno":1,"byteOffset":9,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unrecognized token: \"` 2\"","isSQLiteError":true}}})~",
    R"~(q:too-many-terms	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"too many terms in compound SELECT","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: too many terms in compound SELECT","isSQLiteError":true}}})~",
    R"~(prep:too-many-terms	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"too many terms in compound SELECT","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: too many terms in compound SELECT","isSQLiteError":true}}})~",
    R"~(run:too-many-terms	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"too many terms in compound SELECT","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: too many terms in compound SELECT","isSQLiteError":true}}})~",
    R"~(q:ambiguous	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"ambiguous column name: id","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: ambiguous column name: id","isSQLiteError":true}}})~",
    R"~(prep:ambiguous	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"ambiguous column name: id","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: ambiguous column name: id","isSQLiteError":true}}})~",
    R"~(run:ambiguous	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"ambiguous column name: id","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: ambiguous column name: id","isSQLiteError":true}}})~",
    R"~(q:wrong-args	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"wrong number of arguments to function substr()","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: wrong number of arguments to function substr()","isSQLiteError":true}}})~",
    R"~(prep:wrong-args	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"wrong number of arguments to function substr()","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: wrong number of arguments to function substr()","isSQLiteError":true}}})~",
    R"~(run:wrong-args	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"wrong number of arguments to function substr()","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: wrong number of arguments to function substr()","isSQLiteError":true}}})~",
    R"~(q:misuse-agg	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"misuse of aggregate: count()","code":"SQLITE_ERROR","errno":1,"byteOffset":29,"keys":["code","errno","byteOffset"],"string":"SQLiteError: misuse of aggregate: count()","isSQLiteError":true}}})~",
    R"~(prep:misuse-agg	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"misuse of aggregate: count()","code":"SQLITE_ERROR","errno":1,"byteOffset":29,"keys":["code","errno","byteOffset"],"string":"SQLiteError: misuse of aggregate: count()","isSQLiteError":true}}})~",
    R"~(run:misuse-agg	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"misuse of aggregate: count()","code":"SQLITE_ERROR","errno":1,"byteOffset":29,"keys":["code","errno","byteOffset"],"string":"SQLiteError: misuse of aggregate: count()","isSQLiteError":true}}})~",
    R"~(q:unicode-error-pos	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"t\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":20,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"t\": syntax error","isSQLiteError":true}}})~",
    R"~(prep:unicode-error-pos	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"t\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":20,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"t\": syntax error","isSQLiteError":true}}})~",
    R"~(run:unicode-error-pos	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"near \"t\": syntax error","code":"SQLITE_ERROR","errno":1,"byteOffset":20,"keys":["code","errno","byteOffset"],"string":"SQLiteError: near \"t\": syntax error","isSQLiteError":true}}})~"
  };
  std::size_t next = 0;
  for (const auto& [name, sql] : compileCases) {
    LINE(expected[next++], std::string("q:") + name, return enc(db->query(sql).all());)
    LINE(expected[next++], std::string("prep:") + name, return enc(db->prepare(sql).all());)
    LINE(expected[next++], std::string("run:") + name, return enc(db->run(sql));)
  }
  const std::pair<const char*, const char*> runCases[] = {
    {"pk", "INSERT INTO t (id, u, c) VALUES (1, 1, 1), (1, 2, 1)"},
    {"unique", "INSERT INTO t (id, u, c) VALUES (10, 5, 1), (11, 5, 1)"},
    {"notnull", "INSERT INTO t (id, u, n, c) VALUES (20, 20, NULL, 1)"},
    {"check", "INSERT INTO t (id, u, c) VALUES (30, 30, -1)"},
    {"strict-type", "INSERT INTO s VALUES ('abc')"},
    {"raise-abort", "SELECT RAISE(ABORT, 'custom message')"},
    {"raise-fail", "SELECT RAISE(FAIL, 'fail message')"},
    {"raise-ignore", "SELECT RAISE(IGNORE)"},
    {"int-overflow", "SELECT abs(-9223372036854775808)"},
    {"divzero", "SELECT 1 / 0 AS v"},
    {"json-bad", "SELECT json('{')"},
    {"too-big-blob", "SELECT zeroblob(2000000000)"},
  };
  const std::string runExpected[] = {
    R"~(e:pk	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"UNIQUE constraint failed: t.id","code":"SQLITE_CONSTRAINT_PRIMARYKEY","errno":1555,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: UNIQUE constraint failed: t.id","isSQLiteError":true}}})~",
    R"~(erun:pk	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"UNIQUE constraint failed: t.id","code":"SQLITE_CONSTRAINT_PRIMARYKEY","errno":1555,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: UNIQUE constraint failed: t.id","isSQLiteError":true}}})~",
    R"~(e:unique	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"UNIQUE constraint failed: t.u","code":"SQLITE_CONSTRAINT_UNIQUE","errno":2067,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: UNIQUE constraint failed: t.u","isSQLiteError":true}}})~",
    R"~(erun:unique	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"UNIQUE constraint failed: t.u","code":"SQLITE_CONSTRAINT_UNIQUE","errno":2067,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: UNIQUE constraint failed: t.u","isSQLiteError":true}}})~",
    R"~(e:notnull	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"NOT NULL constraint failed: t.n","code":"SQLITE_CONSTRAINT_NOTNULL","errno":1299,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: NOT NULL constraint failed: t.n","isSQLiteError":true}}})~",
    R"~(erun:notnull	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"NOT NULL constraint failed: t.n","code":"SQLITE_CONSTRAINT_NOTNULL","errno":1299,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: NOT NULL constraint failed: t.n","isSQLiteError":true}}})~",
    R"~(e:check	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"CHECK constraint failed: c > 0","code":"SQLITE_CONSTRAINT_CHECK","errno":275,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: CHECK constraint failed: c > 0","isSQLiteError":true}}})~",
    R"~(erun:check	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"CHECK constraint failed: c > 0","code":"SQLITE_CONSTRAINT_CHECK","errno":275,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: CHECK constraint failed: c > 0","isSQLiteError":true}}})~",
    R"~(e:strict-type	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"cannot store TEXT value in INTEGER column s.x","code":"SQLITE_CONSTRAINT_DATATYPE","errno":3091,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: cannot store TEXT value in INTEGER column s.x","isSQLiteError":true}}})~",
    R"~(erun:strict-type	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"cannot store TEXT value in INTEGER column s.x","code":"SQLITE_CONSTRAINT_DATATYPE","errno":3091,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: cannot store TEXT value in INTEGER column s.x","isSQLiteError":true}}})~",
    R"~(e:raise-abort	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"RAISE() may only be used within a trigger-program","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: RAISE() may only be used within a trigger-program","isSQLiteError":true}}})~",
    R"~(erun:raise-abort	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"RAISE() may only be used within a trigger-program","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: RAISE() may only be used within a trigger-program","isSQLiteError":true}}})~",
    R"~(e:raise-fail	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"RAISE() may only be used within a trigger-program","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: RAISE() may only be used within a trigger-program","isSQLiteError":true}}})~",
    R"~(erun:raise-fail	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"RAISE() may only be used within a trigger-program","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: RAISE() may only be used within a trigger-program","isSQLiteError":true}}})~",
    R"~(e:raise-ignore	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"RAISE() may only be used within a trigger-program","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: RAISE() may only be used within a trigger-program","isSQLiteError":true}}})~",
    R"~(erun:raise-ignore	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"RAISE() may only be used within a trigger-program","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: RAISE() may only be used within a trigger-program","isSQLiteError":true}}})~",
    R"~(e:int-overflow	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"integer overflow","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: integer overflow","isSQLiteError":true}}})~",
    R"~(erun:int-overflow	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"integer overflow","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: integer overflow","isSQLiteError":true}}})~",
    R"~(e:divzero	{"ok":[{"$row":[["v",null]],"proto":"Object"}]})~",
    R"~(erun:divzero	{"ok":{"$row":[["changes",0],["lastInsertRowid",10]],"proto":"Object"}})~",
    R"~(e:json-bad	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"malformed JSON","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: malformed JSON","isSQLiteError":true}}})~",
    R"~(erun:json-bad	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"malformed JSON","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: malformed JSON","isSQLiteError":true}}})~",
    R"~(e:too-big-blob	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"string or blob too big","code":"SQLITE_TOOBIG","errno":18,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: string or blob too big","isSQLiteError":true}}})~",
    R"~(erun:too-big-blob	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"string or blob too big","code":"SQLITE_TOOBIG","errno":18,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: string or blob too big","isSQLiteError":true}}})~"
  };
  next = 0;
  for (const auto& [name, sql] : runCases) {
    LINE(runExpected[next++], std::string("e:") + name, return enc(db->query(sql).all());)
    LINE(runExpected[next++], std::string("erun:") + name, return enc(db->run(sql));)
  }
  db->run("CREATE TABLE audit (x)");
  db->run("CREATE TRIGGER audit_fault BEFORE INSERT ON audit BEGIN SELECT RAISE(ABORT,'synthetic audit fault'); END");
  LINE(R"~(trigger-raise	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"synthetic audit fault","code":"SQLITE_CONSTRAINT_TRIGGER","errno":1811,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: synthetic audit fault","isSQLiteError":true}}})~", "trigger-raise", return enc(db->run("INSERT INTO audit VALUES (1)"));)
  LINE(R"~(trigger-raise-query	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"synthetic audit fault","code":"SQLITE_CONSTRAINT_TRIGGER","errno":1811,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: synthetic audit fault","isSQLiteError":true}}})~", "trigger-raise-query", return enc(db->query("INSERT INTO audit VALUES (1)").run());)
  LINE(R"~(error-msg-lone	{"ok":[{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such table: �","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such table: �","isSQLiteError":true}},["6e","6f","20","73","75","63","68","20","74","61","62","6c","65","3a","20","fffd"]]})~", "error-msg-lone", {
    try {
      db->query(rt::fromUtf16(u"SELECT * FROM \"\xD800\""));
    } catch (const rt::JsError& error) {
      json::array units;
      for (const std::uint32_t point : rt::codePoints(error.message)) {
        char hex[8];
        const auto end = std::to_chars(hex, hex + sizeof hex, point, 16).ptr;
        units.push_back(json::value(std::string(hex, end)));
      }
      return list({encError(error), units});
    }
    return nullptr;
  })
  LINE(R"~(error-msg-lone-run	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such table: �","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such table: �","isSQLiteError":true}}})~", "error-msg-lone-run", return caught([&] { return enc(db->run(rt::fromUtf16(u"SELECT * FROM \"\xD800\""))); });)
  LINE(R"~(error-msg-utf8	{"ok":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such table: é😀","code":"SQLITE_ERROR","errno":1,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such table: é😀","isSQLiteError":true}}})~", "error-msg-utf8", return caught([&] { return enc(db->query("SELECT * FROM \"\xC3\xA9\xF0\x9F\x98\x80\"").get()); });)
  EXPECT_EQ(sqlite::resultCodeName(1555), std::optional<std::string>("SQLITE_CONSTRAINT_PRIMARYKEY"));
  EXPECT_EQ(sqlite::resultCodeName(100), std::optional<std::string>());
}

// ---- fileControl and pragmas (scratch o5_life.ts, o6_more.ts) ----------------------------------------

TEST("fileControl: SQLite's result code; undefined after close") {
  const TempDir dir;
  LINE(R"~(fileControl-memory	{"ok":12})~", "fileControl-memory", {
    const Closing db(Database(":memory:"));
    return *db->fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 0);
  })
  LINE(R"~(fileControl-file	{"ok":[0,0,0]})~", "fileControl-file", {
    const Closing db(Database(dir.path("fc.db")));
    const int off = *db->fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 0);
    const int query = *db->fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, -1);
    const int on = *db->fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 1);
    return list({off, query, on});
  })
  LINE(R"~(fileControl-unknown-op	{"ok":12})~", "fileControl-unknown-op", {
    const Closing db(Database(dir.path("fc2.db")));
    return *db->fileControl(9999, 0);
  })
  LINE(R"~(persist-wal-effect	{"ok":[true,false,false]})~", "persist-wal-effect", {
    const std::string path = dir.path("wal.db");
    const Database db(path);
    db.fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 0);
    db.run("PRAGMA journal_mode=WAL");
    db.run("CREATE TABLE t(a)");
    const bool during = rt::fs::existsSync(path + "-wal");
    db.close(true);
    return list({flag(during), flag(rt::fs::existsSync(path + "-wal")), flag(rt::fs::existsSync(path + "-shm"))});
  })
}

TEST("pragmas: results through query() and run()") {
  const Closing db(Database(":memory:"));
  db->run("CREATE TABLE t (id INTEGER PRIMARY KEY, v)");
  db->run("PRAGMA user_version = 3");
  LINE(R"~(pragma-journal	{"ok":{"$row":[["journal_mode","memory"]],"proto":"Object"}})~", "pragma-journal", return enc(db->query("PRAGMA journal_mode").get());)
  LINE(R"~(pragma-table-info	{"ok":[{"$row":[["cid",0],["name","id"],["type","INTEGER"],["notnull",0],["dflt_value",null],["pk",1]],"proto":"Object"},{"$row":[["cid",1],["name","v"],["type",""],["notnull",0],["dflt_value",null],["pk",0]],"proto":"Object"}]})~", "pragma-table-info", return enc(db->query("PRAGMA table_info(t)").all());)
  LINE(R"~(pragma-user-version	{"ok":{"$row":[["user_version",3]],"proto":"Object"}})~", "pragma-user-version", return enc(db->query("PRAGMA user_version").get());)
  LINE(R"~(pragma-integrity	{"ok":{"$row":[["integrity_check","ok"]],"proto":"Object"}})~", "pragma-integrity", return enc(db->query("PRAGMA integrity_check").get());)
  LINE(R"~(pragma-recursive-triggers	{"ok":{"$row":[["recursive_triggers",0]],"proto":"Object"}})~", "pragma-recursive-triggers", return enc(db->query("PRAGMA recursive_triggers").get());)
  LINE(R"~(pragma-synchronous	{"ok":{"$row":[["synchronous",2]],"proto":"Object"}})~", "pragma-synchronous", return enc(db->query("PRAGMA synchronous").get());)
  LINE(R"~(pragma-foreign-keys	{"ok":{"$row":[["foreign_keys",0]],"proto":"Object"}})~", "pragma-foreign-keys", return enc(db->query("PRAGMA foreign_keys").get());)
  LINE(R"~(pragma-cache-size	{"ok":{"$row":[["cache_size",-2000]],"proto":"Object"}})~", "pragma-cache-size", return enc(db->query("PRAGMA cache_size").get());)
  LINE(R"~(pragma-temp-store	{"ok":{"$row":[["temp_store",0]],"proto":"Object"}})~", "pragma-temp-store", return enc(db->query("PRAGMA temp_store").get());)
  LINE(R"~(pragma-mmap	{"ok":null})~", "pragma-mmap", return enc(db->query("PRAGMA mmap_size").get());)
  LINE(R"~(pragma-busy-timeout	{"ok":{"$row":[["timeout",0]],"proto":"Object"}})~", "pragma-busy-timeout", return enc(db->query("PRAGMA busy_timeout").get());)
  LINE(R"~(pragma-page-size	{"ok":{"$row":[["page_size",4096]],"proto":"Object"}})~", "pragma-page-size", return enc(db->query("PRAGMA page_size").get());)
  LINE(R"~(pragma-auto-vacuum	{"ok":{"$row":[["auto_vacuum",0]],"proto":"Object"}})~", "pragma-auto-vacuum", return enc(db->query("PRAGMA auto_vacuum").get());)
}

// ---- the gateway's own call patterns (scratch o19_inventory.ts) ---------------------------------

TEST("inventory: kv.ts, convert.ts, native-schema.ts, supervisor-inventory.ts and the authority adapters") {
  const TempDir dir;
  const std::string store = dir.path("store.sqlite3");
  // src/storage/kv.ts prepareDatabase and verifyCodec
  LINE(R"~(kv-prepare	{"ok":"prepared"})~", "kv-prepare", {
    const Database database(store, {.strict = true});
    try {
      database
        .transaction([&] {
          database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
          database.run("INSERT INTO uos_kv_metadata (key, value) VALUES (?, ?)", std::vector<Binding>{"codec", "bun-jsc-v1"});
          database.run("INSERT INTO uos_kv_metadata (key, value) VALUES (?, ?)", std::vector<Binding>{"created_bun_version", "1.4.0"});
        })
        .immediate();
    } catch (...) {
      database.close(true);
      throw;
    }
    database.close(true);
    return text("prepared");
  })
  LINE(R"~(kv-verify	{"ok":[{"$row":[["name","uos_kv_metadata"]],"proto":"Object"},"bun-jsc-v1"]})~", "kv-verify", {
    const Database database(store, {.readonly = true, .strict = true});
    const std::optional<Row> table = database.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'uos_kv_metadata'").get();
    json::value codec = nullptr;
    if (table) {
      const std::optional<Row> row = database.query("SELECT value FROM uos_kv_metadata WHERE key = 'codec'").get();
      codec = row ? enc(*row->get(std::string_view("value"))) : undefinedValue();
    }
    database.close(true);
    return list({enc(table), codec});
  })
  LINE(R"~(kv-verify-missing-table	{"ok":null})~", "kv-verify-missing-table", {
    Database(dir.path("plain.sqlite3")).close();
    const Database database(dir.path("plain.sqlite3"), {.readonly = true, .strict = true});
    const json::value result = enc(database.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'uos_kv_metadata'").get());
    database.close(true);
    return result;
  })
  // src/kv-conversion/convert.ts: the converted database
  const std::string candidate = dir.path("candidate.sqlite3");
  const char* const nativeDdl[] = {
    "CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL, v_encoding INTEGER NOT NULL, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0, expiration_ms INTEGER NOT NULL DEFAULT -1) WITHOUT ROWID",
    "CREATE INDEX kv_expiration_ms_idx ON kv (expiration_ms)",
    "CREATE TABLE migration_state (k INTEGER NOT NULL PRIMARY KEY, version INTEGER NOT NULL)",
    "CREATE TABLE queue (ts INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (ts, id))",
    "CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (deadline, id))",
  };
  LINE(R"~(convert-write	{"ok":[{"$row":[["changes",0],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",0],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",2],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",1],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["changes",0],["lastInsertRowid",{"$bigint":"0"}]],"proto":"Object"},[{"$row":[["type","table"],["name","data_version"],["sql","CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0)"]],"proto":"Object"},{"$row":[["type","table"],["name","kv"],["sql","CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL, v_encoding INTEGER NOT NULL, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0, expiration_ms INTEGER NOT NULL DEFAULT -1) WITHOUT ROWID"]],"proto":"Object"},{"$row":[["type","index"],["name","kv_expiration_ms_idx"],["sql","CREATE INDEX kv_expiration_ms_idx ON kv (expiration_ms)"]],"proto":"Object"},{"$row":[["type","table"],["name","migration_state"],["sql","CREATE TABLE migration_state (k INTEGER NOT NULL PRIMARY KEY, version INTEGER NOT NULL)"]],"proto":"Object"},{"$row":[["type","table"],["name","queue"],["sql","CREATE TABLE queue (ts INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (ts, id))"]],"proto":"Object"},{"$row":[["type","table"],["name","queue_running"],["sql","CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (deadline, id))"]],"proto":"Object"},{"$row":[["type","index"],["name","sqlite_autoindex_queue_1"],["sql",null]],"proto":"Object"},{"$row":[["type","index"],["name","sqlite_autoindex_queue_running_1"],["sql",null]],"proto":"Object"},{"$row":[["type","table"],["name","uos_kv_metadata"],["sql","CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID"]],"proto":"Object"}],[{"$row":[["cid",{"$bigint":"0"}],["name","k"],["type","BLOB"],["notnull",{"$bigint":"1"}],["dflt_value",null],["pk",{"$bigint":"1"}]],"proto":"Object"},{"$row":[["cid",{"$bigint":"1"}],["name","v"],["type","BLOB"],["notnull",{"$bigint":"1"}],["dflt_value",null],["pk",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["cid",{"$bigint":"2"}],["name","v_encoding"],["type","INTEGER"],["notnull",{"$bigint":"1"}],["dflt_value",null],["pk",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["cid",{"$bigint":"3"}],["name","version"],["type","INTEGER"],["notnull",{"$bigint":"1"}],["dflt_value",null],["pk",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["cid",{"$bigint":"4"}],["name","seq"],["type","INTEGER"],["notnull",{"$bigint":"1"}],["dflt_value","0"],["pk",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["cid",{"$bigint":"5"}],["name","expiration_ms"],["type","INTEGER"],["notnull",{"$bigint":"1"}],["dflt_value","-1"],["pk",{"$bigint":"0"}]],"proto":"Object"}],{"$row":[["user_version",{"$bigint":"0"}]],"proto":"Object"},[{"$row":[["k",{"$bigint":"0"}],["version",{"$bigint":"3"}]],"proto":"Object"}],{"$row":[["count",{"$bigint":"0"}]],"proto":"Object"},{"$row":[["value","bun-jsc-v1"]],"proto":"Object"},[{"$row":[["k",{"$bigint":"0"}],["version",{"$bigint":"7"}],["seq",{"$bigint":"0"}]],"proto":"Object"}],{"$row":[["integrity_check","ok"]],"proto":"Object"},[{"$row":[["k",{"$bytes":"026100","ctor":"Uint8Array"}],["v",{"$bytes":"0e000000","ctor":"Uint8Array"}],["v_encoding",{"$bigint":"1"}],["version",{"$bigint":"5"}],["seq",{"$bigint":"0"}],["expiration_ms",{"$bigint":"9500"}]],"proto":"Object"},{"$row":[["k",{"$bytes":"026200","ctor":"Uint8Array"}],["v",{"$bytes":"ffffffffffffffff","ctor":"Uint8Array"}],["v_encoding",{"$bigint":"2"}],["version",{"$bigint":"6"}],["seq",{"$bigint":"0"}],["expiration_ms",{"$bigint":"-1"}]],"proto":"Object"},{"$row":[["k",{"$bytes":"026300","ctor":"Uint8Array"}],["v",{"$bytes":"00ff","ctor":"Uint8Array"}],["v_encoding",{"$bigint":"3"}],["version",{"$bigint":"7"}],["seq",{"$bigint":"1"}],["expiration_ms",{"$bigint":"9223372036854775807"}]],"proto":"Object"}]]})~", "convert-write", {
    const Database output(candidate, {.create = true, .strict = true, .safeIntegers = true});
    json::array result;
    result.push_back(enc(output.run("PRAGMA journal_mode=DELETE")));
    result.push_back(enc(output.run("BEGIN IMMEDIATE")));
    for (const char* statement : nativeDdl) output.run(statement);
    result.push_back(enc(output.run("INSERT INTO migration_state(k,version) VALUES(0,3)")));
    result.push_back(enc(output.run("INSERT INTO data_version(k,version,seq) VALUES(?,?,?)", std::vector<Binding>{std::int64_t{0}, std::int64_t{7}, std::int64_t{0}})));
    output.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
    result.push_back(enc(output.run("INSERT INTO uos_kv_metadata(key,value) VALUES('codec','bun-jsc-v1'),('created_bun_version',?)", std::vector<Binding>{"1.4.0"})));
    const Statement insert = output.query("INSERT INTO kv(k,v,v_encoding,version,seq,expiration_ms) VALUES(?,?,?,?,?,?)");
    result.push_back(enc(insert.run(Bytes{2, 97, 0}, Bytes{14, 0, 0, 0}, std::int64_t{1}, std::int64_t{5}, std::int64_t{0}, std::int64_t{9500})));
    result.push_back(enc(insert.run(Bytes{2, 98, 0}, Bytes(8, 255), std::int64_t{2}, std::int64_t{6}, std::int64_t{0}, std::int64_t{-1})));
    result.push_back(enc(insert.run(Bytes{2, 99, 0}, Bytes{0, 255}, std::int64_t{3}, std::int64_t{7}, std::int64_t{1}, std::numeric_limits<std::int64_t>::max())));
    result.push_back(enc(output.run("COMMIT")));
    result.push_back(enc(output.query("SELECT type,name,sql FROM sqlite_master ORDER BY name").all()));
    result.push_back(enc(output.query("PRAGMA table_info(kv)").all()));
    result.push_back(enc(output.query("PRAGMA user_version").get()));
    result.push_back(enc(output.query("SELECT k,version FROM migration_state").all()));
    result.push_back(enc(output.query("SELECT COUNT(*) AS count FROM queue").get()));
    result.push_back(enc(output.query("SELECT value FROM uos_kv_metadata WHERE key='codec'").get()));
    result.push_back(enc(output.query("SELECT k,version,seq FROM data_version").all()));
    result.push_back(enc(output.query("PRAGMA integrity_check").get()));
    std::vector<Row> rows;
    for (Row& row : output.query("SELECT k,v,v_encoding,version,seq,expiration_ms FROM kv ORDER BY k").iterate()) rows.push_back(row);
    result.push_back(enc(rows));
    output.close(true);
    return result;
  })
  // convert.ts openPrivateSnapshot through native-schema.ts createNativeSnapshotReader
  LINE(R"~(snapshot-read	{"ok":[0,true,false,{"$row":[["k",{"$bytes":"026100","ctor":"Uint8Array"}],["v",{"$bytes":"0e000000","ctor":"Uint8Array"}],["v_encoding",{"$bigint":"1"}],["version",{"$bigint":"5"}],["seq",{"$bigint":"0"}],["expiration_ms",{"$bigint":"9500"}]],"proto":"Object"},3,3,1,1,{"$error":{"ctor":"Error","name":"SQLiteError","message":"attempt to write a readonly database","code":"SQLITE_READONLY","errno":8,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: attempt to write a readonly database","isSQLiteError":true}}]})~", "snapshot-read", {
    const std::string snapshot = dir.path("snapshot.sqlite3");
    rt::fs::copyFileSync(candidate, snapshot);
    const Database database("file://" + snapshot + "?immutable=1", constants::SQLITE_OPEN_READONLY | constants::SQLITE_OPEN_URI);
    json::array result;
    const Statement statement = database.query("SELECT k,v,v_encoding,version,seq,expiration_ms FROM kv ORDER BY k");
    result.push_back(statement.paramsCount());
    result.push_back(true);  // "safeIntegers" in statement
    result.push_back(statement.safeIntegers());
    statement.safeIntegers(true);
    result.push_back(enc(statement.get()));
    result.push_back(static_cast<long long>(statement.all().size()));
    long long count = 0;
    for (Row& row : statement.iterate()) {
      (void)row;
      ++count;
    }
    result.push_back(count);
    result.push_back(database.query("SELECT ?").paramsCount());
    result.push_back(database.query("SELECT $missing").paramsCount());
    result.push_back(caught([&] {
      database.query("DELETE FROM kv").all();
      return text("no");
    }));
    database.close(true);
    return result;
  })
  // src/codex/supervisor-inventory.ts readThreadMetadata
  LINE(R"~(supervisor	{"ok":[[{"$row":[["id","t2"],["title",null],["name","N"],["preview",null],["cwd",null],["model",null],["reasoning_effort",null],["tokens_used",null],["git_branch",null],["updated_at",1700000001],["updated_at_ms",null],["archived",1],["thread_source",null],["source",null]],"proto":"Object"},{"$row":[["id","t1"],["title","T"],["name",null],["preview","p"],["cwd","/w"],["model","m"],["reasoning_effort","high"],["tokens_used",1234],["git_branch","main"],["updated_at",1700000000],["updated_at_ms",1700000000123],["archived",0],["thread_source","cli"],["source","{\"parent_thread_id\":\"t0\"}"]],"proto":"Object"}],[{"$row":[["id","t2"],["title",null],["name","N"],["preview",null],["cwd",null],["model",null],["reasoning_effort",null],["tokens_used",null],["git_branch",null],["updated_at",1700000001],["updated_at_ms",null],["archived",1],["thread_source",null],["source",null]],"proto":"Object"}]]})~", "supervisor", {
    const std::string path = dir.path("state_5.sqlite");
    const Database seed(path);
    seed.run(
      "CREATE TABLE threads (id TEXT PRIMARY KEY, title TEXT, name TEXT, preview TEXT, cwd TEXT, model TEXT, reasoning_effort TEXT, tokens_used INTEGER, git_branch TEXT, updated_at INTEGER, "
      "updated_at_ms INTEGER, archived INTEGER, thread_source TEXT, source TEXT)");
    seed.run(
      "INSERT INTO threads VALUES ('t1', 'T', NULL, 'p', '/w', 'm', 'high', 1234, 'main', 1700000000, 1700000000123, 0, 'cli', '{\"parent_thread_id\":\"t0\"}'), "
      "('t2', NULL, 'N', NULL, NULL, NULL, NULL, NULL, NULL, 1700000001, NULL, 1, NULL, NULL)");
    seed.close();
    const Closing database(Database(path, {.readonly = true, .create = false}));
    const std::string columns = "id, title, name, preview, cwd, model, reasoning_effort, tokens_used, git_branch, updated_at, updated_at_ms, archived, thread_source, source";
    const json::value recent = enc(database->prepare("SELECT " + columns + " FROM threads ORDER BY updated_at DESC LIMIT ?").all(60.0));
    const std::vector<Binding> ids{"t2", "t9"};
    return list({recent, enc(database->prepare("SELECT " + columns + " FROM threads WHERE id IN (?,?)").all(ids))});
  })
  // tests/authority-durable.test.ts and the native authority tests: query(q).all(...bindings), transaction(cb)()
  LINE(R"~(authority-adapter	{"ok":[[],[],[{"$row":[["revision",1]],"proto":"Object"}],[],[{"$row":[["id",1],["schema_version",1],["revision",1],["state_json","{\"a\":2}"]],"proto":"Object"}],{"$error":{"ctor":"Error","name":"SQLiteError","message":"CHECK constraint failed: id = 1","code":"SQLITE_CONSTRAINT_CHECK","errno":275,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: CHECK constraint failed: id = 1","isSQLiteError":true}},[{"$row":[["revision",0]],"proto":"Object"}],[{"$row":[["revision",1]],"proto":"Object"}],[{"$row":[["row_key","k"]],"proto":"Object"}],[{"$row":[["due",null]],"proto":"Object"}],false]})~", "authority-adapter", {
    const Database database(":memory:");
    const auto exec = [&](const char* query, const std::vector<Binding>& bindings) { return enc(database.query(query).all(bindings)); };
    json::array result;
    database.transaction([&] {
      exec("CREATE TABLE IF NOT EXISTS uos_authority_atom (id INTEGER PRIMARY KEY CHECK(id = 1), schema_version INTEGER NOT NULL, revision INTEGER NOT NULL, state_json TEXT NOT NULL)", {});
      result.push_back(exec("SELECT schema_version, revision, state_json FROM uos_authority_atom WHERE id = 1", {}));
      result.push_back(exec("INSERT INTO uos_authority_atom (id, schema_version, revision, state_json) VALUES (1, ?, 0, ?)", {1.0, "{\"a\":1}"}));
    })();
    result.push_back(exec("UPDATE uos_authority_atom SET revision = ?, state_json = ? WHERE id = 1 AND revision = ? RETURNING revision", {1.0, "{\"a\":2}", 0.0}));
    result.push_back(exec("UPDATE uos_authority_atom SET revision = ?, state_json = ? WHERE id = 1 AND revision = ? RETURNING revision", {2.0, "{\"a\":3}", 0.0}));
    result.push_back(exec("SELECT * FROM uos_authority_atom", {}));
    result.push_back(caught([&] { return exec("INSERT INTO uos_authority_atom VALUES (2, 1, 0, '{}')", {}); }));
    exec("CREATE TABLE kvt (row_key TEXT PRIMARY KEY, row_json TEXT NOT NULL, revision INTEGER NOT NULL, expires_at_ms INTEGER)", {});
    const char* upsert =
      "INSERT INTO kvt (row_key,row_json,revision,expires_at_ms) VALUES(?,?,?,?) ON CONFLICT(row_key) DO UPDATE SET row_json=excluded.row_json,revision=kvt.revision+1 RETURNING revision";
    result.push_back(exec(upsert, {"k", "{\"id\":\"x\"}", 0.0, nullptr}));
    result.push_back(exec(upsert, {"k", "{\"id\":\"y\"}", 0.0, 5.0}));
    result.push_back(exec("SELECT row_key FROM kvt WHERE json_extract(row_json,'$.id')=? AND (expires_at_ms IS NULL OR expires_at_ms>?) LIMIT 2", {"y", 1.0}));
    result.push_back(exec("SELECT MIN(expires_at_ms) AS due FROM kvt WHERE expires_at_ms IS NULL OR expires_at_ms>?", {0.0}));
    result.push_back(database.inTransaction());
    database.close();
    return result;
  })
}

#if defined(__APPLE__)
TEST("Darwin WAL sidecars survive close unless persistence is explicitly disabled") {
  const TempDir dir;
  const std::string file = dir.path("persist.db");
  const Database writer(file);
  writer.run("PRAGMA journal_mode=WAL");
  writer.run("CREATE TABLE t(a)");
  writer.run("INSERT INTO t VALUES(7)");
  writer.close(true);
  EXPECT(rt::fs::existsSync(file + "-wal"));
  EXPECT(rt::fs::existsSync(file + "-shm"));
  const Database reader(file, {.readonly = true});
  LINE(R"(persist-readonly	{"ok":{"$row":[["a",7]],"proto":"Object"}})", "persist-readonly", return enc(reader.query("SELECT a FROM t").get());)
  reader.close(true);
  const Database reset(file);
  LINE(R"(reset-row	{"ok":{"$row":[["a",7]],"proto":"Object"}})", "reset-row", return enc(reset.query("SELECT a FROM t").get());)
  EXPECT_EQ(reset.fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 0), std::optional<int>(0));
  reset.close(true);
  EXPECT(!rt::fs::existsSync(file + "-wal"));
  EXPECT(!rt::fs::existsSync(file + "-shm"));
  const Database readonly(file, {.readonly = true});
  LINE(R"(readonly-pragma	{"ok":{"$row":[["timeout",5000]],"proto":"Object"}})", "readonly-pragma", return enc(readonly.query("PRAGMA busy_timeout=5000").get());)
  LINE(R"(readonly-comment	{"threw":{"$error":{"ctor":"Error","name":"Error","message":"Statement has finalized","keys":[],"string":"Error: Statement has finalized","isSQLiteError":false}}})",
       "readonly-comment", return enc(readonly.query("-- comment").get());)
  LINE(R"(reset-readonly	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"unable to open database file","code":"SQLITE_CANTOPEN","errno":14,"byteOffset":-1,"keys":["code","errno","byteOffset"],"string":"SQLiteError: unable to open database file","isSQLiteError":true}}})",
       "reset-readonly", return enc(readonly.query("SELECT a FROM t").get());)
  readonly.close(true);
  EXPECT(!rt::fs::existsSync(file + "-wal"));
  EXPECT(!rt::fs::existsSync(file + "-shm"));
}
#endif

TEST("version: the selected SQLite library") {
  // The behaviour differences documented in docs/rt/sqlite.md were measured against this
  // library; re-measure them when it changes.
  EXPECT_EQ(sqlite::libraryVersion(), std::string(SQLITE_VERSIONED_EXPECTATION("3.40.1", "3.54.0")));
  const Closing db(Database(":memory:"));
  // VERSION: Bun 1.4.0 (SQLite 3.53.2) prints 17 significant digits ("0.33333333333333332",
  // "9007199254740992.0") and knows concat()/octet_length().
  LINE(SQLITE_VERSIONED_EXPECTATION(R"~(real-text	{"ok":{"$row":[["a","0.333333333333333"],["b","9.00719925474099e+15"],["c","1.0e+21"],["d","0.1"]],"proto":"Object"}})~", R"~(real-text	{"ok":{"$row":[["a","0.33333333333333332"],["b","9007199254740992.0"],["c","1.0e+21"],["d","0.1"]],"proto":"Object"}})~"), "real-text",
       return enc(db->query("SELECT CAST(1.0/3 AS TEXT) AS a, CAST(9007199254740993.0 AS TEXT) AS b, CAST(1e21 AS TEXT) AS c, CAST(0.1 AS TEXT) AS d").get());)
  LINE(SQLITE_VERSIONED_EXPECTATION(R"~(concat	{"threw":{"$error":{"ctor":"Error","name":"SQLiteError","message":"no such function: concat","code":"SQLITE_ERROR","errno":1,"byteOffset":7,"keys":["code","errno","byteOffset"],"string":"SQLiteError: no such function: concat","isSQLiteError":true}}})~", R"~(concat	{"ok":{"$row":[["v","ab"]],"proto":"Object"}})~"),
       "concat", return enc(db->query("SELECT concat('a', 'b') AS v").get());)
  // Bun1.4.0 rejects this exact SQL. Host3.54 accepts it before the per-connection limit.
  std::string expression = "SELECT 1";
  for (int index = 0; index < 1000; ++index) expression += "+1";
  EXPECT_THROWS_MATCHING(db->query(expression), "Expression tree is too large \\(maximum depth 1000\\)");
}
