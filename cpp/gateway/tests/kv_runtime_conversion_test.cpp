// Links: tests/kv_runtime_conversion_test.cpp
//        src/cache/kv_migration_base.cpp src/authority/native_kernel.cpp src/utils.cpp
//        src/storage/kv.cpp src/kv_conversion/profile_json.cpp src/config.cpp
//        src/storage/sqlite_readonly.cpp src/kv_conversion/native_schema.cpp src/kv_conversion/cursors.cpp src/storage/key_compatibility.cpp
//        src/storage/native_kv.cpp src/kv_conversion/convert.cpp src/kernel/quota_v2.cpp
//        src/authority/native_kernel_contracts.cpp src/kv_conversion/typed_value.cpp src/kv.cpp src/release.cpp
//        src/storage/value_compatibility.cpp
// tests/kv-runtime-conversion.test.ts: the cases for src/kv-conversion/cursors.ts,
// src/kv-conversion/native-schema.ts, src/kv-conversion/convert.ts and the `#kv` key guard.
//
// "snapshot reader fails closed if the pinned statement safe-integer API is unavailable" uses
// a minimal in-memory bun_sqlite::Database instead of `new Database(":memory:")`. Like
// bun:sqlite it returns one cached statement per SQL text, so the statement the test disables
// is the one the reader receives.
//
// Native database fixtures use the linked SQLite implementation in the isolated temp directory.
//
// Not ported yet: "actual Bun profile CLI exports, analyzes and imports disposable typed state with
// unchanged filtering", and "graph interchange preserves cycles, sparse arrays, shared buffers,
// offsets, special numbers and errors"
// (src/kv-conversion/profile-json.ts, src/cache/kv-migration-base.ts, scripts/kv-migrate.ts and
// scripts/test-native-kv.ts; the graph case also needs JS object identity, which rt::KvValue trees
// do not have).
#include "uos/cache/kv_migration_base.hpp"
#include "uos/config.hpp"
#include "uos/kv_conversion/profile_json.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/kv_conversion/convert.hpp"
#include "uos/kv_conversion/cursors.hpp"
#include "uos/kv_conversion/native_schema.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"
#include "uos/storage/kv.hpp"
#include <map>

namespace {
  using namespace uos::kv_conversion;
  namespace rt = uos::rt;
  namespace bun_sqlite = native_schema::bun_sqlite;

  class MemoryStatement final : public bun_sqlite::Statement {
  public:
    bool safeIntegersAvailable = true;
    std::int64_t paramsCount() const override { return 0; }
    bool hasSafeIntegers() const override { return safeIntegersAvailable; }
    void safeIntegers(bool) override {}
    std::optional<bun_sqlite::Row> get() override { return bun_sqlite::Row{{"1", std::int64_t{1}}}; }
    std::vector<bun_sqlite::Row> all() override { return {bun_sqlite::Row{{"1", std::int64_t{1}}}}; }
    bun_sqlite::RowIterator iterate() override {
      auto done = std::make_shared<bool>(false);
      return [done]() -> std::optional<bun_sqlite::Row> {
        if (*done) return std::nullopt;
        *done = true;
        return bun_sqlite::Row{{"1", std::int64_t{1}}};
      };
    }
  };

  class MemoryDatabase final : public bun_sqlite::Database {
  public:
    std::shared_ptr<bun_sqlite::Statement> query(const std::string& sql) override {
      std::shared_ptr<MemoryStatement>& statement = statements[sql];
      if (!statement) statement = std::make_shared<MemoryStatement>();
      return statement;
    }
    void close(bool) override {}
    std::map<std::string, std::shared_ptr<MemoryStatement>> statements;
  };

  // expect(() => f()).toThrow(text): an Error whose message contains `text`.
  template <class F> bool throwsIncluding(F f, std::string_view text) {
    try {
      f();
    } catch (const rt::JsError& error) {
      return rt::includes(error.message, text);
    }
    return false;
  }
}

