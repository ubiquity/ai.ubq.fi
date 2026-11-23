// The MemoryKv semantic suite run against the durable store: every KV_TEST case of
// tests/rt/kv_test.cpp (35 cases; its helpers without runTask, then its lines from the first
// KV_TEST to the last, copied by a script: `KV_TEST("x")` became `ASYNC_TEST("durable: x")` and
// MemoryKv, MemoryKvDatabase, MemoryKvOptions the fixtures below; regenerate it the same way when
// kv_test.cpp's store cases change) over rt::SqliteKv
// behind the native-kv.ts guard (storage::native_kv::guardNativeKv), which is what
// src/storage/kv.ts's openKv returns. Each store is a database file under TMPDIR; a fixture's
// `row`/`rows`/`version` read the file with rt::sqlite as the TS tests read it with bun:sqlite.
// Left out: "operations complete one macrotask later; SDK validation errors do not wait", which
// pins MemoryKv's exact macrotask (the worker's result can overtake a handler posted just
// before it, as the native addon's can); tests/rt/kv_sqlite_test.cpp checks what holds.
// Links: tests/kv_durable_suite_test.cpp src/storage/native_kv.cpp (unit.sh links src/rt).
// Run with TMPDIR set to a scratch directory.
#include "uos/rt/fs.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/weak.hpp"
#include "uos/storage/native_kv.hpp"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace uos;
using namespace uos::rt;
using boost::asio::experimental::awaitable_operators::operator&&;

namespace {
  // MemoryKvOptions for the durable store: the version step (empty: +1, deterministic) and the
  // sweep interval (the native store's 60 s fallback; 0 sweeps before every operation).
  struct DurableOptions {
    std::function<std::uint64_t()> versionIncrement{};
    double sweepIntervalMs = 60'000;
  };

  // MemoryKvDatabase for the durable store: one database file, removed with the object.
  class DurableDatabase {
  public:
    explicit DurableDatabase(std::function<double()> now, DurableOptions options = {}) : now_(std::move(now)), options_(std::move(options)) {
      directory_ = fs::realpathSync(fs::mkdtempSync(path::join(os::tmpdir(), "uos-kv-suite-")));
      file_ = directory_ + "/store.sqlite3";
    }
    ~DurableDatabase() {
      fs::RmOptions remove;
      remove.recursive = true;
      remove.force = true;
      fs::rmSync(directory_, remove);
    }
    DurableDatabase(const DurableDatabase&) = delete;
    DurableDatabase& operator=(const DurableDatabase&) = delete;

    struct Row {
      KvKey key;
      KvValue value;
      int encoding = 1;
      std::uint64_t version = 0;
      double expirationMs = -1;
    };

    std::optional<Row> row(const KvKey& key) const {
      for (Row& found : select("SELECT k, v, v_encoding, version, expiration_ms FROM kv WHERE k = ?", packKey(key))) return std::move(found);
      return std::nullopt;
    }
    std::vector<Row> rows() const { return select("SELECT k, v, v_encoding, version, expiration_ms FROM kv ORDER BY k", std::nullopt); }
    std::uint64_t version() const {
      const sqlite::Database database = open();
      const std::int64_t value = *sqlite::getBigInt(database.query("SELECT version FROM data_version WHERE k = 0").get()->get(std::string_view("version")));
      database.close();
      return static_cast<std::uint64_t>(value);
    }

    // What a handle on this database is opened with.
    SqliteKvOptions handleOptions() const {
      SqliteKvOptions options;
      options.now = now_;
      options.versionIncrement = options_.versionIncrement ? options_.versionIncrement : std::function<std::uint64_t()>([] { return std::uint64_t{1}; });
      options.sweepIntervalMs = options_.sweepIntervalMs;
      options.sweepJitterMs = 0;
      return options;
    }
    const std::string& file() const { return file_; }

  private:
    sqlite::Database open() const {
      sqlite::DatabaseOptions options;
      options.readonly = true;
      options.safeIntegers = true;
      return sqlite::Database(file_, options);
    }

    std::vector<Row> select(std::string_view sql, std::optional<KvBytes> key) const {
      std::vector<Row> out;
      if (!fs::existsSync(file_)) return out;
      const sqlite::Database database = open();
      const sqlite::Statement statement = database.query(sql);
      const std::vector<sqlite::Row> found = key ? statement.all(*key) : statement.all();
      for (const sqlite::Row& row : found) {
        const sqlite::Bytes& bytes = *sqlite::getBytes(row.get(std::string_view("v")));
        const auto encoding = static_cast<int>(*sqlite::getBigInt(row.get(std::string_view("v_encoding"))));
        KvValue value;
        if (encoding == 1) value = kvJscDeserialize(bytes);
        else if (encoding == 3) value = KvValue(bytes);
        else {
          std::uint64_t counter = 0;
          for (std::size_t index = 0; index < 8; ++index) counter |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
          value = KvValue(KvU64(counter));
        }
        out.push_back(Row{unpackKey(*sqlite::getBytes(row.get(std::string_view("k")))), std::move(value), encoding,
                          static_cast<std::uint64_t>(*sqlite::getBigInt(row.get(std::string_view("version")))),
                          static_cast<double>(*sqlite::getBigInt(row.get(std::string_view("expiration_ms"))))});
      }
      database.close();
      return out;
    }

    std::function<double()> now_;
    DurableOptions options_;
    std::string directory_;
    std::string file_;
  };

  // Holds the guarded store before ForwardingKv binds to it.
  struct GuardedStore {
    std::unique_ptr<Kv> guarded;
  };

  // MemoryKv for the durable store: a handle (`openKv(path)`) on its database.
  class DurableKv final : private GuardedStore, public ForwardingKv {
  public:
    explicit DurableKv(std::function<double()> now, DurableOptions options = {}) : DurableKv(std::make_shared<DurableDatabase>(std::move(now), std::move(options))) {}
    explicit DurableKv(std::shared_ptr<DurableDatabase> database)
        : GuardedStore{storage::native_kv::guardNativeKv(std::make_shared<SqliteKv>(database->file(), database->handleOptions()))},
          ForwardingKv(*guarded),
          database_(std::move(database)) {}
    const std::shared_ptr<DurableDatabase>& database() const noexcept { return database_; }

  private:
    std::shared_ptr<DurableDatabase> database_;
  };
}

namespace {
  constexpr double INF = std::numeric_limits<double>::infinity();
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();

