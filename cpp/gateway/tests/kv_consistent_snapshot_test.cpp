// Port of tests/kv-consistent-snapshot.test.ts (every case). Fixtures live under rt::os::tmpdir()
// (TMPDIR), as the TS uses os.tmpdir(); rt::fs has no symlinkSync, so the symlink fixture uses
// symlink(2) directly.
// Links: tests/kv_consistent_snapshot_test.cpp src/kv_conversion/convert.cpp src/kv_conversion/cursors.cpp
//        src/kv_conversion/native_schema.cpp src/kv_conversion/typed_value.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
#include "uos/kv_conversion/convert.hpp"

#include "uos/kv_conversion/native_schema.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/test_async.hpp"

#include <unistd.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace convert = uos::kv_conversion::convert;
  namespace native_schema = uos::kv_conversion::native_schema;
  using native_schema::NativeReader;
  using native_schema::NativeRevision;
  using Row = native_schema::bun_sqlite::Row;

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

  struct Fixture {
    std::string directory;
    std::string source;
  };

  Fixture fixture(bool marked = true) {
    const std::string directory = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-stopped-snapshot-"));
    owned.push_back(directory);
    const std::string source = rt::path::join(directory, "source.sqlite3");
    rt::sqlite::DatabaseOptions options;
    options.create = true;
    options.strict = true;
    options.safeIntegers = true;
    const rt::sqlite::Database database(source, options);
    database.run("PRAGMA journal_mode=DELETE");
    for (const std::string_view ddl : native_schema::NATIVE_DDL) database.run(ddl);
    database.run("INSERT INTO migration_state(k,version) VALUES(0,3)");
    database.run("INSERT INTO data_version(k,version,seq) VALUES(0,7,0)");
    if (marked) {
      database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
      database.run("INSERT INTO uos_kv_metadata(key,value) VALUES('codec','bun-jsc-v1')");
    }
    database.close(true);
    return {directory, source};
  }

  // `assert.rejects(task)`: String(error) of the rejection, std::nullopt when it fulfilled.
  template <class T> rt::Task<std::optional<std::string>> rejection(rt::Task<T> task) {
    std::optional<std::string> reason;
    try {
      co_await std::move(task);
    } catch (...) {
      reason = rt::reasonToString(std::current_exception());
    }
    co_return reason;
  }

  bool matches(const std::optional<std::string>& text, const char* pattern, const char* flags = "") {
    const rt::Regex regex(pattern, flags);
    return text.has_value() && regex.test(*text);
  }

  std::optional<std::int64_t> bigintColumn(const Row& row, std::string_view name) {
    const auto* value = row.get(name);
    if (value == nullptr || !std::holds_alternative<std::int64_t>(*value)) return std::nullopt;
    return std::get<std::int64_t>(*value);
  }

  using ReadCount = std::function<rt::Task<double>(NativeReader reader, NativeRevision revision)>;
}

ASYNC_TEST("private Bun callback reads a verified immutable marked snapshot and removes its private copy") {
  AfterEach cleanup;
  const Fixture files = fixture();
  const std::vector<std::uint8_t> original = rt::fs::readFileSync(files.source);
  std::function<rt::Task<std::vector<Row>>(NativeReader, NativeRevision)> read = [](NativeReader reader, NativeRevision revision) -> rt::Task<std::vector<Row>> {
    EXPECT(revision == (NativeRevision{.k = 0, .version = 7, .seq = 0}));
    std::optional<std::string> refused;
    try {
      reader.query("DELETE FROM data_version").all();
    } catch (...) {
      refused = rt::reasonToString(std::current_exception());
    }
    EXPECT(matches(refused, "readonly", "i"));
    std::vector<Row> rows = reader.query("SELECT version FROM data_version").all();
    co_return rows;
  };
  auto running = convert::withPrivateConsistentBunSnapshot<std::vector<Row>>(files.source, files.directory, read);
  const convert::PrivateConsistentBunSnapshot<std::vector<Row>> result = co_await std::move(running);
  ASSERT_EQ(result.value.size(), std::size_t{1});
  EXPECT_EQ(result.value[0].size(), std::size_t{1});
  EXPECT_EQ(bigintColumn(result.value[0], "version"), std::optional<std::int64_t>(7));
  EXPECT(matches(result.sourceIdentity.sha256, "^[a-f0-9]{64}$"));
  EXPECT_EQ(result.sourceIdentity.drain_boundary, std::string("checkpointed-no-wal"));
  EXPECT(rt::fs::readFileSync(files.source) == original);
  EXPECT_EQ(rt::fs::readdirSync(files.directory), std::vector<std::string>{"source.sqlite3"});
}