TEST("persisted replay cursors transcode their exact prefix boundary both ways without resetting counters") {
  const std::vector<std::uint8_t> suffix = cursors::packStringKey({"capture"});
  const std::string denoCursor = rt::toBase64Url(suffix);
  const std::vector<std::pair<std::vector<std::string>, rt::KvValue>> cases{
    {{"uos_ai", "sentinel_replay", "v1", "reap_cursor"}, rt::KvValue(denoCursor)},
    {{"uos_ai", "sentinel_replay", "v1", "budget"}, rt::KvValue::object({{"bootstrap_cursor", "4|" + denoCursor}, {"stored_bytes", 123}, {"records", 7}})},
  };
  for (const auto& [parts, value] : cases) {
    const std::vector<std::uint8_t> key = cursors::packStringKey(parts);
    const cursors::TranscodePersistedCursorResult forward = cursors::transcodePersistedCursor(key, value, "deno-to-bun");
    EXPECT_EQ(forward.changed, true);
    EXPECT(cursors::transcodePersistedCursor(key, forward.value, "bun-to-deno").value == value);
  }
  const rt::KvValue paid = rt::KvValue::object({{"request_key", rt::KvValue::array({"paid", rt::BigInt(7), rt::KvBytes{8}})}});
  EXPECT(cursors::transcodePersistedCursor(cursors::packStringKey({"uos_ai", "paid_fallback", "v3", "rollup_backfill_cursor"}), paid, "deno-to-bun").value == paid);
  const rt::KvValue unsupported = rt::KvValue::object({{"bootstrap_cursor", "99|bad"}});
  EXPECT(throwsIncluding([&] { cursors::transcodePersistedCursor(cursors::packStringKey({"uos_ai", "sentinel_replay", "v1", "budget"}), unsupported, "deno-to-bun"); },
                         "Unsupported"));
}

namespace {
  std::vector<std::string> owned;

  // `afterEach`: removes the case's fixture directories, also when it fails.
  struct AfterEach {
    AfterEach() = default;
    AfterEach(const AfterEach&) = delete;
    AfterEach& operator=(const AfterEach&) = delete;
    ~AfterEach() {
      rt::fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      for (const std::string& path : owned) rt::fs::rmSync(path, options);
      owned.clear();
    }
  };

  std::string fixture() {
    const auto taskScratch = rt::env::get("TMPDIR");
    if (!taskScratch || taskScratch->empty()) throw rt::JsError("Converter fixture requires the wrapper TMPDIR");
    const std::string path = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-kv-converter-test-"));
    EXPECT_EQ(rt::path::dirname(rt::path::resolve(path)), rt::path::resolve(*taskScratch));
    owned.push_back(path);
    return path;
  }

  // `const kv = await openKv(path); try { await kv.set(key, value); } finally { kv.close(); }`
  rt::Task<void> writeNativeKvFixture(std::string path, rt::KvKey key, rt::KvValue value) {
    auto opening = uos::storage::kv::openKv(std::move(path));
    const std::shared_ptr<rt::Kv> kv = co_await std::move(opening);
    rt::Reason failure;
    try {
      auto setting = kv->set(std::move(key), std::move(value));
      co_await std::move(setting);
    } catch (...) {
      failure = std::current_exception();
    }
    kv->close();
    if (failure) std::rethrow_exception(failure);
  }

  // bun:sqlite's Database and Statement over rt::sqlite, as createNativeSnapshotReader takes them.
  class SqliteStatement final : public bun_sqlite::Statement {
  public:
    explicit SqliteStatement(rt::sqlite::Statement statement_) : statement(std::move(statement_)) {}
    std::int64_t paramsCount() const override { return statement.paramsCount(); }
    bool hasSafeIntegers() const override { return true; }
    void safeIntegers(bool enabled) override { statement.safeIntegers(enabled); }
    std::optional<bun_sqlite::Row> get() override { return statement.get(); }
    std::vector<bun_sqlite::Row> all() override { return statement.all(); }
    bun_sqlite::RowIterator iterate() override {
      auto rows = std::make_shared<rt::sqlite::RowIterator>(statement.iterate());
      return [rows]() { return rows->next(); };
    }

  private:
    rt::sqlite::Statement statement;
  };

