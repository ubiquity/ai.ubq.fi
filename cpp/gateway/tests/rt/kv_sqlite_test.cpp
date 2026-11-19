// uos::rt SqliteKv: the durable store's file format and native semantics. Expected schema text,
// rows and errors were measured on databases the TypeScript store wrote (bun 1.4.0, the
// repository's #kv opener over the pinned native-kv addon) and follow denokv 9fe65cc's
// sqlite/backend.rs; kv_durable_interop_test.cpp checks the two stores against each other and
// kv_durable_suite_test.cpp runs the MemoryKv suite over this store behind the guard.
// Links: tests/rt/kv_sqlite_test.cpp (unit.sh links src/rt). Run with TMPDIR set to a scratch
// directory: every database lives in a temporary directory under it.
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#if defined(__APPLE__)
#include "uos/rt/url.hpp"
#endif
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>

using namespace uos;
using namespace uos::rt;

namespace {
  struct Directory {
    std::string path = fs::realpathSync(fs::mkdtempSync(path::join(os::tmpdir(), "uos-kv-sqlite-")));
    Directory() = default;
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    ~Directory() {
      fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      fs::rmSync(path, options);
    }
    std::string file(std::string_view name = "store.sqlite3") const { return path + "/" + std::string(name); }
  };

  std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
      out += digits[byte >> 4];
      out += digits[byte & 15];
    }
    return out;
  }

  // "Name: message" of the rejection, or "resolved".
  template <class T> Task<std::string> outcome(Task<T> task) {
    try {
      co_await std::move(task);
    } catch (const JsError& error) {
      co_return error.toString();
    }
    co_return "resolved";
  }

  // "ok", "conflict" or the rejection's "Name: message".
  Task<std::string> commitOutcome(Task<KvCommitResult> commit) {
    try {
      const KvCommitResult result = co_await std::move(commit);
      co_return result.ok ? "ok" : "conflict";
    } catch (const JsError& error) {
      co_return error.toString();
    }
  }

  sqlite::Database inspect(const std::string& file) {
    sqlite::DatabaseOptions options;
    options.readonly = true;
    options.safeIntegers = true;
#if defined(__APPLE__)
    // A stopped, checkpointed fixture has no SHM for Apple's readonly WAL
    // reader. Use the same immutable form as the TypeScript snapshot reader;
    // live sidecars always retain the ordinary WAL-aware connection below.
    if (!fs::existsSync(file + "-wal") && !fs::existsSync(file + "-shm") && !fs::existsSync(file + "-journal"))
      return sqlite::Database(pathToFileURL(file).href() + "?immutable=1", options);
#endif
    return sqlite::Database(file, options);
  }

  std::string scalarText(const sqlite::Database& database, std::string_view sql) {
    const std::optional<sqlite::Row> row = database.query(sql).get();
    if (!row || row->size() == 0) return "<none>";
    const sqlite::Value& value = row->begin()->second;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* bytes = std::get_if<sqlite::Bytes>(&value)) return "x" + hex(*bytes);
    if (std::holds_alternative<std::nullptr_t>(value)) return "null";
    return numberToString(std::get<double>(value));
  }

  // One kv row as `v_encoding|hex(v)|version|expiration_ms`.
  std::string kvRow(const std::string& file, const KvKey& key) {
    const sqlite::Database database = inspect(file);
    const std::optional<sqlite::Row> row = database.query("SELECT v, v_encoding, version, expiration_ms FROM kv WHERE k = ?").get(packKey(key));
    std::string out = "<absent>";
    if (row)
      out = std::to_string(*sqlite::getBigInt(row->get(std::string_view("v_encoding")))) + "|" + hex(*sqlite::getBytes(row->get(std::string_view("v")))) + "|" +
            std::to_string(*sqlite::getBigInt(row->get(std::string_view("version")))) + "|" +
            std::to_string(*sqlite::getBigInt(row->get(std::string_view("expiration_ms"))));
    database.close();
    return out;
  }

  std::int64_t dataVersion(const std::string& file) {
    const sqlite::Database database = inspect(file);
    const std::int64_t version = std::stoll(scalarText(database, "SELECT version FROM data_version WHERE k = 0"));
    database.close();
    return version;
  }

  // Deterministic versions and sweeps: a step of 1, no jitter, and the given clock.
  SqliteKvOptions steps(std::function<double()> now = {}) {
    SqliteKvOptions options;
    options.versionIncrement = [] { return std::uint64_t{1}; };
    options.now = std::move(now);
    options.sweepJitterMs = 0;
    return options;
  }

  // How this binary was started (the SDK loader, its arguments and the binary).
  std::vector<std::string> selfCommand() {
#if defined(__APPLE__)
    return {uos::rt::process::execPath()};
#else
    const std::vector<std::uint8_t> bytes = fs::readFileSync("/proc/self/cmdline");
    std::vector<std::string> argv;
    std::string current;
    for (const std::uint8_t byte : bytes) {
      if (byte == 0) {
        argv.push_back(current);
        current.clear();
      } else {
        current.push_back(static_cast<char>(byte));
      }
    }
    const bool viaLoader = !argv.empty() && includes(argv[0], "ld-linux");
    argv.resize(std::min(argv.size(), viaLoader ? std::size_t{4} : std::size_t{1}));
    return argv;
#endif
  }

  // The schema of a database the TypeScript store created, sqlite_master in rowid order
  // (uos_kv_metadata, which src/storage/kv.ts adds before the native open, left out).
  const std::vector<std::string>& nativeSchema() {
    static const std::vector<std::string> schema = {
      "table migration_state CREATE TABLE migration_state(\n  k integer not null primary key,\n  version integer not null\n)",
      "table data_version CREATE TABLE data_version (\n  k integer primary key,\n  version integer not null\n, seq integer not null default 0)",
      "table kv CREATE TABLE kv (\n  k blob primary key,\n  v blob not null,\n  v_encoding integer not null,\n  version integer not null\n, seq integer not null "
      "default 0, expiration_ms integer not null default -1) without rowid",
      "table queue CREATE TABLE queue (\n  ts integer not null,\n  id text not null,\n  data blob not null,\n  backoff_schedule text not null,\n  "
      "keys_if_undelivered blob not null,\n\n  primary key (ts, id)\n)",
      "index sqlite_autoindex_queue_1 null",
      "table queue_running CREATE TABLE queue_running(\n  deadline integer not null,\n  id text not null,\n  data blob not null,\n  backoff_schedule text not "
      "null,\n  keys_if_undelivered blob not null,\n\n  primary key (deadline, id)\n)",
      "index sqlite_autoindex_queue_running_1 null",
      "index kv_expiration_ms_idx CREATE INDEX kv_expiration_ms_idx on kv (expiration_ms)",
    };
    return schema;
  }

  std::vector<std::string> schemaOf(const std::string& file) {
    const sqlite::Database database = inspect(file);
    std::vector<std::string> schema;
    for (const sqlite::Row& row : database.query("SELECT type, name, sql FROM sqlite_master WHERE name <> 'uos_kv_metadata' ORDER BY rowid").all()) {
      const std::string* sql = sqlite::getString(row.get(std::string_view("sql")));
      schema.push_back(*sqlite::getString(row.get(std::string_view("type"))) + " " + *sqlite::getString(row.get(std::string_view("name"))) + " " + (sql ? *sql : "null"));
    }
    database.close();
    return schema;
  }
}

