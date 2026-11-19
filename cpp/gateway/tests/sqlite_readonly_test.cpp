// Port of tests/sqlite-readonly.test.ts.
// Links: tests/sqlite_readonly_test.cpp src/storage/sqlite_readonly.cpp src/storage/kv.cpp src/storage/native_kv.cpp
// src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
//
#include "uos/storage/sqlite_readonly.hpp"
#include "uos/storage/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/url.hpp"

#include <sys/stat.h>

#include <cmath>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::storage::sqlite_readonly::Database;
using uos::storage::sqlite_readonly::DatabaseOptions;
using uos::storage::sqlite_readonly::SQLiteError;
using uos::storage::sqlite_readonly::inspectSqliteReadOnly;
namespace constants = uos::storage::sqlite_readonly::constants;

namespace {
  // The TS keeps its fixtures under .cleanup-evidence/sqlite-readonly in the
  // checkout; these live under the system temporary directory.
  std::string fixtureRoot() {
    static const std::string root = [] {
      std::string path = rt::os::tmpdir() + "/uos-sqlite-readonly";
      rt::fs::mkdirSync(path, {.recursive = true});
      return path;
    }();
    return root;
  }
  std::vector<std::string> directories;

  // `afterEach`: removes the test's fixture directories, also when it fails.
  struct AfterEach {
    AfterEach() = default;
    AfterEach(const AfterEach&) = delete;
    AfterEach& operator=(const AfterEach&) = delete;
    ~AfterEach() {
      for (const std::string& directory : directories) rt::fs::rmSync(directory, {.recursive = true, .force = true});
      directories.clear();
    }
  };

  struct Fixture {
    std::string root;
    std::string path;
  };

  // The store file is named inspected.sqlite3 (the TS names it kv-bun.sqlite3;
  // the module never reads the name).
  Fixture fixture(bool wal = true) {
    const std::string root = rt::fs::mkdtempSync(fixtureRoot() + "/case %?#-");
    directories.push_back(root);
    rt::fs::mkdirSync(root + "/.data");
    const std::string path = root + "/.data/inspected.sqlite3";
    Database db(path);
    std::exception_ptr failure;
    try {
      db.fileControl(constants::SQLITE_FCNTL_PERSIST_WAL, 0);
      if (wal) db.run("PRAGMA journal_mode=WAL");
      db.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
      db.run("INSERT INTO uos_kv_metadata VALUES ('codec','bun-jsc-v1')");
    } catch (...) {
      failure = std::current_exception();
    }
    db.close(true);
    if (failure) std::rethrow_exception(failure);
    return {root, path};
  }

  // `db.query<{ value: string }, []>(...).get()?.value`
  std::optional<std::string> marker(Database& db) {
    const std::optional<json::object> row = db.query("SELECT value FROM uos_kv_metadata WHERE key='codec'").get();
    if (!row) return std::nullopt;
    const std::optional<std::string_view> value = rt::getString(*row, "value");
    return value ? std::optional<std::string>(std::string(*value)) : std::nullopt;
  }

  // `expect(fn).toThrow(text)`: an Error whose message contains `text`.
  template <class Body> void expectThrowIncluding(Body&& body, std::string_view text, const char* file, int line) {
    try {
      body();
    } catch (const rt::JsError& error) {
      if (!rt::includes(error.message, text)) rt::test::report(file, line, "expected a message containing " + rt::test::show(text) + ", got " + rt::test::show(error.message), false);
      return;
    }
    rt::test::report(file, line, "expected a throw containing " + rt::test::show(text), false);
  }

  // node:fs utimesSync(path, atime, mtime) with Date arguments (no rt counterpart).
  void utimesSync(const std::string& path, double atimeMs, double mtimeMs) {
    const auto time = [](double ms) {
      timespec spec{};
      spec.tv_sec = static_cast<std::time_t>(std::floor(ms / 1000));
      spec.tv_nsec = static_cast<long>((ms - static_cast<double>(spec.tv_sec) * 1000) * 1'000'000);
      return spec;
    };
    const timespec times[2] = {time(atimeMs), time(mtimeMs)};
    if (::utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) throw rt::fs::errnoError(errno, "utime", path);
  }