  double fakeNow = 1'700'000'000'000.0;
  std::function<double()> testClock() {
    return [] { return fakeNow; };
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

  template <class F> std::string thrown(F&& function) {
    try {
      function();
    } catch (const JsError& error) {
      return error.toString();
    }
    return "returned";
  }

  std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
      out += digits[byte >> 4];
      out += digits[byte & 15];
    }
    return out;
  }

  KvBytes unhex(std::string_view text) {
    KvBytes out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(index, 2)), nullptr, 16)));
    return out;
  }

  std::vector<KvValue> valuesOf(const std::vector<KvEntry<>>& entries) {
    std::vector<KvValue> out;
    for (const auto& entry : entries) out.push_back(entry.value);
    return out;
  }

  std::string base64(std::string_view text) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t index = 0;
    for (; index + 2 < text.size(); index += 3) {
      const unsigned chunk = (static_cast<unsigned>(static_cast<unsigned char>(text[index])) << 16) | (static_cast<unsigned>(static_cast<unsigned char>(text[index + 1])) << 8) | static_cast<unsigned>(static_cast<unsigned char>(text[index + 2]));
      for (int shift = 18; shift >= 0; shift -= 6) out += alphabet[(chunk >> shift) & 63];
    }
    if (text.size() - index == 1) {
      const unsigned chunk = static_cast<unsigned>(static_cast<unsigned char>(text[index])) << 16;
      out += alphabet[(chunk >> 18) & 63];
      out += alphabet[(chunk >> 12) & 63];
      out += "==";
    } else if (text.size() - index == 2) {
      const unsigned chunk = (static_cast<unsigned>(static_cast<unsigned char>(text[index])) << 16) | (static_cast<unsigned>(static_cast<unsigned char>(text[index + 1])) << 8);
      out += alphabet[(chunk >> 18) & 63];
      out += alphabet[(chunk >> 12) & 63];
      out += alphabet[(chunk >> 6) & 63];
      out += '=';
    }
    return out;
  }

  // GCC 12.2 also destroys a braced aggregate temporary with non-trivial members
  // twice when it sits in a co_await expression, so tests compare against names.
  const KvCommitResult conflict{false, ""};

  KvValue u64Object(std::uint64_t value) { return KvValue::object({{"value", BigInt::fromUint64(value)}}); }

  // Builders without std::initializer_list: GCC 12.2 cannot keep a braced list's
  // array alive across co_await, so coroutine tests build values with these.
  template <class... T> std::vector<KvValue> vals(T&&... items) {
    std::vector<KvValue> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  template <class... T> KvBytes bytes(T... items) { return KvBytes{static_cast<std::uint8_t>(items)...}; }
  template <class... T> KvValue arr(T&&... items) {
    KvArray out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  inline void objSet(KvObject&) {}
  template <class K, class V, class... Rest> void objSet(KvObject& object, K&& key, V&& value, Rest&&... rest) {
    object.set(std::string(std::forward<K>(key)), KvValue(std::forward<V>(value)));
    objSet(object, std::forward<Rest>(rest)...);
  }
  template <class... T> KvValue obj(T&&... items) {
    KvObject object;
    objSet(object, std::forward<T>(items)...);
    return object;
  }
  template <class... T> std::vector<KvKey> keys(T&&... items) {
    std::vector<KvKey> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  template <class... T> std::vector<AtomicCheck> checks(T&&... items) {
    std::vector<AtomicCheck> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
}

ASYNC_TEST("durable: native Bun SQLite retains typed values and KvU64 identity") {
  DurableKv kv(testClock());
  const KvValue value = KvValue::object({{"bytes", KvBytes{0, 128, 255}},
                                         {"amount", BigInt::fromString("1208925819614629174706176")},
                                         {"empty", KvValue()},
                                         {"nested", KvValue::array({nullptr, true, "λ"})}});
  const KvCommitResult committed = co_await kv.set({"typed"}, value);
  EXPECT(committed.ok);
  EXPECT_EQ((co_await kv.get({"typed"})).value, value);
  co_await kv.set({"counter"}, KvU64(12));
  const KvValue counter = (co_await kv.get({"counter"})).value;
  EXPECT(counter.isU64());
  EXPECT_EQ(counter.asU64().value, 12u);
  co_await kv.set({"undefined"}, KvValue());
  const KvEntryMaybe<> stored = co_await kv.get({"undefined"});
  EXPECT(stored.value.isUndefined());
  EXPECT(stored.versionstamp.has_value());
}

ASYNC_TEST("durable: atomic compare-and-set refuses stale checks without partial writes") {
  DurableKv kv(testClock());
  const KvEntryMaybe<> absent = co_await kv.get({"quota"});
  auto operation = kv.atomic();
  operation->check(absent).set({"quota"}, KvValue::object({{"reserved", 1}})).set({"audit"}, "created");
  EXPECT((co_await operation->commit()).ok);
  const auto entries = co_await kv.getMany(keys(KvKey{"quota"}, KvKey{"audit"}));
  EXPECT_EQ(entries[0].versionstamp, entries[1].versionstamp);
  auto stale = kv.atomic();
  stale->check(absent).set({"quota"}, KvValue::object({{"reserved", 2}})).set({"partial"}, true);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv.get({"partial"})).versionstamp);
  EXPECT_EQ(entries[0].value, KvValue::object({{"reserved", 1}}));
}

ASYNC_TEST("durable: native check budget preserves paired analytics sums with two and ten caller checks") {
  DurableKv kv(testClock());
  const std::vector<std::pair<std::string, std::int64_t>> counters{
    {"sample_count", 1}, {"input_tokens", 100}, {"cached_input_tokens", 25}, {"usage_reported_sample_count", 1}, {"request_cache_hit_sample_count", 1}};
  const std::vector<int> checkCounts{2, 10};
  for (const int checkCount : checkCounts) {
    const std::string prefix = "analytics-" + std::to_string(checkCount);
    std::vector<KvKey> fenceKeys;
    for (int index = 0; index < checkCount; ++index) fenceKeys.push_back({prefix, "fence", index});
    const auto fences = co_await kv.getMany(fenceKeys);
    auto operation = kv.atomic();
    operation->check(fences).set({prefix, "marker"}, true).set({prefix, "cardinality"}, 1);
    for (const auto& [counter, amount] : counters) operation->sum({prefix, "dimension", counter}, amount).sum({prefix, "aggregate", counter}, amount);
    EXPECT((co_await operation->commit()).ok);
    for (const auto& [counter, amount] : counters) {
      EXPECT_EQ((co_await kv.get({prefix, "dimension", counter})).value, KvValue(KvU64(amount)));
      EXPECT_EQ((co_await kv.get({prefix, "aggregate", counter})).value, KvValue(KvU64(amount)));
    }
  }
}

ASYNC_TEST("durable: maximum caller checks retain stale CAS and late wrong-type rollback without partial counters") {
  DurableKv kv(testClock());
  std::vector<KvKey> keys;
  for (int index = 0; index < 10; ++index) keys.push_back({"max-fence", index});
  const auto fences = co_await kv.getMany(keys);
  co_await kv.set(keys[9], "changed");
  auto stale = kv.atomic();
  stale->check(fences).set({"stale-marker"}, true).sum({"stale-sum"}, 1);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv.get({"stale-marker"})).versionstamp);
  EXPECT(!(co_await kv.get({"stale-sum"})).versionstamp);
  const auto current = co_await kv.getMany(keys);
  co_await kv.set({"late-wrong-type"}, BigInt(7));
  auto failed = kv.atomic();
  failed->check(current).set({"late-marker"}, true);
  for (int index = 0; index < 9; index += 1) failed->sum({"late-sum", index}, 1);
  failed->sum({"late-wrong-type"}, 1);
  EXPECT_EQ(co_await outcome(failed->commit()), std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT(!(co_await kv.get({"late-marker"})).versionstamp);
  for (int index = 0; index < 9; index += 1) EXPECT(!(co_await kv.get({"late-sum", index})).versionstamp);
  EXPECT_EQ((co_await kv.get({"late-wrong-type"})).value, KvValue(BigInt(7)));
}

namespace {
  struct CountingFacade : ForwardingKv {
    using ForwardingKv::ForwardingKv;
    int lists = 0;
    int atomics = 0;
    std::unique_ptr<KvListIterator> list(KvListSelector selector, KvListOptions options = {}) override {
      lists += 1;
      return ForwardingKv::list(std::move(selector), std::move(options));
    }
    std::unique_ptr<AtomicOperation> atomic() override {
      atomics += 1;
      return ForwardingKv::atomic();
    }
  };
}

ASYNC_TEST("durable: native facade permits captured list and atomic instance overrides without recursion") {
  DurableKv native(testClock());
  CountingFacade kv(native);
  co_await kv.set({"facade"}, "root");
  co_await kv.set({"facade", 1}, "child");
  auto iterator = kv.list({.prefix = KvKey{"facade"}}, {.limit = 1});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*iterator)), vals("child"));
  EXPECT_EQ(kv.lists, 1);
  const KvEntryMaybe<> absent = co_await kv.get({"facade-claim"});
  auto claim = kv.atomic();
  claim->check(absent).set({"facade-claim"}, "owned");
  EXPECT((co_await claim->commit()).ok);
  auto stale = kv.atomic();
  stale->check(absent).set({"facade-stale"}, true);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv.get({"facade-stale"})).versionstamp);
  co_await kv.set({"facade-wrong-sum"}, BigInt(3));
  auto partial = kv.atomic();
  partial->set({"facade-partial"}, true).sum({"facade-wrong-sum"}, 1);
  EXPECT((co_await outcome(partial->commit())).find("KvU64") != std::string::npos);
  EXPECT(!(co_await kv.get({"facade-partial"})).versionstamp);
  KvError error;
  error.message = "fixture";
  error.cause = std::make_shared<KvValue>("unsupported");
  auto valuePartial = kv.atomic();
  valuePartial->set({"facade-value-partial"}, true).set({"facade-value"}, error);
  EXPECT((co_await outcome(valuePartial->commit())).find("Error.cause") != std::string::npos);
  EXPECT(!(co_await kv.get({"facade-value-partial"})).versionstamp);
  EXPECT_EQ(kv.atomics, 4);
}

ASYNC_TEST("durable: native TTL retains its absolute deadline and expires across restart") {
  auto database = std::make_shared<DurableDatabase>(testClock());
  DurableKv kv(database);
  const double before = fakeNow;
  co_await kv.set({"expiring"}, "challenge", {.expireIn = 20});
  const double after = fakeNow;
  co_await kv.set({"durable"}, "ledger");
  const double expiry = database->row({"expiring"})->expirationMs;
  EXPECT(expiry >= before + 20);
  EXPECT(expiry <= after + 20);
  EXPECT_EQ(database->row({"durable"})->expirationMs, -1.0);
  // Native TTL permits delayed visibility; startup sweeps expired rows directly.
  kv.close();
  fakeNow = after + 25;
  DurableKv reopened(database);
  EXPECT(!(co_await reopened.get({"expiring"})).versionstamp);
  EXPECT_EQ((co_await reopened.get({"durable"})).value, KvValue("ledger"));
}

ASYNC_TEST("durable: native list preserves heterogeneous key order and resumable ranges") {
  DurableKv kv(testClock());
  const std::vector<KvKeyPart> parts{KvBytes{0}, KvBytes{2}, KvBytes{10}, "a", "é", BigInt(-2), BigInt(0), BigInt(2), -2, 0, 2, false, true};
  std::vector<KvValue> indexes;
  for (std::size_t index = 0; index < parts.size(); index += 1) {
    co_await kv.set({"ordered", parts[index]}, index);
    indexes.emplace_back(index);
  }
  auto all = kv.list({.prefix = KvKey{"ordered"}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*all)), indexes);
  auto first = kv.list({.prefix = KvKey{"ordered"}}, {.limit = 3, .batchSize = 2});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*first)), vals(0, 1, 2));
  auto resumed = kv.list({.prefix = KvKey{"ordered"}}, {.cursor = first->cursor()});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*resumed)), std::vector<KvValue>(indexes.begin() + 3, indexes.end()));
  auto range = kv.list({.start = KvKey{"ordered", -2}, .end = KvKey{"ordered", 2}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*range)), vals(8, 9));
  auto reverse = kv.list({.prefix = KvKey{"ordered"}}, {.limit = 2, .reverse = true});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*reverse)), vals(12, 11));
}