  class SqliteDatabase final : public bun_sqlite::Database {
  public:
    explicit SqliteDatabase(rt::sqlite::Database database_) : database(std::move(database_)) {}
    std::shared_ptr<bun_sqlite::Statement> query(const std::string& sql) override { return std::make_shared<SqliteStatement>(database.query(sql)); }
    void close(bool throwOnError) override { database.close(throwOnError); }

  private:
    rt::sqlite::Database database;
  };

  // A bun:sqlite Database passed where the TS expects a NativeReader.
  native_schema::NativeReader nativeReader(const rt::sqlite::Database& database) {
    native_schema::NativeReader reader;
    reader.query = [database](const std::string& sql) {
      const rt::sqlite::Statement statement = database.query(sql);
      native_schema::NativeReadStatement read;
      read.get = [statement] { return statement.get(); };
      read.all = [statement] { return statement.all(); };
      read.iterate = [statement]() -> bun_sqlite::RowIterator {
        auto rows = std::make_shared<rt::sqlite::RowIterator>(statement.iterate());
        return [rows]() { return rows->next(); };
      };
      return read;
    };
    return reader;
  }

  // `toEqual` on a row of bigint columns, in Bun's column order.
  bool sameRow(const std::optional<bun_sqlite::Row>& row, const std::vector<std::pair<std::string, std::int64_t>>& expected) {
    if (!row || row->size() != expected.size()) return false;
    std::size_t index = 0;
    for (const auto& [name, value] : *row) {
      if (name != expected[index].first || !std::holds_alternative<std::int64_t>(value) || std::get<std::int64_t>(value) != expected[index].second) return false;
      ++index;
    }
    return true;
  }


  bool matches(const std::optional<std::string>& text, const char* pattern, const char* flags = "") {
    const rt::Regex regex(pattern, flags);
    return text.has_value() && regex.test(*text);
  }

  bool hasSidecar(const std::string& path) {
    for (const char* suffix : {"-wal", "-shm", "-journal"})
      if (rt::fs::existsSync(path + suffix)) return true;
    return false;
  }
}

TEST("raw tuple guard rejects negative NaN and truncation without searching inside byte strings") {
  EXPECT(throwsIncluding([] { uos::storage::kv::assertPortableKvKeyBytes(rt::fromHex("026e616e00210007ffffffffffff")); }, "Negative NaN"));
  for (const char* key : {"21fff8000000000000", "217fffffffffffffff", "218000000000000000", "21000fffffffffffff", "012100ff07ffffffffffff00"}) {
    bool threw = false;
    try {
      uos::storage::kv::assertPortableKvKeyBytes(rt::fromHex(key));
    } catch (...) {
      threw = true;
    }
    EXPECT(!threw);
  }
  EXPECT(throwsIncluding([] { uos::storage::kv::assertPortableKvKeyBytes(std::vector<std::uint8_t>{33, 0}); }, "Truncated"));
}