  // `await assert.rejects(openKv(path), pattern)`: the rejection's message.
  std::string openKvRejection(const std::string& path) {
    try {
      const std::shared_ptr<rt::Kv> opened = rt::blockOn(uos::storage::kv::openKv(path));
      opened->close();
    } catch (const rt::JsError& error) {
      return error.message;
    }
    return "<resolved>";
  }
}

TEST("readonly codec inspection accepts a drained WAL-mode file without changing database bytes") {
  const AfterEach afterEach;
  const Fixture created = fixture();
  const rt::fs::Bytes before = rt::fs::readFileSync(created.path);
  for (const char* suffix : {"-wal", "-shm", "-journal"}) EXPECT_EQ(rt::fs::existsSync(created.path + suffix), false);
  EXPECT_EQ(inspectSqliteReadOnly(created.path, marker), std::optional<std::string>("bun-jsc-v1"));
  // `await assertBunConvertedStore(root)` (ops/assert-bun-store.ts) is outside the ported tree.
  EXPECT(rt::fs::readFileSync(created.path) == before);
}

TEST("an immutable read-only SQLite URI addresses a database with reserved path bytes") {
  const AfterEach afterEach;
  const Fixture created = fixture();
  const rt::fs::Bytes before = rt::fs::readFileSync(created.path);
  rt::URL uri = rt::pathToFileURL(created.path);
  uri.searchParams().set("immutable", "1");
  const rt::sqlite::Database db(uri.href(), rt::sqlite::constants::SQLITE_OPEN_READONLY | rt::sqlite::constants::SQLITE_OPEN_URI);
  const auto row = db.query("SELECT value FROM uos_kv_metadata WHERE key='codec'").get();
  ASSERT(row.has_value());
  EXPECT_EQ(rt::sqlite::toJson(*row), json::object({{"value", "bun-jsc-v1"}}));
  EXPECT_THROWS_MATCHING(db.run("DELETE FROM uos_kv_metadata"), "readonly");
  for (const char* suffix : {"-wal", "-shm", "-journal"}) EXPECT(!rt::fs::existsSync(created.path + suffix));
  db.close(true);
  for (const char* suffix : {"-wal", "-shm", "-journal"}) EXPECT(!rt::fs::existsSync(created.path + suffix));
  EXPECT(rt::fs::readFileSync(created.path) == before);
}

TEST("readonly codec inspection sees the latest live WAL marker before native opening") {
  const AfterEach afterEach;
  const Fixture created = fixture();
  Database writer(created.path);
  std::exception_ptr failure;
  try {
    writer.run("UPDATE uos_kv_metadata SET value='deno-v8' WHERE key='codec'");
    EXPECT_EQ(rt::fs::existsSync(created.path + "-wal"), true);
    const rt::fs::Bytes before = rt::fs::readFileSync(created.path);
    const rt::fs::Bytes walBefore = rt::fs::readFileSync(created.path + "-wal");
    EXPECT_EQ(inspectSqliteReadOnly(created.path, marker), std::optional<std::string>("deno-v8"));
    // `assertBunConvertedStore(root)` rejecting /explicit Bun\/JSC marker/ (ops/assert-bun-store.ts,
    // outside the ported tree) is not checked here.
    EXPECT(rt::includes(openKvRejection(created.path), "codec is missing or incompatible"));
    EXPECT(rt::fs::readFileSync(created.path) == before);
    EXPECT(rt::fs::readFileSync(created.path + "-wal") == walBefore);
  } catch (...) {
    failure = std::current_exception();
  }
  writer.close(true);
  if (failure) std::rethrow_exception(failure);
}

TEST("read-only validation errors propagate without retrying an inspection") {
  const AfterEach afterEach;
  const Fixture created = fixture(false);
  int reads = 0;
  EXPECT_THROWS_MESSAGE(inspectSqliteReadOnly(created.path,
                                              [&reads](Database&) {
                                                reads++;
                                                throw rt::JsError("marker inspection rejected");
                                              }),
                        rt::JsError, "marker inspection rejected");
  EXPECT_EQ(reads, 1);
}