ASYNC_TEST("durable: prefix selectors exclude the exact prefix and native cursors retain cross-range behavior") {
  DurableKv kv(testClock());
  co_await kv.set({"prefix"}, "root");
  co_await kv.set({"prefix", 1}, "child");
  co_await kv.set({"prefix", 2}, "next");
  auto first = kv.list({.prefix = KvKey{"prefix"}}, {.limit = 1});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*first)), vals("child"));
  auto other = kv.list({.prefix = KvKey{"other"}}, {.cursor = first->cursor()});
  EXPECT((co_await kvListToArray(*other)).empty());
  EXPECT_EQ(thrown([&] { (void)kv.list({.prefix = KvKey{"prefix"}, .end = KvKey{"outside", 1}}); }),
            std::string("TypeError: End key is not in the keyspace defined by prefix"));
  auto bounded = kv.list({.prefix = KvKey{"prefix"}, .end = KvKey{"prefix", 2}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*bounded)), vals("child"));
}

ASYNC_TEST("durable: native cursor lifecycle clears empty and exhausted scans but preserves limited continuations") {
  DurableKv kv(testClock());
  auto empty = kv.list({.prefix = KvKey{"empty-cursor"}});
  EXPECT_EQ(thrown([&] { (void)empty->cursor(); }), std::string("Error: Cannot get cursor before first iteration"));
  EXPECT((co_await kvListToArray(*empty)).empty());
  EXPECT_EQ(empty->cursor(), std::string(""));
  co_await kv.set({"cursor", 1}, "first");
  auto single = kv.list({.prefix = KvKey{"cursor"}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*single)), vals("first"));
  EXPECT_EQ(single->cursor(), std::string(""));
  co_await kv.set({"cursor", 2}, "second");
  auto limited = kv.list({.prefix = KvKey{"cursor"}}, {.limit = 1});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*limited)), vals("first"));
  const std::string continuation = limited->cursor();
  EXPECT(continuation != "");
  auto resumed = kv.list({.prefix = KvKey{"cursor"}}, {.cursor = continuation});
  EXPECT_EQ(resumed->cursor(), continuation);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*resumed)), vals("second"));
  EXPECT_EQ(resumed->cursor(), std::string(""));
}

ASYNC_TEST("durable: unsigned sums wrap, retain real U64 values, and roll back wrong-type mutations") {
  DurableKv kv(testClock());
  co_await kv.set({"sum"}, KvU64(std::numeric_limits<std::uint64_t>::max()));
  auto wrap = kv.atomic();
  wrap->sum({"sum"}, 1);
  EXPECT((co_await wrap->commit()).ok);
  EXPECT_EQ((co_await kv.get({"sum"})).value, KvValue(KvU64(0)));
  auto missing = kv.atomic();
  missing->sum({"missing-sum"}, 5);
  co_await missing->commit();
  EXPECT((co_await kv.get({"missing-sum"})).value.isU64());
  co_await kv.set({"wrong-type"}, BigInt(5));
  auto rollback = kv.atomic();
  rollback->set({"must-roll-back"}, "partial").sum({"wrong-type"}, 1);
  EXPECT_EQ(co_await outcome(rollback->commit()), std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT(!(co_await kv.get({"must-roll-back"})).versionstamp);
  EXPECT_EQ((co_await kv.get({"wrong-type"})).value, KvValue(BigInt(5)));
}

ASYNC_TEST("durable: snapshot reads preserve absent values and clone caller-owned buffers") {
  DurableKv kv(testClock());
  KvBytes owned{1, 2, 3};
  co_await kv.set({"bytes"}, owned);
  owned[0] = 99;
  auto entries = co_await kv.getMany(keys(KvKey{"bytes"}, KvKey{"missing"}));
  EXPECT_EQ(entries[0].value, KvValue(KvBytes{1, 2, 3}));
  EXPECT(entries[1] == (KvEntryMaybe<>{{"missing"}, nullptr, std::nullopt}));
  entries[0].value.as<KvBytes>()[1] = 99;
  EXPECT_EQ((co_await kv.get({"bytes"})).value, KvValue(bytes(1, 2, 3)));
}

ASYNC_TEST("durable: concurrent native arithmetic retains updates while caller CAS failure remains final") {
  DurableKv kv(testClock());
  co_await kv.set({"guard-counter"}, KvU64(0));
  auto first = kv.atomic();
  first->sum({"guard-counter"}, 1);
  auto second = kv.atomic();
  second->sum({"guard-counter"}, 1);
  const auto committed = co_await (first->commit() && second->commit());
  const auto& one = std::get<0>(committed);
  const auto& two = std::get<1>(committed);
  EXPECT(one.ok && two.ok);
  EXPECT_EQ((co_await kv.get({"guard-counter"})).value, KvValue(KvU64(2)));
  const KvEntryMaybe<> stale = co_await kv.get({"caller-fence"});
  co_await kv.set({"caller-fence"}, true);
  auto fenced = kv.atomic();
  fenced->check(stale).sum({"guard-counter"}, 1);
  EXPECT_EQ(co_await fenced->commit(), conflict);
  EXPECT_EQ((co_await kv.get({"guard-counter"})).value, KvValue(KvU64(2)));
}

ASYNC_TEST("durable: numeric key order distinguishes signed zeros and retains infinity and canonical NaN") {
  DurableKv kv(testClock());
  const std::vector<double> numbers{-INF, -2, -0.0, 0, 2, INF, NAN_};
  std::vector<KvValue> indexes;
  for (std::size_t index = 0; index < numbers.size(); index += 1) {
    co_await kv.set({"numeric", numbers[index]}, index);
    indexes.emplace_back(index);
  }
  auto iterator = kv.list({.prefix = KvKey{"numeric"}});
  const auto rows = co_await kvListToArray(*iterator);
  EXPECT_EQ(valuesOf(rows), indexes);
  EXPECT(std::signbit(rows[2].key[1].number()));
  EXPECT(!std::signbit(rows[3].key[1].number()));
  EXPECT(std::isnan(rows[6].key[1].number()));
}

ASYNC_TEST("durable: Error cause graphs fail before any atomic mutation can lose persisted fields") {
  DurableKv kv(testClock());
  KvError inner;
  inner.message = "inner";
  KvError error;
  error.message = "outer";
  error.cause = std::make_shared<KvValue>(inner);
  const KvValue graph = KvValue::object({{"records", KvMap{{"failure", error}}}});
  auto operation = kv.atomic();
  operation->set({"cause-partial"}, "must not publish").set({"cause"}, graph);
  EXPECT_EQ(co_await outcome(operation->commit()), std::string("TypeError: Bun KV cannot preserve Error.cause; persist an explicit error record instead"));
  EXPECT(!(co_await kv.get({"cause-partial"})).versionstamp);
  EXPECT(!(co_await kv.get({"cause"})).versionstamp);
}

ASYNC_TEST("durable: two native handles share CAS fencing and monotonic persisted revisions") {
  auto database = std::make_shared<DurableDatabase>(testClock());
  DurableKv first(database);
  DurableKv second(database);
  const KvEntryMaybe<> absent = co_await first.get({"claim"});
  auto a = first.atomic();
  a->check(absent).set({"claim"}, "first");
  auto b = second.atomic();
  b->check(absent).set({"claim"}, "second");
  const auto committed = co_await (a->commit() && b->commit());
  const auto& ra = std::get<0>(committed);
  const auto& rb = std::get<1>(committed);
  EXPECT_EQ(static_cast<int>(ra.ok) + static_cast<int>(rb.ok), 1);
  const KvEntryMaybe<> before = co_await first.get({"claim"});
  co_await second.delete_({"claim"});
  co_await second.set({"claim"}, "replacement");
  const KvEntryMaybe<> after = co_await first.get({"claim"});
  EXPECT(after.versionstamp != before.versionstamp);
  EXPECT(after.versionstamp.value_or("") > before.versionstamp.value_or(""));
  auto staleOwner = first.atomic();
  staleOwner->check(before).set({"stale-owner"}, true);
  EXPECT_EQ(co_await staleOwner->commit(), conflict);
}

ASYNC_TEST("durable: two native handles preserve ten caller checks under mixed counter-type updates") {
  auto database = std::make_shared<DurableDatabase>(testClock());
  DurableKv first(database);
  DurableKv second(database);
  std::vector<KvKey> fenceKeys;
  for (int index = 0; index < 10; ++index) fenceKeys.push_back({"mixed-fence", index});
  const auto fences = co_await first.getMany(fenceKeys);
  co_await first.set({"mixed-counter"}, KvU64(0));
  auto writer = [&]() -> Task<void> {
    for (int index = 0; index < 40; index += 1) {
      co_await second.set({"mixed-counter"}, BigInt(3));
      co_await second.set({"mixed-counter"}, KvU64(0));
    }
  };
  std::vector<bool> outcomes;
  auto committer = [&]() -> Task<void> {
    for (int index = 0; index < 20; index += 1) {
      auto operation = first.atomic();
      operation->check(fences).set({"mixed-marker", index}, true).sum({"mixed-side", index}, 1).sum({"mixed-counter"}, 1);
      bool ok = false;
      try {
        const KvCommitResult result = co_await operation->commit();
        EXPECT(result.ok);
        ok = true;
      } catch (const TypeError& error) {
        if (error.message.find("KvU64") == std::string::npos) throw;
      }
      outcomes.push_back(ok);
    }
  };
  co_await (writer() && committer());
  EXPECT_EQ(outcomes.size(), 20u);
  for (std::size_t index = 0; index < outcomes.size(); index += 1) {
    EXPECT_EQ((co_await first.get({"mixed-marker", index})).value, outcomes[index] ? KvValue(true) : KvValue(nullptr));
    EXPECT_EQ((co_await first.get({"mixed-side", index})).value, outcomes[index] ? KvValue(KvU64(1)) : KvValue(nullptr));
  }
}

// ---- DurableKv: oracle-pinned semantics ---------------------------------------------------------

ASYNC_TEST("durable: every commit takes the next version, empty and check-only commits included") {
  DurableKv kv(testClock());
  std::vector<std::string> stamps;
  for (int index = 0; index < 12; index += 1) stamps.push_back((co_await kv.set({"v", index}, index)).versionstamp);
  EXPECT_EQ(stamps.front(), std::string("00000000000000010000"));
  EXPECT_EQ(stamps.back(), std::string("000000000000000c0000"));
  auto multi = kv.atomic();
  multi->set({"m", 1}, 1).set({"m", 2}, 2).delete_({"v", 0});
  const KvCommitResult result = co_await multi->commit();
  EXPECT_EQ(result.versionstamp, std::string("000000000000000d0000"));
  EXPECT_EQ((co_await kv.get({"m", 1})).versionstamp, std::optional<std::string>(result.versionstamp));
  EXPECT_EQ((co_await kv.get({"m", 2})).versionstamp, std::optional<std::string>(result.versionstamp));
  const KvCommitResult fourteenth{true, "000000000000000e0000"};
  EXPECT_EQ((co_await kv.atomic()->commit()), fourteenth);
  EXPECT_EQ((co_await kv.atomic()->commit()).versionstamp, std::string("000000000000000f0000"));
  co_await kv.delete_({"nope"});
  EXPECT_EQ((co_await kv.set({"after"}, 1)).versionstamp, std::string("00000000000000110000"));
  auto checkOnly = kv.atomic();
  checkOnly->check(co_await kv.get({"after"}));
  EXPECT_EQ((co_await checkOnly->commit()).versionstamp, std::string("00000000000000120000"));
  EXPECT_EQ(kv.database()->version(), 0x12u);
}

ASYNC_TEST("durable: versionIncrement models the native random step") {
  const std::vector<std::uint64_t> steps{2, 9, 5};
  std::size_t next = 0;
  DurableKv kv(testClock(), DurableOptions{[&] { return steps[next++ % steps.size()]; }});
  std::vector<std::string> stamps;
  for (int index = 0; index < 3; index += 1) stamps.push_back((co_await kv.set({"v", index}, index)).versionstamp);
  EXPECT_EQ(stamps, (std::vector<std::string>{"00000000000000020000", "000000000000000b0000", "00000000000000100000"}));
}

ASYNC_TEST("durable: atomic checks follow the native versionstamp rules") {
  DurableKv kv(testClock());
  for (int index = 0; index < 12; index += 1) co_await kv.set({"warm"}, index);
  const auto commitChecks = [&](std::vector<AtomicCheck> checks, std::optional<KvValue> value) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    operation->check(std::move(checks));
    if (value) operation->set({"a"}, *value);
    co_return co_await operation->commit();
  };
  EXPECT((co_await commitChecks(checks(AtomicCheck{{"a"}, std::nullopt}), KvValue(1))).ok);
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"a"}, std::nullopt}), KvValue(2)), conflict);
  const KvEntryMaybe<> a = co_await kv.get({"a"});
  EXPECT((co_await commitChecks(checks(a), KvValue(3))).ok);
  EXPECT_EQ(co_await commitChecks(checks(a), KvValue(4)), conflict);
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"absent"}, "00000000000000010000"}), std::nullopt), conflict);
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "xyz"}), std::nullopt)), std::string("TypeError: Invalid byte 'x'"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "abc"}), std::nullopt)), std::string("RangeError: Odd length hex string"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "é0"}), std::nullopt)), std::string("TypeError: Invalid byte 'Ã'"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "0001"}), std::nullopt)), std::string("Error: InvalidVersionstamp"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "000000000000000000000000"}), std::nullopt)), std::string("Error: InvalidVersionstamp"));
  std::string current = *(co_await kv.get({"a"})).versionstamp;
  EXPECT(current.find_first_of("abcdef") != std::string::npos);
  std::string upper = current;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
  EXPECT((co_await commitChecks(checks(AtomicCheck{{"a"}, upper}), std::nullopt)).ok);
  EXPECT((co_await commitChecks(checks(AtomicCheck{{"es"}, ""}), std::nullopt)).ok);
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"a"}, ""}), std::nullopt), conflict);
  current = *(co_await kv.get({"a"})).versionstamp;
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"a"}, current.substr(0, 16) + "0001"}), std::nullopt), conflict);
  EXPECT((co_await commitChecks(checks(AtomicCheck{KvKey{}, std::nullopt}), std::nullopt)).ok);
  std::vector<AtomicCheck> ten;
  for (int index = 0; index < 10; index += 1) ten.push_back(AtomicCheck{{"c", index}, std::nullopt});
  EXPECT((co_await commitChecks(ten, std::nullopt)).ok);
  std::vector<AtomicCheck> eleven = ten;
  eleven.push_back(AtomicCheck{{"c", 10}, std::nullopt});
  EXPECT_EQ(co_await outcome(commitChecks(eleven, std::nullopt)), std::string("Error: TooManyChecks"));
  eleven.back() = AtomicCheck{{"a"}, std::nullopt};
  EXPECT_EQ(co_await outcome(commitChecks(eleven, std::nullopt)), std::string("Error: TooManyChecks"));
}