// `Object.keys(statement)` is ["all", "get", "iterate"] and `Object.keys(reader)` is ["close",
// "query"] by construction in C++: NativeReadStatement and NativeSnapshotReader have exactly those
// members.
ASYNC_TEST("immutable native snapshot reader preserves signed64-bit integers without parameters, writes or close sidecars") {
  AfterEach cleanup;
  const std::string root = fixture();
  const std::string source = rt::path::join(root, "native.sqlite3");
  const std::string snapshot = rt::path::join(root, "private # % ?.sqlite3");
  auto writing = writeNativeKvFixture(source, rt::KvKey{"snapshot"}, rt::KvValue::object({{"count", rt::BigInt::fromString("9007199254740993")}}));
  co_await std::move(writing);
  EXPECT_EQ(static_cast<int>(rt::fs::readFileSync(source).at(18)), 2);
  auto digestingSource = convert::fileDigest(source);
  const std::string before = co_await std::move(digestingSource);
  rt::fs::copyFileSync(source, snapshot, rt::fs::constants::COPYFILE_EXCL);
  auto digestingSnapshot = convert::fileDigest(snapshot);
  const std::string copied = co_await std::move(digestingSnapshot);
  EXPECT_EQ(copied, before);
  EXPECT(!hasSidecar(source));
  EXPECT(!hasSidecar(snapshot));
  rt::URL uri = rt::pathToFileURL(snapshot);
  uri.searchParams().set("immutable", "1");
  const rt::sqlite::Database database(uri.href(), rt::sqlite::constants::SQLITE_OPEN_READONLY | rt::sqlite::constants::SQLITE_OPEN_URI);
  const native_schema::NativeSnapshotReader reader = native_schema::createNativeSnapshotReader(std::make_shared<SqliteDatabase>(database));
  rt::Reason failure;
  try {
    EXPECT(native_schema::verifyNativeSchema(reader, "bun-jsc-v1").version > 0);
    const native_schema::NativeReadStatement statement =
      reader.query("SELECT 9007199254740993 AS exact, -9223372036854775808 AS minimum, 9223372036854775807 AS maximum");
    const std::vector<std::pair<std::string, std::int64_t>> expected{
      {"exact", 9007199254740993},
      {"minimum", INT64_MIN},
      {"maximum", INT64_MAX},
    };
    EXPECT(sameRow(statement.get(), expected));
    const std::vector<bun_sqlite::Row> all = statement.all();
    EXPECT(all.size() == 1 && sameRow(all[0], expected));
    std::vector<bun_sqlite::Row> iterated;
    const bun_sqlite::RowIterator next = statement.iterate();
    while (std::optional<bun_sqlite::Row> row = next()) iterated.push_back(std::move(*row));
    EXPECT(iterated.size() == 1 && sameRow(iterated[0], expected));
    EXPECT(throwsIncluding([&] { reader.query("SELECT ?"); }, "must not have bound parameters"));
    EXPECT(throwsIncluding([&] { reader.query("SELECT $missing"); }, "must not have bound parameters"));
    std::optional<std::string> refused;
    try {
      reader.query("DELETE FROM kv").all();
    } catch (...) {
      refused = rt::reasonToString(std::current_exception());
    }
    EXPECT(matches(refused, "readonly", "i"));
  } catch (...) {
    failure = std::current_exception();
  }
  reader.close();
  if (failure) std::rethrow_exception(failure);
  auto digestingSnapshotAfter = convert::fileDigest(snapshot);
  const std::string snapshotAfter = co_await std::move(digestingSnapshotAfter);
  EXPECT_EQ(snapshotAfter, before);
  auto digestingSourceAfter = convert::fileDigest(source);
  const std::string sourceAfter = co_await std::move(digestingSourceAfter);
  EXPECT_EQ(sourceAfter, before);
  EXPECT(!hasSidecar(source));
  EXPECT(!hasSidecar(snapshot));
}

TEST("snapshot reader fails closed if the pinned statement safe-integer API is unavailable") {
  const auto database = std::make_shared<MemoryDatabase>();
  database->query("SELECT 1");
  database->statements.at("SELECT 1")->safeIntegersAvailable = false;
  const native_schema::NativeSnapshotReader reader = native_schema::createNativeSnapshotReader(database);
  EXPECT(throwsIncluding([&] { reader.query("SELECT 1"); }, "safe integers are unavailable"));
  reader.close();
}

TEST("native schema validation refuses unsupported schemas and queues") {
  AfterEach cleanup;
  const std::string root = fixture();
  const std::string source = rt::path::join(root, "source.sqlite3");
  rt::sqlite::DatabaseOptions databaseOptions;
  databaseOptions.safeIntegers = true;
  const rt::sqlite::Database db(source, databaseOptions);
  for (const std::string_view ddl : native_schema::NATIVE_DDL) db.run(ddl);
  db.run("INSERT INTO migration_state VALUES(0,3)");
  db.run("INSERT INTO data_version VALUES(0,11,0)");
  db.run("INSERT INTO queue VALUES(0,'queued',X'00','[]',X'00')");
  EXPECT(throwsIncluding([&] { native_schema::verifyNativeSchema(nativeReader(db), "deno-v8"); }, "Nonempty"));
  db.run("DELETE FROM queue");
  db.run("UPDATE data_version SET version='unsupported'");
  EXPECT(throwsIncluding([&] { native_schema::verifyNativeSchema(nativeReader(db), "deno-v8"); }, "metadata types"));
  db.run("UPDATE data_version SET version=11");
  db.run("CREATE TABLE unknown_state (value TEXT)");
  EXPECT(throwsIncluding([&] { native_schema::verifyNativeSchema(nativeReader(db), "deno-v8"); }, "Unsupported native KV tables"));
  db.close(true);
}