ASYNC_TEST("a new database gets the native schema, migration state and pragmas byte for byte") {
  const Directory directory;
  {
    SqliteKv kv(directory.file());
    kv.close();
  }
  // Closing the only connection checkpoints and removes the WAL (a stopped file is drained).
  EXPECT(!fs::existsSync(directory.file() + "-wal"));
  EXPECT_EQ(schemaOf(directory.file()), nativeSchema());
  const sqlite::Database database = inspect(directory.file());
  EXPECT_EQ(scalarText(database, "SELECT k || ',' || version FROM migration_state"), std::string("0,3"));
  EXPECT_EQ(scalarText(database, "SELECT k || ',' || version || ',' || seq FROM data_version"), std::string("0,0,0"));
#if defined(__APPLE__)
  // Bun's immutable reader reports its connection mode as delete; the stopped
  // database itself still has both WAL-format header bytes set to 2.
  EXPECT_EQ(scalarText(database, "PRAGMA journal_mode"), std::string("delete"));
  const auto header = fs::readFileSync(directory.file());
  EXPECT_EQ(header[18], 2);
  EXPECT_EQ(header[19], 2);
#else
  EXPECT_EQ(scalarText(database, "PRAGMA journal_mode"), std::string("wal"));
#endif
  EXPECT_EQ(scalarText(database, "PRAGMA page_size"), std::string("4096"));
  EXPECT_EQ(scalarText(database, "PRAGMA user_version"), std::string("0"));
  EXPECT_EQ(scalarText(database, "PRAGMA application_id"), std::string("0"));
  database.close();
  co_return;
}

ASYNC_TEST("a database left at an earlier migration is upgraded to the same schema text") {
  const Directory directory;
  {
    const sqlite::Database legacy(directory.file());
    legacy.run("create table if not exists migration_state(\n  k integer not null primary key,\n  version integer not null\n)");
    legacy.run("create table data_version (\n  k integer primary key,\n  version integer not null\n)");
    legacy.run("insert into data_version (k, version) values (0, 0)");
    legacy.run("create table kv (\n  k blob primary key,\n  v blob not null,\n  v_encoding integer not null,\n  version integer not null\n) without rowid");
    legacy.run("replace into migration_state (k, version) values(0, 1)");
    legacy.run("insert into kv (k, v, v_encoding, version) values (x'026100', x'0e00000007', 1, 0)");
    legacy.close();
  }
  SqliteKv kv(directory.file());
  const KvEntryMaybe<> entry = co_await kv.get(KvKey{"a"});
  EXPECT_EQ(entry.value, KvValue(1));
  kv.close();
  EXPECT_EQ(schemaOf(directory.file()), nativeSchema());
  EXPECT_EQ(kvRow(directory.file(), KvKey{"a"}), std::string("1|0e00000007|0|-1"));
}