ASYNC_TEST("durable: mutations apply in order inside one all-or-nothing commit") {
  DurableKv kv(testClock());
  const auto commitOf = [&](const std::function<void(AtomicOperation&)>& build) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    build(*operation);
    co_return co_await operation->commit();
  };
  const auto many = [](int count) {
    return [count](AtomicOperation& operation) {
      for (int index = 0; index < count; index += 1) operation.set({"mm", index}, index);
    };
  };
  EXPECT((co_await commitOf(many(1000))).ok);
  EXPECT_EQ(co_await outcome(commitOf(many(1001))), std::string("Error: TooManyMutations"));
  co_await commitOf([](AtomicOperation& operation) { operation.set({"dup"}, 1).set({"dup"}, 2); });
  EXPECT_EQ((co_await kv.get({"dup"})).value, KvValue(2));
  co_await commitOf([](AtomicOperation& operation) { operation.set({"sd"}, 1).delete_({"sd"}); });
  EXPECT(!(co_await kv.get({"sd"})).versionstamp);
  co_await commitOf([](AtomicOperation& operation) { operation.delete_({"ds"}).set({"ds"}, 5); });
  EXPECT_EQ((co_await kv.get({"ds"})).value, KvValue(5));
  co_await commitOf([](AtomicOperation& operation) { operation.sum({"ss"}, 2).set({"ss"}, "x"); });
  EXPECT_EQ((co_await kv.get({"ss"})).value, KvValue("x"));
  try {
    co_await commitOf([](AtomicOperation& operation) { operation.set({"ts"}, "x").sum({"ts"}, 2); });
    EXPECT(false);
  } catch (const TypeError& error) {
    EXPECT_EQ(error.message, std::string("Atomic arithmetic requires an absent key or KvU64 value"));
    ASSERT(error.cause != nullptr);
    EXPECT_EQ(error.cause->toString(), std::string("Error: Failed to perform 'sum' mutation on a non-U64 value in the database"));
  }
  EXPECT(!(co_await kv.get({"ts"})).versionstamp);
  co_await commitOf([](AtomicOperation& operation) { operation.sum({"s2"}, 2).sum({"s2"}, 3); });
  EXPECT_EQ((co_await kv.get({"s2"})).value, KvValue(KvU64(5)));
  co_await commitOf([](AtomicOperation& operation) { operation.set({"s3"}, KvU64(10)).sum({"s3"}, 3); });
  EXPECT_EQ((co_await kv.get({"s3"})).value, KvValue(KvU64(13)));
  co_await commitOf([](AtomicOperation& operation) { operation.min({"minabs"}, 7); });
  EXPECT_EQ((co_await kv.get({"minabs"})).value, KvValue(KvU64(7)));
  co_await commitOf([](AtomicOperation& operation) { operation.max({"maxabs"}, 7); });
  EXPECT_EQ((co_await kv.get({"maxabs"})).value, KvValue(KvU64(7)));
  co_await kv.set({"u"}, KvU64(10));
  co_await commitOf([](AtomicOperation& operation) { operation.min({"u"}, 3); });
  EXPECT_EQ((co_await kv.get({"u"})).value, KvValue(KvU64(3)));
  co_await commitOf([](AtomicOperation& operation) { operation.max({"u"}, 30); });
  EXPECT_EQ((co_await kv.get({"u"})).value, KvValue(KvU64(30)));
  const KvCommitResult smaller = co_await commitOf([](AtomicOperation& operation) { operation.max({"u"}, 1); });
  const KvEntryMaybe<> unchanged = co_await kv.get({"u"});
  EXPECT_EQ(unchanged.value, KvValue(KvU64(30)));
  EXPECT_EQ(unchanged.versionstamp, std::optional<std::string>(smaller.versionstamp));
  co_await kv.set({"w"}, KvU64(std::numeric_limits<std::uint64_t>::max() - 1));
  co_await commitOf([](AtomicOperation& operation) { operation.sum({"w"}, 5); });
  EXPECT_EQ((co_await kv.get({"w"})).value, KvValue(KvU64(3)));
  co_await kv.set({"num"}, 5);
  co_await kv.set({"big"}, BigInt(5));
  co_await kv.set({"bytes"}, bytes(1, 2, 3, 4, 5, 6, 7, 8));
  const std::vector<const char*> wrongTypes{"num", "big", "bytes"};
  for (const char* key : wrongTypes)
    EXPECT_EQ(co_await outcome(commitOf([key](AtomicOperation& operation) { operation.sum({key}, 1); })),
              std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT_EQ(co_await outcome(commitOf([](AtomicOperation& operation) { operation.min({"num"}, 1); })),
            std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT_EQ(co_await outcome(commitOf([](AtomicOperation& operation) { operation.max({"num"}, 1); })),
            std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT_EQ(co_await outcome(commitOf([](AtomicOperation& operation) { operation.mutate({KvMutation{{"ms"}, KvMutationType::sum, 5, std::nullopt}}); })),
            std::string("Error: Some of the parameters are not valid V8 values"));
  co_await commitOf([](AtomicOperation& operation) { operation.mutate({KvMutation{{"mm4"}, KvMutationType::min, KvU64(4), std::nullopt}}); });
  EXPECT_EQ((co_await kv.get({"mm4"})).value, KvValue(KvU64(4)));
}

ASYNC_TEST("durable: builder validation throws synchronously like the SDK") {
  DurableKv kv(testClock());
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = 0}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = 1.5}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 1.5"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = -5}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found -5"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = NAN_}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found NaN"));
  EXPECT_EQ(co_await outcome(kv.set({"e"}, 1, {.expireIn = 0})), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(co_await outcome(kv.set({}, 1)), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(co_await outcome(kv.get({})), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(co_await outcome(kv.delete_({})), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] { kv.atomic()->delete_({}); }), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] {
              kv.atomic()->mutate({KvMutation{{"me"}, KvMutationType::set, 1, 0.0}, KvMutation{{}, KvMutationType::delete_, KvValue(), std::nullopt}});
            }),
            std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] { kv.atomic()->mutate({KvMutation{{"me"}, KvMutationType::set, 1, 0.0}}); }),
            std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({}, 1, {.expireIn = 0}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(thrown([&] { kv.atomic()->sum({}, -1); }), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] { kv.atomic()->sum({"x"}, -1); }), std::string("Error: value must be a positive bigint"));
  EXPECT_EQ(thrown([&] { kv.atomic()->min({"x"}, BigInt(1) << BigInt(64)); }), std::string("Error: value must fit in a 64-bit unsigned integer"));
  EXPECT_EQ(thrown([&] { kv.atomic()->max({"x"}, std::numeric_limits<std::uint64_t>::max()); }), std::string("returned"));
}