TEST("retains profile JSON wire tags, nested values, U64 identity and profile namespace filtering") {
  namespace base = uos::cache::kv_migration_base;
  namespace profile = uos::kv_conversion::profile_json;
  const rt::KvValue source = rt::KvValue::object({
    {"nested", rt::KvValue::array({rt::KvValue(), rt::BigInt(4), rt::KvBytes{0, 255}})},
    {"when", rt::KvDate{1700000000000}}, {"set", rt::KvSet{rt::BigInt(2)}},
    {"map", rt::KvMap{{true, "yes"}}}, {"count", rt::KvU64(rt::BigInt::fromString("18446744073709551615"))}});
  const rt::KvKey key{"uos_ai", "runtime_config", "v2", rt::KvBytes{7}, rt::BigInt(9), true, rt::Number::POSITIVE_INFINITY};
  const rt::KvEntry<> entry{.key = key, .value = source, .versionstamp = "00000000000000010000"};
  const auto row = profile::entryToJson(entry); EXPECT_EQ(std::string(row.value.at("type").as_string()), std::string("object"));
  EXPECT_EQ(profile::toKey(row.key), key); const std::string line = rt::jsonStringify(uos::json::value_from(row));
  EXPECT_EQ(base::parseKvMigrationEntryLine(line).value, source);
  const uos::json::value wire = uos::json::object{{"type", "KvU64"}, {"value", "42"}};
  EXPECT_EQ(profile::toValue(wire).asU64().toBigInt(), rt::BigInt(42));
  const base::KvMigrationClassifyOptions prod{.profile = "prod", .includeCache = false, .includeLegacy = false};
  const base::KvMigrationClassifyOptions local{.profile = "local", .includeCache = false, .includeLegacy = true};
  EXPECT_EQ(base::classifyKvMigrationKey(rt::KvKey{"uos_ai", "sentinel_replay", "v1", "manifest", "x"}, prod).action, std::string("skip"));
  EXPECT_EQ(base::classifyKvMigrationKey(rt::KvKey{"ubq_ai", "codex_auth"}, local).action, std::string("import"));
  EXPECT_EQ(base::classifyKvMigrationKey(rt::KvKey{"ubq_ai", "codex_auth"}, prod).action, std::string("skip"));
}
ASYNC_TEST("profile export is NDJSON through actual Bun KV with typed values") {
  namespace base = uos::cache::kv_migration_base;
  namespace profile = uos::kv_conversion::profile_json;
  const auto taskScratch = rt::env::get("TMPDIR");
  if (!taskScratch || taskScratch->empty()) throw rt::JsError("Profile fixture requires the wrapper TMPDIR");
  rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c8-isolated"}, {"TMPDIR", *taskScratch}}};
  const auto scratchRoot = rt::path::resolve(*taskScratch); EXPECT_EQ(rt::path::resolve(rt::os::tmpdir()), scratchRoot);
  const std::string saved = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear();
  const auto restore = rt::scopeExit([saved] { uos::config::config.codexAuthJsonB64 = saved; });
  const std::string memory(":memory:"); auto opening = uos::storage::kv::openKv(memory); const auto kv = co_await std::move(opening);
  const auto closing = rt::scopeExit([kv] { kv->close(); });
  const rt::KvKey key{"uos_ai", "runtime_config", "v2"}; const rt::KvValue value = rt::KvValue::object({{"payload", rt::KvBytes{1, 2}}, {"missing", rt::KvValue()}});
  auto setting = kv->set(key, value); co_await std::move(setting);
  const rt::KvListSelector selector{.prefix = rt::KvKey{}}; auto stream = profile::exportEntries(*kv, selector); rt::Response response{rt::BodyInit(stream)};
  const auto text = co_await response.text(); EXPECT(rt::endsWith(text, "\n")); EXPECT_EQ(base::parseKvMigrationEntryLine(rt::trim(text)).value, value);
}