ASYNC_TEST("rows keep the native encodings, versions and expiry") {
  double clock = 1'700'000'000'000;
  const Directory directory;
  SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
  co_await kv.set(KvKey{"u64"}, KvValue(KvU64(0x0102030405060708)));
  const KvValue raw(KvBytes{0, 255});
  co_await kv.set(KvKey{"bytes"}, raw);
  const KvValue view(KvArrayBufferView{KvViewKind::Uint8Array, KvBytes{9, 1, 2, 9}, 1, 2});
  co_await kv.set(KvKey{"view"}, view);
  const KvValue record = KvValue::object({{"a", 1}, {"b", "x"}});
  co_await kv.set(KvKey{"v8"}, record, {.expireIn = 5'000});
  const KvValue nested = KvValue::object({{"n", KvU64(5)}});
  co_await kv.set(KvKey{"nested"}, nested);
  EXPECT_EQ(kvRow(directory.file(), KvKey{"u64"}), std::string("2|0807060504030201|1|-1"));
  EXPECT_EQ(kvRow(directory.file(), KvKey{"bytes"}), std::string("3|00ff|2|-1"));
  EXPECT_EQ(kvRow(directory.file(), KvKey{"view"}), std::string("3|0102|3|-1"));
  EXPECT_EQ(kvRow(directory.file(), KvKey{"v8"}), "1|" + hex(kvJscSerialize(record)) + "|4|1700000005000");
  EXPECT_EQ(kvRow(directory.file(), KvKey{"nested"}), "1|" + hex(kvJscSerialize(nested)) + "|5|-1");
  EXPECT_EQ((co_await kv.get(KvKey{"u64"})).value, KvValue(KvU64(0x0102030405060708)));
  const KvValue viewed(KvBytes{1, 2});
  EXPECT_EQ((co_await kv.get(KvKey{"view"})).value, viewed);
  const KvValue nestedRead = KvValue::object({{"n", KvValue::object({{"value", BigInt(5)}})}});
  EXPECT_EQ((co_await kv.get(KvKey{"nested"})).value, nestedRead);
  // A plain set and every arithmetic write clear the expiry.
  co_await kv.set(KvKey{"v8"}, record);
  EXPECT_EQ(kvRow(directory.file(), KvKey{"v8"}), "1|" + hex(kvJscSerialize(record)) + "|6|-1");
  co_await kv.set(KvKey{"counter"}, KvValue(KvU64(1)), {.expireIn = 5'000});
  auto sum = kv.atomic();
  sum->sum(KvKey{"counter"}, 2);
  co_await sum->commit();
  EXPECT_EQ(kvRow(directory.file(), KvKey{"counter"}), std::string("2|0300000000000000|8|-1"));
  EXPECT_EQ(dataVersion(directory.file()), 8);
  kv.close();
}

ASYNC_TEST("each commit advances data_version by the native step; failed checks and failed writes do not") {
  const Directory directory;
  std::vector<std::uint64_t> drawn;
  SqliteKvOptions options;
  options.versionIncrement = [&drawn] {
    drawn.push_back(drawn.size() % 9 + 1);
    return drawn.back();
  };
  SqliteKv kv(directory.file(), options);
  const KvCommitResult first = co_await kv.set(KvKey{"a"}, KvValue(1));
  EXPECT_EQ(first.versionstamp, std::string("00000000000000010000"));
  const KvCommitResult second = co_await kv.set(KvKey{"b"}, KvValue(2));
  EXPECT_EQ(second.versionstamp, std::string("00000000000000030000"));
  auto stale = kv.atomic();
  stale->check(AtomicCheck{KvKey{"a"}, std::nullopt}).set(KvKey{"c"}, KvValue(3));
  const KvCommitResult refused = co_await stale->commit();
  EXPECT(!refused.ok);
  EXPECT_EQ(dataVersion(directory.file()), 3);
  // The rollback patch: a failing mutation undoes the write's earlier mutations and its version.
  co_await kv.set(KvKey{"text"}, KvValue("x"));
  EXPECT_EQ(dataVersion(directory.file()), 6);
  auto failing = kv.atomic();
  failing->set(KvKey{"partial"}, KvValue(true)).sum(KvKey{"text"}, 1);
  EXPECT_EQ(co_await outcome(failing->commit()), std::string("Error: Failed to perform 'sum' mutation on a non-U64 value in the database"));
  EXPECT(!(co_await kv.get(KvKey{"partial"})).versionstamp);
  EXPECT_EQ(dataVersion(directory.file()), 6);
  // Operand errors of the native mutations come after the checks (a failed check wins).
  auto vSum = kv.atomic();
  std::vector<KvMutation> sumOfNumber;
  sumOfNumber.push_back(KvMutation{KvKey{"s"}, KvMutationType::sum, KvValue(5), std::nullopt});
  vSum->mutate(sumOfNumber);
  EXPECT_EQ(co_await outcome(vSum->commit()), std::string("Error: Some of the parameters are not valid V8 values"));
  auto vMin = kv.atomic();
  std::vector<KvMutation> minOfNumber;
  minOfNumber.push_back(KvMutation{KvKey{"s"}, KvMutationType::min, KvValue(5), std::nullopt});
  vMin->mutate(minOfNumber);
  EXPECT_EQ(co_await outcome(vMin->commit()), std::string("Error: Failed to perform 'min' mutation on a non-U64 operand"));
  auto checkedSum = kv.atomic();
  checkedSum->check(AtomicCheck{KvKey{"a"}, std::nullopt}).mutate(sumOfNumber);
  EXPECT(!(co_await checkedSum->commit()).ok);
  EXPECT_EQ(dataVersion(directory.file()), 6);
  kv.close();
}

ASYNC_TEST("expired rows stay readable until a sweep: at open, then at the earliest expiry or the interval") {
  double clock = 1'700'000'000'000;
  const Directory directory;
  {
    SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
    co_await kv.set(KvKey{"short"}, KvValue("s"), {.expireIn = 100});
    co_await kv.set(KvKey{"long"}, KvValue("l"), {.expireIn = 120'000});
    co_await kv.set(KvKey{"keep"}, KvValue("k"));
    clock += 500;
    // The sweep at open saw no expiring rows, so the next one is 60 s after it.
    EXPECT((co_await kv.get(KvKey{"short"})).versionstamp.has_value());
    clock += 59'499;
    EXPECT((co_await kv.get(KvKey{"short"})).versionstamp.has_value());
    clock += 1;
    EXPECT(!(co_await kv.get(KvKey{"short"})).versionstamp);
    EXPECT_EQ(dataVersion(directory.file()), 4);
    // That sweep saw `long` expiring at +120 s: the next sweep is then, not 60 s later.
    clock = 1'700'000'000'000 + 119'999;
    EXPECT((co_await kv.get(KvKey{"long"})).versionstamp.has_value());
    clock += 1;
    EXPECT(!(co_await kv.get(KvKey{"long"})).versionstamp);
    EXPECT_EQ(dataVersion(directory.file()), 5);
    co_await kv.set(KvKey{"restart"}, KvValue("r"), {.expireIn = 1'000});
    kv.close();
  }
  // Expiry across restart: the next open sweeps what lapsed while no handle was open.
  clock += 5'000;
  SqliteKv reopened(directory.file(), steps([&clock] { return clock; }));
  EXPECT(!(co_await reopened.get(KvKey{"restart"})).versionstamp);
  EXPECT_EQ((co_await reopened.get(KvKey{"keep"})).value, KvValue("k"));
  EXPECT_EQ(dataVersion(directory.file()), 7);
  reopened.close();
}

ASYNC_TEST("enqueue writes the native queue row with the SDK's fixed backoff schedule") {
  double clock = 1'700'000'000'000;
  const Directory directory;
  SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
  KvEnqueueOptions options;
  options.delay = 2'500;
  std::vector<KvKey> keys;
  keys.push_back(KvKey{"dead", 1});
  options.keysIfUndelivered = keys;
  options.backoffSchedule = std::vector<double>{5'000, 30'000};
  const KvValue message = KvValue::object({{"key_id", "k1"}});
  auto enqueuing = kv.enqueueValue(message, options);
  const KvCommitResult committed = co_await std::move(enqueuing);
  EXPECT_EQ(committed.versionstamp, std::string("00000000000000010000"));
  const KvEnqueueOptions plain;
  auto second = kv.enqueue(KvValue(KvU64(7)), plain);
  co_await std::move(second);
  kv.close();
  const sqlite::Database database = inspect(directory.file());
  const std::vector<sqlite::Row> rows =
    database.query("SELECT ts, id, data, backoff_schedule, keys_if_undelivered, typeof(keys_if_undelivered) AS t FROM queue ORDER BY ts").all();
  ASSERT_EQ(rows.size(), std::size_t{2});
  EXPECT_EQ(*sqlite::getBigInt(rows[0].get(std::string_view("ts"))), std::int64_t{1'700'000'000'000});
  EXPECT_EQ(*sqlite::getBigInt(rows[1].get(std::string_view("ts"))), std::int64_t{1'700'000'002'500});
  static const Regex UUID("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  EXPECT(UUID.test(*sqlite::getString(rows[0].get(std::string_view("id")))));
  EXPECT(*sqlite::getBytes(rows[1].get(std::string_view("data"))) == kvJscSerialize(message));
  // A KvU64 message is a V8 payload (an object), never the LE64 row encoding.
  EXPECT(*sqlite::getBytes(rows[0].get(std::string_view("data"))) == kvJscSerialize(KvValue(KvU64(7))));
  EXPECT_EQ(*sqlite::getString(rows[1].get(std::string_view("backoff_schedule"))), std::string("[100,200,400,800]"));
  EXPECT_EQ(*sqlite::getString(rows[1].get(std::string_view("keys_if_undelivered"))), std::string("[[2,100,101,97,100,0,33,191,240,0,0,0,0,0,0]]"));
  EXPECT_EQ(*sqlite::getString(rows[0].get(std::string_view("keys_if_undelivered"))), std::string("[]"));
  EXPECT_EQ(*sqlite::getString(rows[0].get(std::string_view("t"))), std::string("text"));
  EXPECT_EQ(scalarText(database, "SELECT version FROM data_version"), std::string("2"));
  database.close();
}

ASYNC_TEST("the open requeues abandoned queue messages and delivers exhausted ones to their keys") {
  double clock = 1'700'000'000'000;
  const Directory directory;
  {
    SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
    kv.close();
  }
  const KvBytes payload = kvJscSerialize(KvValue("job"));
  {
    sqlite::DatabaseOptions options;
    options.safeIntegers = true;
    const sqlite::Database database(directory.file(), options);
    const sqlite::Statement running =
      database.query("INSERT INTO queue_running (deadline, id, data, backoff_schedule, keys_if_undelivered) VALUES (?, ?, ?, ?, ?)");
    running.run(std::int64_t{1'699'999'999'000}, "retry", payload, "[1000,2000]", "[]");
    running.run(std::int64_t{1'699'999'999'000}, "exhausted", payload, "[]", "[[2,100,101,97,100,0]]");
    running.run(std::int64_t{1'700'000'009'000}, "alive", payload, "[1000]", "[]");
    database.close();
  }
  SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
  EXPECT_EQ((co_await kv.get(KvKey{"dead"})).value, KvValue("job"));
  kv.close();
  const sqlite::Database database = inspect(directory.file());
  EXPECT_EQ(scalarText(database, "SELECT id || '@' || ts || ' ' || backoff_schedule FROM queue"), std::string("retry@1700000001000 [2000]"));
  EXPECT_EQ(scalarText(database, "SELECT group_concat(id) FROM queue_running"), std::string("alive"));
  EXPECT_EQ(scalarText(database, "SELECT v_encoding || ' ' || version FROM kv WHERE k = x'026465616400'"), std::string("1 1"));
  database.close();
}

ASYNC_TEST("SQLite queue recovery accepts stored JSON at the byte limit and refuses the next byte") {
  double clock = 1'700'000'000'000;
  for (const std::size_t extra : {0u, 1u}) {
    const Directory directory;
    {
      SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
      kv.close();
    }
    {
      const sqlite::Database database(directory.file());
      const auto insert = database.query("INSERT INTO queue_running (deadline, id, data, backoff_schedule, keys_if_undelivered) VALUES (?, ?, ?, ?, ?)");
      std::string schedule = "[]";
      schedule.resize(limits::kJsonDocumentBytes + extra, ' ');
      const auto job = kvJscSerialize(KvValue("job"));
      insert.run(std::int64_t{1'699'999'999'000}, "fixture", job, schedule, "[[2,100,101,97,100,0]]");
      database.close();
    }
    SqliteKv kv(directory.file(), steps([&clock] { return clock; }));
    auto reading = kv.get(KvKey{"dead"});
    if (extra == 0) {
      const auto row = co_await std::move(reading);
      EXPECT_EQ(row.value, KvValue("job"));
    } else {
      EXPECT_REJECTS_MESSAGE(std::move(reading), JsError, "Database is closed.");
    }
    kv.close();
  }
}

ASYNC_TEST("the SDK store's list keeps its cursor at the last entry once the scan ends") {
  const Directory directory;
  SqliteKv kv(directory.file(), steps());
  for (int index = 0; index < 5; index += 1) co_await kv.set(KvKey{"l", index}, KvValue(index));
  KvListSelector selector;
  selector.prefix = KvKey{"l"};
  KvListOptions page;
  page.limit = 2;
  page.batchSize = 1;
  std::unique_ptr<KvListIterator> first = kv.list(selector, page);
  EXPECT_THROWS_MESSAGE((void)first->cursor(), JsError, "Cannot get cursor before first iteration");
  const std::vector<KvEntry<>> head = co_await kvListToArray(*first);
  EXPECT_EQ(head.size(), std::size_t{2});
  KvListOptions rest;
  rest.cursor = first->cursor();
  std::unique_ptr<KvListIterator> second = kv.list(selector, rest);
  // A passed cursor is not the iterator's own until it yields (CursorHolder).
  EXPECT_THROWS_MESSAGE((void)second->cursor(), JsError, "Cannot get cursor before first iteration");
  const std::vector<KvEntry<>> tail = co_await kvListToArray(*second);
  EXPECT_EQ(tail.size(), std::size_t{3});
  EXPECT_EQ(second->cursor(), packCursor(packKey(KvKey{"l", 4})));
  KvListOptions reversed;
  reversed.reverse = true;
  std::unique_ptr<KvListIterator> backwards = kv.list(selector, reversed);
  const std::vector<KvEntry<>> descending = co_await kvListToArray(*backwards);
  EXPECT_EQ(descending.front().value, KvValue(4));
  kv.close();
}

ASYNC_TEST("operations complete asynchronously once the worker is done, in the order they started") {
  const Directory directory;
  SqliteKv kv(directory.file(), steps());
  co_await kv.set(KvKey{"a"}, KvValue(1));
  auto executor = co_await boost::asio::this_coro::executor;
  // The worker's result never arrives synchronously (as with the native addon, it can
  // overtake a handler posted just before; MemoryKv's arrives exactly one macrotask later).
  Promise<KvEntryMaybe<>> pending(kv.get(KvKey{"a"}));
  EXPECT(pending.pending());
  EXPECT_EQ((co_await pending.wait()).value, KvValue(1));
  bool ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  EXPECT_EQ(co_await outcome(kv.get(KvKey{})), std::string("TypeError: Key cannot be empty"));
  EXPECT(!ran);
  co_await boost::asio::post(executor, boost::asio::use_awaitable);
  std::vector<Promise<KvCommitResult>> writes;
  for (int index = 0; index < 20; index += 1) writes.push_back(Promise<KvCommitResult>(kv.set(KvKey{"order"}, KvValue(index))));
  Promise<KvEntryMaybe<>> read(kv.get(KvKey{"order"}));
  auto all = rt::all(std::move(writes));
  const std::vector<KvCommitResult> results = co_await all.wait();
  const KvEntryMaybe<> last = co_await read.wait();
  EXPECT_EQ(last.value, KvValue(19));
  EXPECT_EQ(last.versionstamp, std::optional<std::string>(results.back().versionstamp));
  kv.close();
}

ASYNC_TEST("close follows the SDK and the native handle") {
  const Directory directory;
  SqliteKv kv(directory.file(), steps());
  co_await kv.set(KvKey{"a"}, KvValue(1));
  KvListSelector selector;
  selector.prefix = KvKey{};
  std::unique_ptr<KvListIterator> started = kv.list(selector);
  kv.close();
  EXPECT_THROWS_MESSAGE(kv.close(), JsError, "Cannot call '.close' after '.close' is called");
  EXPECT_EQ(co_await outcome(kv.get(KvKey{"a"})), std::string("Error: Cannot call '.get' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.set(KvKey{"a"}, KvValue(2))), std::string("Error: Cannot call '.set' after '.close' is called"));
  const KvEnqueueOptions plain;
  EXPECT_EQ(co_await outcome(kv.enqueue(KvValue(1), plain)), std::string("Error: Cannot call '.enqueue' after '.close' is called"));
  EXPECT_THROWS_MESSAGE((void)kv.list(selector), JsError, "Cannot call '.list' after '.close' is called");
  EXPECT_EQ(co_await outcome(started->next()), std::string("Error: db not found"));
}

ASYNC_TEST("operations queued when the handle closes fail with the native closed error") {
  const Directory directory;
  SqliteKv kv(directory.file(), steps());
  co_await kv.set(KvKey{"a"}, KvValue(1));
  // Another connection holds the write lock: the next write waits in the busy handler while
  // a read queues behind it; a helper thread releases the lock.
  const sqlite::Database holder(directory.file());
  holder.run("BEGIN IMMEDIATE");
  Promise<KvCommitResult> waiting(kv.set(KvKey{"w"}, KvValue(1)));
  Promise<KvEntryMaybe<>> queued(kv.get(KvKey{"a"}));
  std::thread releaser([holder] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    holder.run("ROLLBACK");
  });
  // Let the worker take the write before closing.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  kv.close();
  releaser.join();
  holder.close();
  EXPECT((co_await waiting.wait()).ok);
  std::string failure = "resolved";
  try {
    co_await queued.wait();
  } catch (const JsError& error) {
    failure = error.toString();
  }
  EXPECT_EQ(failure, std::string("Error: Database is closed."));
}

#if defined(__APPLE__)
TEST("a writer follows Bun's Darwin busy wait when a six-second lock is released") {
#else
TEST("a writer waits five seconds for another connection's lock, then reports it locked") {
#endif
  const Directory directory;
  const sqlite::Database holder(directory.file());
  std::optional<std::thread> releaser;
  std::string writeOutcome;
  std::string readOutcome;
  std::int64_t waited = 0;
  blockOn(async([&]() -> Task<void> {
    SqliteKv kv(directory.file(), steps());
    // The open's own sweep writes, so the lock is taken once it is done.
    co_await kv.set(KvKey{"a"}, KvValue(0));
    holder.run("BEGIN IMMEDIATE");
    releaser.emplace([holder] {
      std::this_thread::sleep_for(std::chrono::milliseconds(6'000));
      holder.run("ROLLBACK");
    });
    // WAL readers are not blocked by the writer's lock.
    auto reading = outcome(kv.get(KvKey{"a"}));
    readOutcome = co_await std::move(reading);
    const auto started = std::chrono::steady_clock::now();
    auto writing = outcome(kv.set(KvKey{"a"}, KvValue(1)));
    writeOutcome = co_await std::move(writing);
    waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    kv.close();
  }));
  releaser->join();
  holder.close();
  EXPECT_EQ(readOutcome, std::string("resolved"));
#if defined(__APPLE__)
  // Bun 1.4.0's busy_timeout=5000 also succeeds after a six-second lock on this
  // host: Darwin coalesces the busy handler's sleeps (measured 6251 ms).
  EXPECT_EQ(writeOutcome, std::string("resolved"));
  EXPECT(waited >= 6'000 && waited < 7'000);
#else
  EXPECT_EQ(writeOutcome, std::string("Error: database is locked"));
  EXPECT(waited >= 4'900 && waited < 5'900);
#endif
}

#if defined(__APPLE__)
TEST("Darwin busy timeout rejects a writer while the other connection still holds its lock") {
  const Directory directory;
  const sqlite::Database holder(directory.file());
  std::string writeOutcome;
  std::int64_t waited = 0;
  blockOn(async([&]() -> Task<void> {
    SqliteKv kv(directory.file(), steps());
    co_await kv.set(KvKey{"a"}, KvValue(0));
    holder.run("BEGIN IMMEDIATE");
    const auto started = std::chrono::steady_clock::now();
    auto writing = outcome(kv.set(KvKey{"a"}, KvValue(1)));
    writeOutcome = co_await std::move(writing);
    waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    EXPECT(holder.inTransaction());
    holder.run("ROLLBACK");
    kv.close();
  }));
  holder.close();
  std::fprintf(stderr, "Darwin held-lock busy timeout: %lld ms\n", static_cast<long long>(waited));
  EXPECT_EQ(writeOutcome, std::string("Error: database is locked"));
  EXPECT(waited >= 4'900);
}
#endif

ASYNC_TEST("two handles on one file share CAS fencing and monotonic versions") {
  const Directory directory;
  SqliteKv first(directory.file());
  SqliteKv second(directory.file());
  const KvEntryMaybe<> absent = co_await first.get(KvKey{"claim"});
  auto one = first.atomic();
  one->check(absent).set(KvKey{"claim"}, KvValue("first"));
  auto two = second.atomic();
  two->check(absent).set(KvKey{"claim"}, KvValue("second"));
  // Both run at once (`Promise.all`): one commits; the other fails its check, or reports the
  // lock when its read snapshot went stale before it could write (the guard retries that).
  Promise<std::string> a(commitOutcome(one->commit()));
  Promise<std::string> b(commitOutcome(two->commit()));
  const std::string firstOutcome = co_await a.wait();
  const std::string secondOutcome = co_await b.wait();
  std::vector<std::string> outcomes = {firstOutcome, secondOutcome};
  EXPECT_EQ(std::count(outcomes.begin(), outcomes.end(), std::string("ok")), 1);
  for (const std::string& result : outcomes) EXPECT(result == "ok" || result == "conflict" || result == "Error: database is locked");
  const KvEntryMaybe<> before = co_await second.get(KvKey{"claim"});
  co_await second.delete_(KvKey{"claim"});
  co_await second.set(KvKey{"claim"}, KvValue("replacement"));
  const KvEntryMaybe<> after = co_await first.get(KvKey{"claim"});
  EXPECT_EQ(after.value, KvValue("replacement"));
  EXPECT(*after.versionstamp > *before.versionstamp);
  auto stale = first.atomic();
  stale->check(before).set(KvKey{"stale-owner"}, KvValue(true));
  EXPECT(!(co_await stale->commit()).ok);
  first.close();
  second.close();
}

ASYNC_TEST("stored rows the native store would refuse to decode fail the read") {
  const Directory directory;
  {
    SqliteKv kv(directory.file(), steps());
    kv.close();
  }
  {
    const sqlite::Database database(directory.file());
    database.run("INSERT INTO kv (k, v, v_encoding, version) VALUES (x'02656e6300', x'00', 7, 1)");
    database.run("INSERT INTO kv (k, v, v_encoding, version) VALUES (x'027536340000', x'01020304050607', 2, 1)");
    database.run("INSERT INTO kv (k, v, v_encoding, version) VALUES (x'026a736300', x'0f00000003', 1, 1)");
    database.close();
  }
  SqliteKv kv(directory.file(), steps());
  EXPECT_EQ(co_await outcome(kv.get(KvKey{"enc"})), std::string("Error: The value encoding 7 is unknown."));
  EXPECT_EQ(co_await outcome(kv.get(KvKey{"u64"})), std::string("Error: The KvU64 value has 7 bytes, not 8."));
  EXPECT_EQ(co_await outcome(kv.get(KvKey{"jsc"})), std::string("TypeError: Unable to deserialize data."));
  kv.close();
}

ASYNC_TEST("the native request limits and the enqueue limits apply before the database") {
  const Directory directory;
  SqliteKv kv(directory.file(), steps());
  auto checks = kv.atomic();
  for (int index = 0; index < 11; index += 1) checks->check(AtomicCheck{KvKey{"c", index}, std::nullopt});
  EXPECT_EQ(co_await outcome(checks->commit()), std::string("Error: TooManyChecks"));
  const std::string longText(2'100, 'k');
  EXPECT_EQ(co_await outcome(kv.set(KvKey{longText}, KvValue(1))), std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await outcome(kv.set(KvKey{"big"}, KvValue(std::string(70'000, 'v')))), std::string("Error: ValueTooLong"));
  KvEnqueueOptions tooMany;
  std::vector<KvKey> eleven;
  for (int index = 0; index < 11; index += 1) eleven.push_back(KvKey{"d", index});
  tooMany.keysIfUndelivered = eleven;
  auto tooManyKeys = kv.enqueue(KvValue(1), tooMany);
  EXPECT_EQ(co_await outcome(std::move(tooManyKeys)), std::string("Error: TooManyQueueUndeliveredKeys"));
  KvEnqueueOptions longKey;
  std::vector<KvKey> one;
  one.push_back(KvKey{longText});
  longKey.keysIfUndelivered = one;
  auto longUndelivered = kv.enqueue(KvValue(1), longKey);
  EXPECT_EQ(co_await outcome(std::move(longUndelivered)), std::string("Error: KeyTooLong"));
  const KvEnqueueOptions plain;
  auto bigPayload = kv.enqueue(KvValue(std::string(70'000, 'p')), plain);
  EXPECT_EQ(co_await outcome(std::move(bigPayload)), std::string("Error: ValueTooLong"));
  KvEnqueueOptions past;
  past.delay = -31.0 * 24 * 60 * 60 * 1000;
  auto pastDeadline = kv.enqueue(KvValue(1), past);
  EXPECT_EQ(co_await outcome(std::move(pastDeadline)), std::string("Error: InvalidMutationEnqueueDeadline"));
  std::vector<KvKey> manyReads;
  for (int index = 0; index < 11; index += 1) manyReads.push_back(KvKey{"r", index});
  EXPECT_EQ(co_await outcome(kv.getMany(manyReads)), std::string("Error: TooManyReadRanges"));
  kv.close();
  const sqlite::Database database = inspect(directory.file());
  EXPECT_EQ(scalarText(database, "SELECT count(*) FROM kv") + "," + scalarText(database, "SELECT count(*) FROM queue"), std::string("0,0"));
  database.close();
}

// The crash child: commits pairs in a loop and acknowledges each one on stdout.
TEST("crash child: writer") {
  if (env::get("UOS_KV_SQLITE_CHILD") != std::optional<std::string>("writer")) return;
  const std::string file = *env::get("UOS_KV_SQLITE_PATH");
  blockOn(async([file]() -> Task<void> {
    SqliteKv kv(file);
    const std::string padding(512, 'p');
    for (double n = 1;; n += 1) {
      const KvEntryMaybe<> current = co_await kv.get(KvKey{"pair", "a"});
      const KvValue record = KvValue::object({{"n", n}, {"padding", padding}});
      std::unique_ptr<AtomicOperation> operation = kv.atomic();
      operation->check(current).set(KvKey{"pair", "a"}, KvValue(n)).set(KvKey{"pair", "b"}, KvValue(n)).set(KvKey{"log", n}, record);
      const KvCommitResult result = co_await operation->commit();
      if (!result.ok) throw JsError("unexpected conflict");
      std::printf("committed %s\n", numberToString(n).c_str());
      std::fflush(stdout);
    }
  }));
}

ASYNC_TEST("a writer killed mid-loop leaves every acknowledged commit and no partial one") {
  const Directory directory;
  const std::string file = directory.file();
  {
    SqliteKv kv(file);
    kv.close();
  }
  std::vector<std::pair<std::string, std::string>> environment;
  environment.emplace_back("UOS_KV_SQLITE_CHILD", "writer");
  environment.emplace_back("UOS_KV_SQLITE_PATH", file);
  process::SpawnOptions options;
  options.env = environment;
  options.stdout = process::Stdio::pipe;
  options.stderr = process::Stdio::inherit;
  std::vector<std::string> command = selfCommand();
  command.emplace_back("crash child: writer");
  const process::Subprocess child = process::spawn(command, options);
  ReadableStreamDefaultReader<std::vector<std::uint8_t>> reader = child.stdout().getReader();
  std::string text;
  double acknowledged = 0;
  while (acknowledged < 40) {
    auto reading = reader.read();
    const ReadableStreamReadResult<std::vector<std::uint8_t>> chunk = co_await std::move(reading);
    if (chunk.done) break;
    text += std::string(chunk.value->begin(), chunk.value->end());
    for (const std::string& line : split(text, "\n"))
      if (startsWith(line, "committed ")) acknowledged = std::max(acknowledged, stringToNumber(slice(line, 10)));
  }
  child.kill("SIGKILL");
  auto exited = child.exited();
  const int code = co_await exited.wait();
  EXPECT_EQ(code, 128 + 9);
  ASSERT(acknowledged >= 40);
  SqliteKv recovered(file, steps());
  std::vector<KvKey> pairKeys;
  pairKeys.push_back(KvKey{"pair", "a"});
  pairKeys.push_back(KvKey{"pair", "b"});
  const std::vector<KvEntryMaybe<>> pair = co_await recovered.getMany(pairKeys);
  ASSERT(pair[0].value.isNumber());
  const double last = pair[0].value.asNumber();
  EXPECT(last >= acknowledged);
  EXPECT_EQ(pair[1].value, KvValue(last));
  EXPECT_EQ(pair[0].versionstamp, pair[1].versionstamp);
  KvListSelector logs;
  logs.prefix = KvKey{"log"};
  std::unique_ptr<KvListIterator> iterator = recovered.list(logs);
  const std::vector<KvEntry<>> entries = co_await kvListToArray(*iterator);
  EXPECT_EQ(static_cast<double>(entries.size()), last);
  EXPECT_EQ(entries.back().versionstamp, *pair[0].versionstamp);
  recovered.close();
  const sqlite::Database database = inspect(file);
  EXPECT_EQ(scalarText(database, "PRAGMA integrity_check"), std::string("ok"));
  database.close();
}