ASYNC_TEST("durable: getMany follows the native read-range limit") {
  DurableKv kv(testClock());
  co_await kv.set({"a"}, 3);
  EXPECT((co_await kv.getMany({})).empty());
  const auto keysOf = [](int count) {
    std::vector<KvKey> keys;
    for (int index = 0; index < count; index += 1) keys.push_back({"gm", index});
    return keys;
  };
  EXPECT_EQ((co_await kv.getMany(keysOf(10))).size(), 10u);
  EXPECT_EQ(co_await outcome(kv.getMany(keysOf(11))), std::string("Error: TooManyReadRanges"));
  const auto duplicates = co_await kv.getMany(keys(KvKey{"a"}, KvKey{"a"}));
  EXPECT_EQ(duplicates[0].value, KvValue(3));
  EXPECT_EQ(duplicates[1].value, KvValue(3));
  EXPECT_EQ(co_await outcome(kv.getMany(keys(KvKey{"a"}, KvKey{}))), std::string("TypeError: Key cannot be empty"));
}

ASYNC_TEST("durable: key and value sizes are limited like the native store") {
  DurableKv kv(testClock());
  const auto keyOf = [](std::size_t bytes) { return KvKey{std::string(bytes - 2, 'k')}; };
  const std::string tooLong = "Error: KeyTooLong";
  const auto checkAbsent = [&](std::size_t bytes) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    operation->check(AtomicCheck{keyOf(bytes), std::nullopt});
    co_return co_await operation->commit();
  };
  const auto sumAt = [&](std::size_t bytes) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    operation->sum(keyOf(bytes), 1);
    co_return co_await operation->commit();
  };
  const auto listPrefix = [&](std::size_t bytes) -> Task<std::size_t> {
    auto iterator = kv.list({.prefix = keyOf(bytes)});
    co_return (co_await kvListToArray(*iterator)).size();
  };
  const auto listRange = [&](std::size_t bytes) -> Task<std::size_t> {
    auto iterator = kv.list({.start = keyOf(bytes), .end = KvKey{"z"}});
    co_return (co_await kvListToArray(*iterator)).size();
  };
  EXPECT((co_await kv.set(keyOf(2048), 1)).ok);
  EXPECT((co_await kv.get(keyOf(2048))).versionstamp.has_value());
  co_await kv.delete_(keyOf(2048));
  EXPECT((co_await checkAbsent(2048)).ok);
  EXPECT((co_await sumAt(2048)).ok);
  EXPECT_EQ(co_await outcome(listPrefix(2048)), tooLong);
  EXPECT_EQ(co_await listRange(2048), 1u);
  EXPECT_EQ((co_await kv.getMany(keys(keyOf(2048)))).size(), 1u);
  EXPECT_EQ(co_await outcome(kv.set(keyOf(2049), 1)), tooLong);
  EXPECT(!(co_await kv.get(keyOf(2049))).versionstamp);
  EXPECT_EQ(co_await outcome(kv.delete_(keyOf(2049))), tooLong);
  EXPECT((co_await checkAbsent(2049)).ok);
  EXPECT_EQ(co_await outcome(sumAt(2049)), tooLong);
  EXPECT_EQ(co_await outcome(listPrefix(2049)), tooLong);
  EXPECT_EQ(co_await listRange(2049), 0u);
  EXPECT_EQ((co_await kv.getMany(keys(keyOf(2049)))).size(), 1u);
  const std::vector<std::size_t> tooLongSizes{2050, 4096};
  for (const std::size_t size : tooLongSizes) {
    EXPECT_EQ(co_await outcome(kv.set(keyOf(size), 1)), tooLong);
    EXPECT_EQ(co_await outcome(kv.get(keyOf(size))), tooLong);
    EXPECT_EQ(co_await outcome(kv.delete_(keyOf(size))), tooLong);
    EXPECT_EQ(co_await outcome(checkAbsent(size)), tooLong);
    EXPECT_EQ(co_await outcome(sumAt(size)), tooLong);
    EXPECT_EQ(co_await outcome(listPrefix(size)), tooLong);
    EXPECT_EQ(co_await outcome(listRange(size)), tooLong);
    EXPECT_EQ(co_await outcome(kv.getMany(keys(keyOf(size)))), tooLong);
  }
  EXPECT((co_await kv.set({"bv"}, KvBytes(65536))).ok);
  EXPECT_EQ(co_await outcome(kv.set({"bv"}, KvBytes(65537))), std::string("Error: ValueTooLong"));
  // A string of n Latin-1 characters serializes to n + 9 bytes.
  EXPECT((co_await kv.set({"sv"}, std::string(65527, 's'))).ok);
  EXPECT_EQ(co_await outcome(kv.set({"sv"}, std::string(65528, 's'))), std::string("Error: ValueTooLong"));
}