// Differential cases (not in the TS file), recorded with bun 1.4.0 on Linux: the
// fallback this platform reaches, and why its immutable retry cannot open.

TEST("a missing database falls back to the immutable retry and fails reading its file state") {
  const AfterEach afterEach;
  const Fixture created = fixture();
  const std::string missing = created.root + "/.data/missing.sqlite3";
  EXPECT_THROWS_MESSAGE(inspectSqliteReadOnly(missing, marker), rt::fs::ErrnoError, "ENOENT: no such file or directory, lstat '" + missing + "'");
}

#if defined(__APPLE__)
TEST("bun:sqlite on Darwin opens an immutable file: URI without creating sidecars") {
  const AfterEach afterEach;
  const Fixture created = fixture(false);
  rt::URL uri = rt::pathToFileURL(created.path);
  uri.searchParams().set("immutable", "1");
  DatabaseOptions options;
  options.readonly = true;
  options.strict = true;
  Database db(uri.href(), options);
  EXPECT_EQ(marker(db), std::optional<std::string>("bun-jsc-v1"));
  db.close(true);
  for (const char* suffix : {"-wal", "-shm", "-journal"}) EXPECT_EQ(rt::fs::existsSync(created.path + suffix), false);
}
#else
TEST("bun:sqlite on Linux opens an immutable file: URI as a plain path, which does not exist") {
  const AfterEach afterEach;
  const Fixture created = fixture(false);
  rt::URL uri = rt::pathToFileURL(created.path);
  uri.searchParams().set("immutable", "1");
  DatabaseOptions options;
  options.readonly = true;
  options.strict = true;
  bool threw = false;
  try {
    Database db(uri.href(), options);
  } catch (const SQLiteError& error) {
    threw = true;
    EXPECT_EQ(error.name, std::string("SQLiteError"));
    EXPECT_EQ(error.message, std::string("unable to open database file"));
    EXPECT_EQ(error.code, std::optional<std::string>("SQLITE_CANTOPEN"));
    EXPECT_EQ(error.errno_, 14);
    EXPECT_EQ(error.byteOffset, -1);
  }
  EXPECT(threw);
}
#endif

namespace {
  [[maybe_unused]] const bool darwinCases = [] {
    if (rt::process::platform != "darwin") return false;
    rt::test::registry().push_back({"immutable readonly inspection refuses every sidecar appearing during its fence", [] {
                                      for (const char* suffix : {"-wal", "-shm", "-journal"}) {
                                        const AfterEach afterEach;
                                        const Fixture created = fixture();
                                        const rt::fs::Bytes before = rt::fs::readFileSync(created.path);
                                        expectThrowIncluding(
                                          [&] {
                                            inspectSqliteReadOnly(created.path, [&](Database& db) {
                                              const std::optional<std::string> value = marker(db);
                                              rt::fs::writeFileSync(created.path + suffix, std::string_view(""));
                                              return value;
                                            });
                                          },
                                          "refuses WAL", __FILE__, __LINE__);
                                        EXPECT(rt::fs::readFileSync(created.path) == before);
                                      }
                                    }});
    rt::test::registry().push_back({"immutable readonly inspection refuses a file changed during its fence", [] {
                                      const AfterEach afterEach;
                                      const Fixture created = fixture();
                                      const rt::fs::Bytes before = rt::fs::readFileSync(created.path);
                                      expectThrowIncluding(
                                        [&] {
                                          inspectSqliteReadOnly(created.path, [&](Database& db) {
                                            const std::optional<std::string> value = marker(db);
                                            utimesSync(created.path, rt::dateNow(), rt::dateNow() + 1000);
                                            return value;
                                          });
                                        },
                                        "source changed", __FILE__, __LINE__);
                                      EXPECT(rt::fs::readFileSync(created.path) == before);
                                    }});
    return true;
  }();
}