ASYNC_TEST("unmarked, corrupt, unsettled and symlink sources refuse before invoking the callback") {
  AfterEach cleanup;
  const Fixture files = fixture(false);
  const auto calls = std::make_shared<double>(0);
  const ReadCount read = [calls](NativeReader, NativeRevision) -> rt::Task<double> {
    *calls += 1;
    co_return *calls;
  };
  auto unmarked = rejection(convert::withPrivateConsistentBunSnapshot<double>(files.source, files.directory, read));
  const std::optional<std::string> unmarkedReason = co_await std::move(unmarked);
  EXPECT(unmarkedReason.has_value());
  const std::string corrupt = rt::path::join(files.directory, "corrupt.sqlite3");
  rt::fs::writeFileSync(corrupt, "not a database");
  auto corrupted = rejection(convert::withPrivateConsistentBunSnapshot<double>(corrupt, files.directory, read));
  const std::optional<std::string> corruptReason = co_await std::move(corrupted);
  EXPECT(corruptReason.has_value());
  const std::string link = rt::path::join(files.directory, "link.sqlite3");
  ASSERT(::symlink(files.source.c_str(), link.c_str()) == 0);
  auto linked = rejection(convert::withPrivateConsistentBunSnapshot<double>(link, files.directory, read));
  const std::optional<std::string> linkReason = co_await std::move(linked);
  EXPECT(matches(linkReason, "regular SQLite"));
  const std::vector<std::string> suffixes{"-wal", "-journal"};
  for (const std::string& suffix : suffixes) {
    rt::fs::writeFileSync(files.source + suffix, "unsettled");
    auto unsettled = rejection(convert::withPrivateConsistentBunSnapshot<double>(files.source, files.directory, read));
    const std::optional<std::string> unsettledReason = co_await std::move(unsettled);
    EXPECT(matches(unsettledReason, "stopped|journal"));
    rt::fs::rmSync(files.source + suffix);
  }
  EXPECT_EQ(*calls, 0.0);
  bool captureLeft = false;
  for (const std::string& entry : rt::fs::readdirSync(files.directory)) captureLeft = captureLeft || entry.rfind(".uos-kv-capture-", 0) == 0;
  EXPECT(!captureLeft);
}

ASYNC_TEST("source changes and callback errors fail without publishing or retaining a snapshot") {
  AfterEach cleanup;
  const Fixture files = fixture();
  const std::string source = files.source;
  const std::function<rt::Task<std::monostate>(NativeReader, NativeRevision)> mutate = [source](NativeReader, NativeRevision) -> rt::Task<std::monostate> {
    rt::sqlite::DatabaseOptions options;
    options.strict = true;
    options.safeIntegers = true;
    const rt::sqlite::Database changed(source, options);
    changed.run("UPDATE data_version SET version=8");
    changed.close(true);
    co_return std::monostate{};
  };
  auto changing = rejection(convert::withPrivateConsistentBunSnapshot<std::monostate>(files.source, files.directory, mutate));
  const std::optional<std::string> changedReason = co_await std::move(changing);
  EXPECT(matches(changedReason, "Source changed"));
  const std::function<rt::Task<std::monostate>(NativeReader, NativeRevision)> refuse = [](NativeReader, NativeRevision) -> rt::Task<std::monostate> {
    throw rt::JsError("synthetic callback refusal");
    co_return std::monostate{};
  };
  auto refusing = rejection(convert::withPrivateConsistentBunSnapshot<std::monostate>(files.source, files.directory, refuse));
  const std::optional<std::string> refusedReason = co_await std::move(refusing);
  EXPECT(matches(refusedReason, "synthetic callback refusal"));
  EXPECT_EQ(rt::fs::readdirSync(files.directory), std::vector<std::string>{"source.sqlite3"});
}