ASYNC_TEST("durable: commit payloads are limited to 819200 bytes and validated in native order") {
  DurableKv kv(testClock());
  const auto keyOf = [](std::size_t bytes) { return KvKey{std::string(bytes - 2, 'k')}; };
  // Twelve 65536-byte values under 12-byte keys, then ["q"] (3 bytes) with `extra` bytes.
  const auto payload = [&](std::size_t extra, std::size_t checkBytes, std::optional<KvMutationType> tail) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    if (checkBytes) operation->check(AtomicCheck{keyOf(checkBytes), std::nullopt});
    for (int index = 0; index < 12; index += 1) operation->set({"p", index}, KvBytes(65536));
    operation->set({"q"}, KvBytes(extra));
    if (tail == KvMutationType::delete_) operation->delete_({"dd"});
    if (tail == KvMutationType::sum) operation->sum({"ss"}, 1);
    co_return co_await operation->commit();
  };
  const std::size_t base = 12 * (12 + 65536) + 3;
  const std::string tooLarge = "Error: AtomicWriteTooLarge";
  EXPECT((co_await payload(819200 - base, 0, std::nullopt)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base, 0, std::nullopt)), tooLarge);
  EXPECT((co_await payload(819200 - base - 2000, 2000, std::nullopt)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base - 2000, 2000, std::nullopt)), tooLarge);
  EXPECT((co_await payload(819200 - base - 4, 0, KvMutationType::delete_)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base - 4, 0, KvMutationType::delete_)), tooLarge);
  EXPECT((co_await payload(819200 - base - 4 - 8, 0, KvMutationType::sum)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base - 4 - 8, 0, KvMutationType::sum)), tooLarge);

  const auto checksOf = [&](int count, std::size_t keyBytes) {
    std::vector<AtomicCheck> checks;
    for (int index = 0; index < count; index += 1) {
      std::string name = "c" + std::to_string(index);
      if (keyBytes > name.size() + 2) name.resize(keyBytes - 2, 'c');
      checks.push_back(AtomicCheck{{name}, std::nullopt});
    }
    return checks;
  };
  const auto attempt = [&](const std::function<void(AtomicOperation&)>& build) -> Task<std::string> {
    auto operation = kv.atomic();
    build(*operation);
    co_return co_await outcome(operation->commit());
  };
  const auto manySets = [](AtomicOperation& operation) {
    for (int index = 0; index < 1001; index += 1) operation.set({"m", index}, 1);
  };
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { manySets(operation.check(checksOf(11, 2))); }), std::string("Error: TooManyChecks"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { manySets(operation.check(checksOf(10, 2050))); }), std::string("Error: TooManyMutations"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{keyOf(2050), "0001"}); }), std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{{"a"}, "0001"}, AtomicCheck{keyOf(2050), std::nullopt}); }),
            std::string("Error: InvalidVersionstamp"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{keyOf(2050), std::nullopt}).set(keyOf(2049), 1); }),
            std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{{"a"}, "0001"}).set(keyOf(2049), 1); }),
            std::string("Error: InvalidVersionstamp"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.set({"v"}, KvBytes(65537)).set(keyOf(2049), 1); }), std::string("Error: ValueTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.set(keyOf(2049), 1).set({"v"}, KvBytes(65537)); }), std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.set(keyOf(2049), KvBytes(65537)); }), std::string("Error: KeyTooLong"));
  KvError caused;
  caused.message = "x";
  caused.cause = std::make_shared<KvValue>(1);
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(checksOf(11, 2)).set({"x"}, caused); }),
            std::string("TypeError: Bun KV cannot preserve Error.cause; persist an explicit error record instead"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{{"a"}, "zz"}).set({"x"}, caused); }),
            std::string("TypeError: Invalid byte 'z'"));
}

ASYNC_TEST("durable: list pages, cursors and selectors follow the SDK") {
  DurableKv kv(testClock());
  co_await kv.set({"p"}, "root");
  for (int index = 1; index <= 7; index += 1) co_await kv.set({"p", index}, "c" + std::to_string(index));
  co_await kv.set({"p", 3, "x"}, "deep");
  co_await kv.set({"q", 1}, "other");
  const auto page = [&](KvListSelector selector, KvListOptions options) -> Task<std::pair<std::vector<KvValue>, std::string>> {
    auto iterator = kv.list(std::move(selector), std::move(options));
    std::vector<KvValue> values = valuesOf(co_await kvListToArray(*iterator));
    co_return std::pair{std::move(values), iterator->cursor()};
  };
  const KvListSelector p{.prefix = KvKey{"p"}};
  const KvListOptions limit3{.limit = 3};
  const auto firstPage = co_await page(p, limit3);
  const auto& page1 = std::get<0>(firstPage);
  const auto& cursor1 = std::get<1>(firstPage);
  EXPECT_EQ(page1, (std::vector<KvValue>{"c1", "c2", "c3"}));
  EXPECT_EQ(cursor1, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDA4MDAwMDAwMDAwMDAwIn0="));
  auto second = kv.list(p, {.limit = 3, .cursor = cursor1});
  EXPECT_EQ(second->cursor(), cursor1);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*second)), vals("deep", "c4", "c5"));
  const std::string cursor2 = second->cursor();
  EXPECT_EQ(cursor2, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDE0MDAwMDAwMDAwMDAwIn0="));
  const KvListOptions fromCursor2{.cursor = cursor2};
  const auto thirdPage = co_await page(p, fromCursor2);
  const auto& page3 = std::get<0>(thirdPage);
  const auto& cursor3 = std::get<1>(thirdPage);
  EXPECT_EQ(page3, (std::vector<KvValue>{"c6", "c7"}));
  EXPECT_EQ(cursor3, std::string(""));
  const KvListOptions reverse2{.limit = 2, .reverse = true};
  const auto reversePage = co_await page(p, reverse2);
  const auto& reversed = std::get<0>(reversePage);
  const auto& reverseCursor = std::get<1>(reversePage);
  EXPECT_EQ(reversed, (std::vector<KvValue>{"c7", "c6"}));
  EXPECT_EQ(reverseCursor, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDE4MDAwMDAwMDAwMDAwIn0="));
  const KvListOptions reverseFromCursor{.cursor = reverseCursor, .reverse = true};
  const auto reverseRemainingPage = co_await page(p, reverseFromCursor);
  const auto& reversed2 = std::get<0>(reverseRemainingPage);
  const auto& reverseCursor2 = std::get<1>(reverseRemainingPage);
  EXPECT_EQ(reversed2, (std::vector<KvValue>{"c5", "c4", "deep", "c3", "c2", "c1"}));
  EXPECT_EQ(reverseCursor2, std::string(""));
  const std::vector<KvValue> children{"c1", "c2", "c3", "deep", "c4", "c5", "c6", "c7"};
  const KvListOptions none{};
  const KvListOptions reverse{.reverse = true};
  const KvListOptions batch1{.batchSize = 1};
  EXPECT_EQ((co_await page(p, batch1)).first, children);
  const KvListSelector fromP3{.prefix = KvKey{"p"}, .start = KvKey{"p", 3}};
  EXPECT_EQ((co_await page(fromP3, none)).first, vals("c3", "deep", "c4", "c5", "c6", "c7"));
  const KvListSelector toP3{.prefix = KvKey{"p"}, .end = KvKey{"p", 3}};
  EXPECT_EQ((co_await page(toP3, none)).first, vals("c1", "c2"));
  const KvListSelector pToP3{.start = KvKey{"p"}, .end = KvKey{"p", 3}};
  EXPECT_EQ((co_await page(pToP3, none)).first, vals("root", "c1", "c2"));
  EXPECT_EQ((co_await page(pToP3, reverse)).first, vals("c2", "c1", "root"));
  const KvListSelector p2ToP2{.start = KvKey{"p", 2}, .end = KvKey{"p", 2}};
  EXPECT((co_await page(p2ToP2, none)).first.empty());
  const KvListSelector everything{.prefix = KvKey{}};
  EXPECT_EQ((co_await page(everything, none)).first, vals("root", "c1", "c2", "c3", "deep", "c4", "c5", "c6", "c7", "other"));
  const KvListSelector fromP5{.prefix = KvKey{"p"}, .start = KvKey{"p", 5}};
  EXPECT_EQ((co_await page(fromP5, reverse)).first, vals("c7", "c6", "c5"));
  const KvListSelector toP5{.prefix = KvKey{"p"}, .end = KvKey{"p", 5}};
  EXPECT_EQ((co_await page(toP5, reverse2)).first, vals("c4", "deep"));
  const KvListSelector q{.prefix = KvKey{"q"}};
  const KvListOptions limit1{.limit = 1};
  const auto exactPage = co_await page(q, limit1);
  const auto& exact = std::get<0>(exactPage);
  const auto& exactCursor = std::get<1>(exactPage);
  EXPECT_EQ(exact, std::vector<KvValue>{"other"});
  EXPECT_EQ(exactCursor, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MTAwMjFiZmYwMDAwMDAwMDAwMDAwIn0="));
  const KvListOptions afterExact{.limit = 1, .cursor = exactCursor};
  const auto afterPage = co_await page(q, afterExact);
  const auto& after = std::get<0>(afterPage);
  const auto& afterCursor = std::get<1>(afterPage);
  EXPECT(after.empty());
  EXPECT_EQ(afterCursor, std::string(""));
  const KvListOptions crossCursor{.cursor = cursor1};
  EXPECT_EQ((co_await page(q, crossCursor)).first, vals("deep", "c4", "c5", "c6", "c7", "other"));
  const KvListOptions crossCursor2{.cursor = exactCursor};
  EXPECT((co_await page(p, crossCursor2)).first.empty());
  const auto listError = [&](KvListSelector selector, KvListOptions options) { return thrown([&] { (void)kv.list(std::move(selector), std::move(options)); }); };
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .start = KvKey{"p", 1}, .end = KvKey{"p", 2}}, {}),
            std::string("TypeError: Selector can not specify both 'start' and 'end' key when specifying 'prefix'"));
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .start = KvKey{"q"}}, {}), std::string("TypeError: Start key is not in the keyspace defined by prefix"));
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .start = KvKey{"p"}}, {}), std::string("TypeError: Start key is not in the keyspace defined by prefix"));
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .end = KvKey{"outside", 1}}, {}), std::string("TypeError: End key is not in the keyspace defined by prefix"));
  EXPECT_EQ(listError({.start = KvKey{"z"}, .end = KvKey{"a"}}, {}), std::string("TypeError: Start key is greater than end key"));
  EXPECT_EQ(listError({.start = KvKey{"z"}}, {}), std::string("TypeError: Selector must specify either 'prefix' or both 'start' and 'end' key"));
  EXPECT_EQ(listError({.end = KvKey{"z"}}, {}), std::string("TypeError: Selector must specify either 'prefix' or both 'start' and 'end' key"));
  EXPECT_EQ(listError(p, {.limit = 0}), std::string("TypeError: Bad 'limit': 0"));
  EXPECT_EQ(listError(p, {.limit = 1.5}), std::string("TypeError: Bad 'limit': 1.5"));
  EXPECT_EQ(listError(p, {.limit = -1}), std::string("TypeError: Bad 'limit': -1"));
  EXPECT_EQ(listError(p, {.batchSize = 1001}), std::string("TypeError: Bad 'batchSize': 1001"));
  EXPECT_EQ(listError(p, {.batchSize = 0}), std::string("TypeError: Bad 'batchSize': 0"));
  const std::vector<std::string> invalidCursors{"garbage", "", base64("{\"lastYieldedKeyBytes\":\"zz\"}")};
  for (const std::string& cursor : invalidCursors) {
    auto invalid = kv.list(p, {.cursor = cursor});
    EXPECT_EQ(invalid->cursor(), cursor);
    EXPECT_EQ(co_await outcome(invalid->next()), std::string("Error: Invalid cursor"));
    EXPECT(!(co_await invalid->next()));
  }
}

ASYNC_TEST("durable: list batches observe writes beyond the fetched batch only") {
  DurableKv kv(testClock());
  co_await kv.set({"snap", 1}, 1);
  co_await kv.set({"snap", 3}, 3);
  auto small = kv.list({.prefix = KvKey{"snap"}}, {.batchSize = 1});
  EXPECT_EQ((co_await small->next())->key, (KvKey{"snap", 1}));
  co_await kv.set({"snap", 2}, 2);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*small)), vals(2, 3));
  co_await kv.delete_({"snap", 2});
  auto large = kv.list({.prefix = KvKey{"snap"}});
  EXPECT_EQ((co_await large->next())->key, (KvKey{"snap", 1}));
  co_await kv.set({"snap", 2}, 2);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*large)), vals(3));
}

ASYNC_TEST("durable: reverse pages, deleted cursor keys and forgiving cursors") {
  DurableKv kv(testClock());
  co_await kv.set({"a"}, 0);
  for (int index = 1; index <= 5; index += 1) co_await kv.set({"rv", index}, index);
  std::optional<std::string> cursor;
  std::vector<std::pair<std::vector<KvValue>, std::string>> pages;
  for (int index = 0; index < 4; index += 1) {
    auto iterator = kv.list({.prefix = KvKey{"rv"}}, {.limit = 2, .cursor = cursor, .reverse = true, .batchSize = 2});
    std::vector<KvValue> values = valuesOf(co_await kvListToArray(*iterator));
    pages.emplace_back(std::move(values), iterator->cursor());
    cursor = iterator->cursor();
    if (cursor->empty()) break;
  }
  ASSERT_EQ(pages.size(), 3u);
  EXPECT_EQ(pages[0].first, (std::vector<KvValue>{5, 4}));
  EXPECT_EQ(unpackCursor(pages[0].second), packKey({"rv", 4}));
  EXPECT_EQ(hex(unpackCursor(pages[0].second)), std::string("0272760021c010000000000000"));
  EXPECT_EQ(pages[1].first, (std::vector<KvValue>{3, 2}));
  EXPECT_EQ(hex(unpackCursor(pages[1].second)), std::string("0272760021c000000000000000"));
  EXPECT_EQ(pages[2].first, (std::vector<KvValue>{1}));
  EXPECT_EQ(pages[2].second, std::string(""));
  auto limited = kv.list({.prefix = KvKey{"rv"}}, {.limit = 2});
  co_await kvListToArray(*limited);
  co_await kv.delete_({"rv", 2});
  const std::string resume = limited->cursor();
  const auto rest = [&](std::string text) -> Task<std::vector<KvValue>> {
    auto iterator = kv.list({.prefix = KvKey{"rv"}}, {.cursor = std::move(text)});
    co_return valuesOf(co_await kvListToArray(*iterator));
  };
  EXPECT_EQ(co_await rest(resume), vals(3, 4, 5));
  std::string unpadded = resume;
  while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
  EXPECT_EQ(co_await rest(unpadded), vals(3, 4, 5));
  EXPECT_EQ(co_await rest(" " + resume + "\n"), vals(3, 4, 5));
  EXPECT_EQ(co_await outcome(rest(base64("5"))), std::string("Error: Invalid cursor"));
  EXPECT_EQ(co_await rest(base64("{\"lastYieldedKeyBytes\":\"0272760021BFF0000000000000\"}")), vals(3, 4, 5));
  // An empty key cursor starts the scan at the beginning of the whole keyspace.
  EXPECT_EQ(co_await rest(base64("{\"lastYieldedKeyBytes\":\"\"}")), vals(0, 1, 3, 4, 5));
}

ASYNC_TEST("durable: closing a handle follows the SDK's close checks") {
  DurableKv kv(testClock());
  co_await kv.set({"a"}, 1);
  co_await kv.set({"b"}, 2);
  auto pending = kv.list({.prefix = KvKey{}});
  auto started = kv.list({.prefix = KvKey{}}, {.batchSize = 1});
  EXPECT_EQ((co_await started->next())->key, (KvKey{"a"}));
  auto pendingAtomic = kv.atomic();
  pendingAtomic->set({"c"}, 3);
  kv.close();
  EXPECT_EQ(co_await outcome(kv.get({"a"})), std::string("Error: Cannot call '.get' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.getMany(keys(KvKey{"a"}))), std::string("Error: Cannot call '.getMany' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.getMany({})), std::string("Error: Cannot call '.getMany' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.set({"a"}, 1)), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.delete_({"a"})), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(thrown([&] { (void)kv.list({.prefix = KvKey{}}); }), std::string("Error: Cannot call '.list' after '.close' is called"));
  auto built = kv.atomic();
  built->set({"x"}, 1);
  EXPECT_EQ(co_await outcome(built->commit()), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(co_await outcome(pendingAtomic->commit()), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(co_await outcome(pending->next()), std::string("Error: db not found"));
  EXPECT_EQ(co_await outcome(started->next()), std::string("Error: db not found"));
  EXPECT_EQ(thrown([&] { kv.close(); }), std::string("Error: Cannot call '.close' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.get({})), std::string("Error: Cannot call '.get' after '.close' is called"));
}

ASYNC_TEST("durable: values read back like a native round trip") {
  DurableKv kv(testClock());
  const auto roundTrip = [&](KvValue value) -> Task<KvValue> {
    co_await kv.set({"value"}, std::move(value));
    co_return (co_await kv.get({"value"})).value;
  };
  EXPECT_EQ(co_await roundTrip(obj("c", KvU64(3), "arr", arr(KvU64(4)))), obj("c", u64Object(3), "arr", arr(u64Object(4))));
  KvMap map;
  map.set(1, "a");
  map.set("k", KvSet{BigInt(1), "x"});
  const KvValue rich = KvValue::object({{"m", map}, {"d", KvDate{1700000000123.0}}, {"r", KvRegExp{"x+", "gi"}}});
  EXPECT_EQ(co_await roundTrip(rich), rich);
  KvError boom;
  boom.name = "TypeError";
  boom.message = "boom";
  const KvValue error = co_await roundTrip(obj("e", boom));
  EXPECT_EQ(error["e"].as<KvError>().name, std::string("TypeError"));
  EXPECT_EQ(error["e"].as<KvError>().message, std::optional<std::string>("boom"));
  KvError custom;
  custom.name = "CustomError";
  custom.message = "x";
  EXPECT_EQ((co_await roundTrip(custom)).as<KvError>().name, std::string("Error"));
  const KvValue undefinedMembers = KvValue::object({{"a", KvValue()}, {"b", KvValue::array({KvValue(), 1})}, {"c", 1}});
  const KvValue members = co_await roundTrip(undefinedMembers);
  EXPECT_EQ(members, undefinedMembers);
  EXPECT(members.asObject().contains("a"));
  KvArray holes(4, KvHole{});
  holes[1] = 1;
  holes[3] = 2;
  EXPECT_EQ(co_await roundTrip(holes), KvValue(holes));
  const KvValue numbers = co_await roundTrip(obj("z", -0.0, "n", NAN_, "i", -INF));
  EXPECT(std::signbit(numbers["z"].asNumber()));
  EXPECT(std::isnan(numbers["n"].asNumber()));
  EXPECT_EQ(numbers["i"], KvValue(-INF));
  KvObject ordered;
  for (const auto& [key, value] : std::vector<std::pair<std::string, int>>{{"b", 1}, {"2", 2}, {"a", 3}, {"1", 4}, {"-1", 5}, {"01", 6}, {"4294967294", 7}, {"4294967295", 8}})
    ordered.set(key, value);
  std::vector<std::string> names;
  const KvValue orderedBack = co_await roundTrip(ordered);
  for (const auto& entry : orderedBack.asObject()) names.push_back(entry.first);
  EXPECT_EQ(names, (std::vector<std::string>{"1", "2", "4294967294", "b", "a", "-1", "01", "4294967295"}));
  const KvBytes backing{9, 1, 2, 9};
  const KvArrayBufferView middle{KvViewKind::Uint8Array, backing, 1, 2};
  EXPECT_EQ(co_await roundTrip(middle), KvValue(bytes(1, 2)));
  const KvValue nested = co_await roundTrip(obj("s", middle));
  EXPECT_EQ(nested["s"].as<KvArrayBufferView>().byteOffset, 1u);
  EXPECT_EQ(nested["s"].as<KvArrayBufferView>().buffer.size(), 4u);
  const KvArrayBuffer buffer{bytes(1, 2)};
  EXPECT_EQ(co_await roundTrip(buffer), KvValue(buffer));
  const KvArrayBufferView float32{KvViewKind::Float32Array, bytes(0, 0, 0xc0, 0x3f), 0, 4};
  const KvValue floats = co_await roundTrip(float32);
  EXPECT_EQ(floats.kind(), std::string_view("Float32Array"));
  EXPECT_EQ(co_await roundTrip(obj("v", -(BigInt(1) << BigInt(70)))), obj("v", -(BigInt(1) << BigInt(70))));
  EXPECT_EQ(co_await roundTrip("λ"), KvValue("λ"));
  EXPECT_EQ(co_await roundTrip("\xed\xa0\x80"), KvValue("\xed\xa0\x80"));
}

ASYNC_TEST("durable: expired rows stay readable until a native sweep") {
  fakeNow = 1'700'000'000'000.0;
  const double opened = fakeNow;
  DurableKv kv(testClock());
  const KvCommitResult written = co_await kv.set({"e"}, "v", {.expireIn = 100});
  EXPECT_EQ(kv.database()->row({"e"})->expirationMs, opened + 100);
  fakeNow = opened + 500;
  EXPECT_EQ((co_await kv.get({"e"})).value, KvValue("v"));
  auto rows = kv.list({.prefix = KvKey{}});
  EXPECT_EQ((co_await kvListToArray(*rows)).size(), 1u);
  auto check = kv.atomic();
  check->check(AtomicCheck{{"probe-dummy"}, std::nullopt}).check(AtomicCheck{{"e"}, written.versionstamp});
  EXPECT((co_await check->commit()).ok);
  co_await kv.set({"o"}, 1, {.expireIn = 60'000});
  co_await kv.set({"o"}, 2);
  EXPECT_EQ(kv.database()->row({"o"})->expirationMs, -1.0);
  co_await kv.set({"s"}, KvU64(1), {.expireIn = 60'000});
  auto sum = kv.atomic();
  sum->sum({"s"}, 1);
  co_await sum->commit();
  EXPECT_EQ(kv.database()->row({"s"})->expirationMs, -1.0);
  fakeNow = opened + 59'999;
  EXPECT((co_await kv.get({"e"})).versionstamp.has_value());
  fakeNow = opened + 60'000;
  EXPECT(!(co_await kv.get({"e"})).versionstamp);
  EXPECT(!kv.database()->row({"e"}));
}

ASYNC_TEST("durable: a zero sweep interval expires rows as soon as they lapse") {
  DurableKv kv(testClock(), {.sweepIntervalMs = 0});
  co_await kv.set({"e"}, "v", {.expireIn = 10});
  fakeNow += 9;
  EXPECT((co_await kv.get({"e"})).versionstamp.has_value());
  fakeNow += 1;
  EXPECT(!(co_await kv.get({"e"})).versionstamp);
}

ASYNC_TEST("durable: string key parts normalize through TextEncoder and TextDecoder") {
  DurableKv kv(testClock());
  co_await kv.set({"bom", "﻿a"}, 1);
  co_await kv.set({"bom", "\xed\xa0\x80x"}, 2);
  co_await kv.set({"bom", "a﻿"}, 3);
  auto iterator = kv.list({.prefix = KvKey{"bom"}});
  const auto rows = co_await kvListToArray(*iterator);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].key, (KvKey{"bom", "a﻿"}));
  EXPECT_EQ(rows[1].key, (KvKey{"bom", "a"}));
  EXPECT_EQ(rows[2].key, (KvKey{"bom", "�x"}));
  EXPECT_EQ(valuesOf(rows), (std::vector<KvValue>{3, 1, 2}));
  EXPECT_EQ((co_await kv.get({"bom", "\xed\xa0\x80x"})).value, KvValue(2));
  EXPECT_EQ((co_await kv.get({"bom", "�x"})).value, KvValue(2));
  EXPECT_EQ((co_await kv.get({"bom", "﻿a"})).key, (KvKey{"bom", "﻿a"}));
  EXPECT_EQ(hex(packKey({"bom", "\uFEFFa"})), std::string("02626f6d0002efbbbf6100"));
  EXPECT_EQ(unhex("02626f6d00"), packKey({"bom"}));
}


ASYNC_TEST("durable: typed get and getMany convert entries") {
  DurableKv kv(testClock());
  co_await kv.set({"rec"}, obj("a", 1, "b", "x"));
  co_await kv.set({"counter"}, KvU64(5));
  const KvEntryMaybe<json::object> typed = co_await kv.get<json::object>({"rec"});
  EXPECT_EQ(typed.value.at("a").as_int64(), 1);
  EXPECT_EQ(typed.value.at("b").as_string(), json::string("x"));
  EXPECT(typed.versionstamp.has_value());
  const auto missing = co_await kv.get<std::optional<json::object>>({"missing"});
  EXPECT(!missing.value);
  EXPECT(!missing.versionstamp);
  EXPECT((co_await kv.get<json::value>({"missing"})).value.is_null());
  EXPECT_EQ(co_await outcome(kv.get<json::object>({"missing"})), std::string("TypeError: KV value is null, not Object"));
  EXPECT_EQ((co_await kv.get<KvU64>({"counter"})).value, KvU64(5));
  const auto both = co_await kv.getMany<json::value>(keys(KvKey{"rec"}, KvKey{"missing"}));
  EXPECT(both[0].value.is_object());
  EXPECT(both[1].value.is_null());
  const KvEntryMaybe<> raw = co_await kv.get({"rec"});
  EXPECT_EQ(raw.as<json::value>().value, json::parse(R"({"a":1,"b":"x"})"));
}

namespace {
  struct DedupeObservation {
    std::vector<AtomicCheck> checks;
    bool deletesDedupe = false;
    std::optional<KvCommitResult> result;
  };

  struct ObservedAtomic : ForwardingAtomicOperation {
    ObservedAtomic(std::unique_ptr<AtomicOperation> inner_, DedupeObservation& observation_, KvKey key_)
        : ForwardingAtomicOperation(std::move(inner_)), observation(observation_), key(std::move(key_)) {}
    using ForwardingAtomicOperation::check;
    AtomicOperation& check(std::vector<AtomicCheck> items) override {
      observation.checks.insert(observation.checks.end(), items.begin(), items.end());
      return ForwardingAtomicOperation::check(std::move(items));
    }
    AtomicOperation& delete_(KvKey target) override {
      if (target == key) observation.deletesDedupe = true;
      return ForwardingAtomicOperation::delete_(std::move(target));
    }
    Task<KvCommitResult> commit() override {
      const KvCommitResult result = co_await ForwardingAtomicOperation::commit();
      if (observation.deletesDedupe) observation.result = result;
      co_return result;
    }
    DedupeObservation& observation;
    KvKey key;
  };

  struct ObservingKv : ForwardingKv {
    ObservingKv(Kv& inner_, DedupeObservation& observation_, KvKey key_) : ForwardingKv(inner_), observation(observation_), key(std::move(key_)) {}
    std::unique_ptr<AtomicOperation> atomic() override { return std::make_unique<ObservedAtomic>(ForwardingKv::atomic(), observation, key); }
    DedupeObservation& observation;
    KvKey key;
  };
}

ASYNC_TEST("durable: forwarding wrappers observe builder calls and forward the commit") {
  DurableKv native(testClock());
  co_await native.set({"dedupe", "f"}, "row");
  DedupeObservation observation;
  ObservingKv kv(native, observation, {"dedupe", "f"});
  const KvEntryMaybe<> row = co_await kv.get({"dedupe", "f"});
  auto operation = kv.atomic();
  operation->check(row).delete_({"dedupe", "f"});
  const KvCommitResult result = co_await operation->commit();
  EXPECT(result.ok);
  ASSERT_EQ(observation.checks.size(), 1u);
  EXPECT_EQ(observation.checks[0], AtomicCheck(row));
  EXPECT(observation.deletesDedupe);
  EXPECT_EQ(observation.result, std::optional<KvCommitResult>(result));
  EXPECT(!(co_await native.get({"dedupe", "f"})).versionstamp);
}

ASYNC_TEST("durable: return() ends a scan like breaking out of for await") {
  DurableKv kv(testClock());
  for (int index = 1; index <= 3; index += 1) co_await kv.set({"r", index}, index);
  auto broken = kv.list({.prefix = KvKey{"r"}});
  EXPECT_EQ((co_await broken->next())->value, KvValue(1));
  co_await broken->return_();
  EXPECT_EQ(hex(unpackCursor(broken->cursor())), std::string("02720021bff0000000000000"));
  EXPECT(!(co_await broken->next()));
  EXPECT_EQ(broken->cursor(), std::string(""));
  auto unstarted = kv.list({.prefix = KvKey{"r"}}, {.limit = 2});
  co_await unstarted->return_();
  EXPECT(!(co_await unstarted->next()));
  EXPECT_EQ(unstarted->cursor(), std::string(""));
  auto limited = kv.list({.prefix = KvKey{"r"}}, {.limit = 1});
  co_await limited->next();
  co_await limited->return_();
  EXPECT(!(co_await limited->next()));
  EXPECT_EQ(hex(unpackCursor(limited->cursor())), std::string("02720021bff0000000000000"));
}

