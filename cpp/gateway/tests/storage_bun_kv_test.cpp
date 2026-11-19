// Exercise the marked SQLite store, its guard, concurrent handles and subprocess durability.
// Links: tests/storage_bun_kv_test.cpp src/kv.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
#include "uos/kv.hpp"
#include "uos/storage/kv.hpp"
#include "uos/storage/sqlite_readonly.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/test_async.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <span>
#include <string>
#include <vector>

using namespace uos;

namespace {
  // The TS keeps fixtures under `.cleanup-evidence/m01-storage` and removes them after
  // each test; here they live under the temporary directory.
  struct Fixtures {
    std::string root = rt::fs::realpathSync(rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-storage-bun-kv-")));
    ~Fixtures() {
      rt::fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      rt::fs::rmSync(root, options);
    }
    std::string temporaryPath() const { return rt::fs::mkdtempSync(root + "/native-") + "/kv.sqlite3"; }
    // A database file for the openKv cases: the TS names it kv.sqlite3, which the host rule
    // reserves (no `kv*.sqlite3` fixture files), so it is store.sqlite3 here.
    std::string databasePath() const { return rt::fs::mkdtempSync(root + "/native-") + "/store.sqlite3"; }
  };

}

// scripts/test-runner.ts runs every bun test file with the provider variables removed;
// unit.sh does not, so the case first establishes that environment (values are never
// read or printed) and then checks the storage checks need none of them.
TEST("storage checks execute without provider credentials") {
  const std::vector<std::string> names = {"CODEX_AUTH_JSON_B64", "DENO_DEPLOY_TOKEN", "UOS_AI_TOKEN", "SURPLUS_API_KEY", "METERED_API_KEY", "OPENROUTER_API_KEY"};
  std::vector<std::pair<std::string, std::optional<std::string>>> removed;
  for (const std::string& name : names) removed.emplace_back(name, std::nullopt);
  const rt::env::ScopedOverride scrubbed(removed);
  bool absent = true;
  for (const std::string& name : names) absent = absent && !rt::truthy(rt::env::get(name).value_or(""));
  EXPECT(absent);
}

// ---- src/storage/kv.ts and src/kv.ts ------------------------------------------------------------
namespace {
  using storage::sqlite_readonly::Database;

  // node:fs symlinkSync for the alias fixture.
  void symlinkSync(const std::string& target, const std::string& path) {
    if (::symlink(target.c_str(), path.c_str()) != 0) throw std::runtime_error("symlink failed: " + path);
  }

  // What the native store received for one open.
  struct OpenCall {
    std::string path;
    std::string implementation;
    storage::kv::NativeKvOpenOptions options;
  };

  // The @deno/kv store behind openKv for one case: one MemoryKvDatabase per file path (a reopen
  // shares the rows, as a second native handle on the file does), a private store for ":memory:".
  struct FixtureSdk {
    std::map<std::string, std::shared_ptr<rt::MemoryKvDatabase>> files;
    std::vector<OpenCall> calls;
    // Runs before each open; throwing fails that open.
    std::function<void(const std::string& path)> beforeOpen;
    // Commits fail with the native addon's arithmetic error text instead of committing.
    bool nativeArithmeticFailure = false;
  };

  // A native store whose commits report the addon's wrong-type arithmetic failure.
  class NativeArithmeticFailureOperation final : public rt::ForwardingAtomicOperation {
  public:
    using rt::ForwardingAtomicOperation::ForwardingAtomicOperation;
    rt::Task<rt::KvCommitResult> commit() override {
      throw rt::JsError("Failed to perform 'sum' mutation on a non-U64 value in the database");
      co_return rt::KvCommitResult{};
    }
  };
  class NativeArithmeticFailureKv final : public rt::ForwardingKv {
  public:
    explicit NativeArithmeticFailureKv(std::shared_ptr<rt::Kv> inner_) : rt::ForwardingKv(*inner_), owned(std::move(inner_)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<NativeArithmeticFailureOperation>(inner.atomic()); }

  private:
    std::shared_ptr<rt::Kv> owned;
  };

  // Installs a FixtureSdk as the @deno/kv opener for one case and restores the default after it.
  struct SdkScope {
    std::shared_ptr<FixtureSdk> sdk = std::make_shared<FixtureSdk>();
    storage::kv::NativeKvOpener saved = storage::kv::openNativeKv();
    SdkScope() {
      std::shared_ptr<FixtureSdk> current = sdk;
      storage::kv::openNativeKv() = [current](std::string path, storage::kv::NativeKvOpenOptions options) { return openFixtureStore(current, path, options); };
    }
    ~SdkScope() {
      storage::kv::openNativeKv() = saved;
      kv::setKvForTest(nullptr);
    }
    SdkScope(const SdkScope&) = delete;
    SdkScope& operator=(const SdkScope&) = delete;

    static rt::Task<std::shared_ptr<rt::Kv>> openFixtureStore(std::shared_ptr<FixtureSdk> sdk, std::string path, storage::kv::NativeKvOpenOptions options) {
      sdk->calls.push_back(OpenCall{path, options.implementation, options});
      if (sdk->beforeOpen) sdk->beforeOpen(path);
      const std::function<double()> now = [] { return rt::dateNow(); };
      if (sdk->nativeArithmeticFailure) co_return std::make_shared<NativeArithmeticFailureKv>(std::make_shared<rt::MemoryKv>(now));
      if (path == ":memory:") co_return std::make_shared<rt::MemoryKv>(now);
      std::shared_ptr<rt::MemoryKvDatabase>& database = sdk->files[path];
      if (!database) database = std::make_shared<rt::MemoryKvDatabase>(now);
      co_return std::make_shared<rt::MemoryKv>(database);
    }
  };

  // `await expect(task).rejects.toThrow(...)`: the rejection's message, "<resolved>" otherwise.
  rt::Task<std::string> rejectionMessage(rt::Task<std::shared_ptr<rt::Kv>> task) {
    std::string message = "<resolved>";
    try {
      std::shared_ptr<rt::Kv> opened = co_await std::move(task);
      opened->close();
    } catch (const rt::JsError& error) {
      message = error.message;
    }
    co_return message;
  }

  rt::Task<std::shared_ptr<rt::Kv>> countedOpen(std::shared_ptr<int> attempts, std::shared_ptr<rt::Kv> kv) {
    *attempts += 1;
    co_await rt::sleep(5);
    co_return kv;
  }

  rt::Task<std::shared_ptr<rt::Kv>> failFirstOpen(std::shared_ptr<int> attempts, std::shared_ptr<rt::Kv> kv) {
    *attempts += 1;
    if (*attempts == 1) throw rt::JsError("fixture unavailable");
    co_return kv;
  }

  std::optional<std::string> metadataValue(const std::string& path, std::string_view key) {
    storage::sqlite_readonly::DatabaseOptions options;
    options.readonly = true;
    Database marker(path, options);
    const std::optional<json::object> row = marker.query("SELECT value FROM uos_kv_metadata WHERE key = '" + std::string(key) + "'").get();
    marker.close(true);
    if (!row) return std::nullopt;
    const std::optional<std::string_view> value = rt::getString(*row, "value");
    return value ? std::optional<std::string>(*value) : std::nullopt;
  }
}

ASYNC_TEST("openKv requires an explicit local SQLite or memory path") {
  std::vector<std::string> remote;
  remote.push_back("");
  remote.push_back("http://example.test/kv");
  remote.push_back("HTTPS://example.test/store.sqlite3");
  for (const std::string& path : remote) {
    const std::string message = co_await rejectionMessage(storage::kv::openKv(path));
    EXPECT_EQ(message, std::string("Bun KV requires an explicit local SQLite path or :memory:"));
  }
}

ASYNC_TEST("a persisted Bun database reopens with its codec marker and typed values") {
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> first = co_await storage::kv::openKv(path);
  rt::KvObject persisted;
  persisted.set("amount", rt::KvValue(rt::BigInt(9)));
  persisted.set("bytes", rt::KvValue(rt::KvBytes{7}));
  const rt::KvValue value(persisted);
  co_await first->set(rt::KvKey{"persisted"}, value);
  const rt::KvEntryMaybe<> before = co_await first->get(rt::KvKey{"persisted"});
  first->close();
  std::shared_ptr<rt::Kv> second = co_await storage::kv::openKv(path);
  EXPECT_EQ(metadataValue(path, "codec"), std::optional<std::string>(std::string(storage::kv::BUN_KV_CODEC)));
  const rt::KvEntryMaybe<> after = co_await second->get(rt::KvKey{"persisted"});
  EXPECT(after == before);
  second->close();
}

ASYNC_TEST("an unmarked legacy SQLite file is rejected without changing its bytes") {
  const SdkScope scope;
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  rt::sqlite::DatabaseOptions create;
  create.create = true;
  const rt::sqlite::Database legacy(path, create);
  legacy.run("CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL)");
  const std::vector<rt::sqlite::Binding> blobs = {rt::sqlite::Bytes{1}, rt::sqlite::Bytes{255, 15}};
  legacy.run("INSERT INTO kv VALUES (?, ?)", blobs);
  legacy.close(true);
  const rt::fs::Bytes before = rt::fs::readFileSync(path);
  const std::string message = co_await rejectionMessage(storage::kv::openKv(path));
  EXPECT(rt::includes(message, "convert the legacy database offline"));
  EXPECT(rt::fs::readFileSync(path) == before);
  EXPECT(scope.sdk->calls.empty());
}

ASYNC_TEST("the injection seam disables disk fallback and accepts initialization") {
  const SdkScope scope;
  kv::setKvForTest(nullptr);
  const std::shared_ptr<rt::Kv> absent = co_await kv::getKv().wait();
  EXPECT(absent == nullptr);
  std::shared_ptr<rt::Kv> opened = co_await storage::kv::openKv(":memory:");
  kv::initializeKv(opened);
  const std::shared_ptr<rt::Kv> installed = co_await kv::getKv().wait();
  EXPECT(installed == opened);
  EXPECT_THROWS_MESSAGE(kv::initializeKv(opened), rt::JsError, "KV has already been initialized");
  kv::setKvForTest(nullptr);
  opened->close();
}

ASYNC_TEST("codec mismatch and symlink paths reject before native opening") {
  const SdkScope scope;
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> first = co_await storage::kv::openKv(path);
  first->close();
  const rt::sqlite::Database marker(path);
  marker.run("UPDATE uos_kv_metadata SET value = 'v8-legacy' WHERE key = 'codec'");
  marker.close(true);
  const rt::fs::Bytes before = rt::fs::readFileSync(path);
  const std::string mismatch = co_await rejectionMessage(storage::kv::openKv(path));
  EXPECT(rt::includes(mismatch, "codec is missing or incompatible"));
  EXPECT(rt::fs::readFileSync(path) == before);
  const std::string alias = path + ".alias";
  symlinkSync(path, alias);
  const std::string symlinked = co_await rejectionMessage(storage::kv::openKv(alias));
  EXPECT(rt::includes(symlinked, "regular file"));
  EXPECT_EQ(scope.sdk->calls.size(), std::size_t{1});
}

ASYNC_TEST("concurrent opener calls coalesce and scoped failures retry after backoff") {
  const SdkScope scope;
  const std::shared_ptr<rt::Kv> opened = co_await storage::kv::openKv(":memory:");
  const std::shared_ptr<int> attempts = std::make_shared<int>(0);
  kv::setKvForTest(nullptr, [attempts, opened] { return countedOpen(attempts, opened); });
  std::vector<rt::Promise<std::shared_ptr<rt::Kv>>> pending;
  for (int index = 0; index < 3; ++index) pending.push_back(kv::getKv());
  auto all = rt::all(std::move(pending));
  const std::vector<std::shared_ptr<rt::Kv>> results = co_await all.wait();
  bool every = true;
  for (const std::shared_ptr<rt::Kv>& result : results) every = every && result == opened;
  EXPECT(every);
  EXPECT_EQ(*attempts, 1);
  {
    const rt::console::Capture silenced({rt::console::Method::error});
    *attempts = 0;
    kv::setKvForTest(nullptr, [attempts, opened] { return failFirstOpen(attempts, opened); });
    const std::shared_ptr<rt::Kv> firstTry = co_await kv::getKv().wait();
    EXPECT(firstTry == nullptr);
    const std::shared_ptr<rt::Kv> backedOff = co_await kv::getKv().wait();
    EXPECT(backedOff == nullptr);
    EXPECT_EQ(*attempts, 1);
    co_await rt::sleep(320);
    const std::shared_ptr<rt::Kv> reopened = co_await kv::getKv().wait();
    EXPECT(reopened == opened);
    EXPECT_EQ(*attempts, 2);
    ASSERT_EQ(silenced.calls().size(), std::size_t{1});
    EXPECT(silenced.calls()[0].args[0] == "[ai.ubq.fi] Failed to open KV; a later request will retry:");
    EXPECT(silenced.calls()[0].args[1].kind() == rt::console::Value::Kind::error && silenced.calls()[0].args[1].errorInfo().message == "fixture unavailable");
  }
  kv::setKvForTest(nullptr);
  opened->close();
}

ASYNC_TEST("replacing the KV opener keeps a suspended open callback alive") {
  const auto resume = rt::withResolvers<void>();
  auto captured = std::make_shared<std::string>("original KV opener");
  const std::weak_ptr observer = captured;
  const auto original = std::make_shared<rt::MemoryKv>(rt::dateNow);
  kv::setKvForTest(nullptr, [capture = std::move(captured), waiting = resume.promise, original]() -> rt::Task<std::shared_ptr<rt::Kv>> {
    co_await waiting.wait();
    EXPECT_EQ(*capture, "original KV opener");
    co_return original;
  });
  const auto reset = rt::scopeExit([] { kv::setKvForTest(nullptr); });
  const auto pending = kv::getKv();
  kv::setKvForTest(nullptr);
  ASSERT(!observer.expired());
  resume.resolve();
  const auto opened = co_await pending.wait();
  EXPECT_EQ(opened, original);
  EXPECT(observer.expired());
}

// Differential cases from src/storage/kv.ts beyond the bun tests.
ASYNC_TEST("openKv creates a marked database exclusively and hands the native store its resolved path") {
  const SdkScope scope;
  const Fixtures fixtures;
  const std::string path = fixtures.root + "/nested/deeper/store.sqlite3";
  const std::string relative = rt::path::relative(rt::process::cwd(), path);
  std::shared_ptr<rt::Kv> opened = co_await storage::kv::openKv(relative);
  opened->close();
  ASSERT_EQ(scope.sdk->calls.size(), std::size_t{1});
  EXPECT_EQ(scope.sdk->calls[0].path, path);
  EXPECT_EQ(scope.sdk->calls[0].implementation, std::string("sqlite"));
  EXPECT_EQ(rt::fs::lstatSync(path).mode & 0777u, 0600u);
  EXPECT_EQ(metadataValue(path, "codec"), std::optional<std::string>("bun-jsc-v1"));
  EXPECT_EQ(metadataValue(path, "created_bun_version"), std::optional<std::string>("1.4.0"));
  std::shared_ptr<rt::Kv> memory = co_await storage::kv::openKv(":memory:");
  memory->close();
  EXPECT_EQ(scope.sdk->calls[1].path, std::string(":memory:"));
  const std::string directory = co_await rejectionMessage(storage::kv::openKv(fixtures.root));
  EXPECT_EQ(directory, std::string("KV database must be a regular file, not a symlink or directory"));
}

ASYNC_TEST("openKv encodes values with the portable Bun serialization and retries only a locked database") {
  const SdkScope scope;
  std::shared_ptr<rt::Kv> opened = co_await storage::kv::openKv(":memory:");
  opened->close();
  const storage::kv::NativeKvOpenOptions& options = scope.sdk->calls.at(0).options;
  const rt::KvValue plain = rt::KvValue::object({{"amount", rt::KvValue(1)}});
  EXPECT(options.encodeV8(plain) == rt::kvJscSerialize(plain));
  rt::KvError failure;
  failure.message = "outer";
  failure.cause = std::make_shared<const rt::KvValue>(rt::KvValue("inner"));
  EXPECT_THROWS_MESSAGE(options.encodeV8(rt::KvValue(failure)), rt::TypeError, "Bun KV cannot preserve Error.cause; persist an explicit error record instead");
  const std::vector<std::uint8_t> stored = rt::kvJscSerialize(plain);
  EXPECT(options.decodeV8(stored) == plain);

  const std::shared_ptr<int> opens = std::make_shared<int>(0);
  scope.sdk->beforeOpen = [opens](const std::string&) {
    *opens += 1;
    if (*opens == 1) throw rt::JsError("database is locked");
  };
  std::shared_ptr<rt::Kv> retried = co_await storage::kv::openKv(":memory:");
  retried->close();
  EXPECT_EQ(*opens, 2);
  *opens = 0;
  scope.sdk->beforeOpen = [opens](const std::string&) {
    *opens += 1;
    throw rt::JsError("disk I/O error");
  };
  const std::string message = co_await rejectionMessage(storage::kv::openKv(":memory:"));
  EXPECT_EQ(message, std::string("disk I/O error"));
  EXPECT_EQ(*opens, 1);
}

ASYNC_TEST("openKv guards the native store it opened") {
  const SdkScope scope;
  // The native addon's wrong-type arithmetic error becomes the guard's TypeError.
  scope.sdk->nativeArithmeticFailure = true;
  const rt::KvKey key{"guard", "counter"};
  std::shared_ptr<rt::Kv> opened = co_await storage::kv::openKv(":memory:");
  auto operation = opened->atomic();
  operation->sum(key, 1);
  EXPECT_REJECTS_MESSAGE(operation->commit(), rt::TypeError, "Atomic arithmetic requires an absent key or KvU64 value");
  opened->close();
}

ASYNC_TEST("the default @deno/kv opener is the linked durable store, behind the guard") {
  std::shared_ptr<rt::Kv> memory = co_await storage::kv::openKv(":memory:");
  const rt::KvCommitResult written = co_await memory->set(rt::KvKey{"linked"}, rt::KvValue(1));
  EXPECT(written.ok);
  EXPECT_EQ((co_await memory->get(rt::KvKey{"linked"})).value, rt::KvValue(1));
  // The guard forwards the native store's queue.
  EXPECT(dynamic_cast<rt::KvEnqueue*>(memory.get()) != nullptr);
  memory->close();
  // What the SDK's openKv checks before opening a SQLite store.
  storage::kv::NativeKvOpenOptions options;
  options.implementation = "sqlite";
  auto codecOpening = storage::kv::openNativeKv()(std::string(":memory:"), options);
  const std::string codec = co_await rejectionMessage(std::move(codecOpening));
  EXPECT_EQ(codec, std::string("Bun provides v8.serialize/deserialize, but it uses an incompatible format (JavaScriptCore). Provide explicit 'encodeV8' and "
                               "'decodeV8' functions via options. See https://www.npmjs.com/package/@deno/kv#other-runtimes"));
  options.encodeV8 = rt::kvJscSerialize;
  auto halfOpening = storage::kv::openNativeKv()(std::string(":memory:"), options);
  const std::string half = co_await rejectionMessage(std::move(halfOpening));
  EXPECT_EQ(half, std::string("Provide both 'encodeV8' or 'decodeV8', or neither"));
  options.decodeV8 = rt::kvJscDeserialize;
  options.implementation = "postgres";
  auto implementationOpening = storage::kv::openNativeKv()(std::string(":memory:"), options);
  const std::string implementation = co_await rejectionMessage(std::move(implementationOpening));
  EXPECT_EQ(implementation, std::string("Bad 'opts.implementation': postgres"));
}

// ---- The native store's semantics and durability, through the default opener --------------------
namespace {
  using rt::AtomicCheck;
  using rt::BigInt;
  using rt::KvBytes;
  using rt::KvCommitResult;
  using rt::KvEntry;
  using rt::KvEntryMaybe;
  using rt::KvError;
  using rt::KvKey;
  using rt::KvKeyPart;
  using rt::KvMap;
  using rt::KvU64;
  using rt::KvValue;

  constexpr double INF = std::numeric_limits<double>::infinity();
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();
  const KvCommitResult conflict{false, ""};

  // Open the linked store directly.
  rt::Task<std::shared_ptr<rt::Kv>> openFixture(std::string path) {
      std::shared_ptr<rt::Kv> kv = co_await storage::kv::openKv(std::move(path));
    co_return kv;
  }
  rt::Task<std::shared_ptr<rt::Kv>> openFixture() { return openFixture(":memory:"); }

  // "Name: message" of the rejection, or "resolved".
  template <class T> rt::Task<std::string> outcome(rt::Task<T> task) {
    try {
      co_await std::move(task);
    } catch (const rt::JsError& error) {
      co_return error.toString();
    }
    co_return "resolved";
  }

  template <class F> std::string thrown(F&& function) {
    try {
      function();
    } catch (const rt::JsError& error) {
      return error.toString();
    }
    return "returned";
  }

  std::vector<KvValue> valuesOf(const std::vector<KvEntry<>>& entries) {
    std::vector<KvValue> out;
    for (const auto& entry : entries) out.push_back(entry.value);
    return out;
  }
  template <class... T> std::vector<KvValue> vals(T&&... items) {
    std::vector<KvValue> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  template <class... T> std::vector<KvKey> keys(T&&... items) {
    std::vector<KvKey> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  rt::KvListSelector prefixed(KvKey prefix) {
    rt::KvListSelector selector;
    selector.prefix = std::move(prefix);
    return selector;
  }

  // How this binary was started (the SDK loader, its arguments and the binary).
  std::vector<std::string> selfCommand() {
#if defined(__APPLE__)
    return {uos::rt::process::execPath()};
#else
    const std::vector<std::uint8_t> bytes = rt::fs::readFileSync("/proc/self/cmdline");
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
    const bool viaLoader = !argv.empty() && rt::includes(argv[0], "ld-linux");
    argv.resize(std::min(argv.size(), viaLoader ? std::size_t{4} : std::size_t{1}));
    return argv;
#endif
  }

  rt::process::Subprocess spawnChild(const std::string& test, const std::string& path) {
    std::vector<std::pair<std::string, std::string>> environment;
    environment.emplace_back("UOS_STORAGE_BUN_KV_CHILD", test);
    environment.emplace_back("UOS_STORAGE_BUN_KV_PATH", path);
    environment.emplace_back("TMPDIR", rt::os::tmpdir());
    rt::process::SpawnOptions options;
    options.env = environment;
    options.stdout = rt::process::Stdio::pipe;
    options.stderr = rt::process::Stdio::pipe;
    std::vector<std::string> command = selfCommand();
    command.emplace_back("storage child: " + test);
    return rt::process::spawn(command, options);
  }
}

ASYNC_TEST("native Bun SQLite retains typed values and KvU64 identity") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  const KvValue value = KvValue::object({{"bytes", KvBytes{0, 128, 255}},
                                         {"amount", BigInt::fromString("1208925819614629174706176")},
                                         {"empty", KvValue()},
                                         {"nested", KvValue::array({nullptr, true, "\xce\xbb"})}});
  const KvCommitResult committed = co_await kv->set({"typed"}, value);
  EXPECT(committed.ok);
  EXPECT_EQ((co_await kv->get({"typed"})).value, value);
  co_await kv->set({"counter"}, KvU64(12));
  const KvValue counter = (co_await kv->get({"counter"})).value;
  EXPECT(counter.isU64());
  EXPECT_EQ(counter.asU64().value, 12u);
  co_await kv->set({"undefined"}, KvValue());
  const KvEntryMaybe<> stored = co_await kv->get({"undefined"});
  EXPECT(stored.value.isUndefined());
  EXPECT(stored.versionstamp.has_value());
  kv->close();
}

ASYNC_TEST("atomic compare-and-set refuses stale checks without partial writes") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  const KvEntryMaybe<> absent = co_await kv->get({"quota"});
  auto operation = kv->atomic();
  operation->check(absent).set({"quota"}, KvValue::object({{"reserved", 1}})).set({"audit"}, "created");
  EXPECT((co_await operation->commit()).ok);
  const auto entries = co_await kv->getMany(keys(KvKey{"quota"}, KvKey{"audit"}));
  EXPECT_EQ(entries[0].versionstamp, entries[1].versionstamp);
  auto stale = kv->atomic();
  stale->check(absent).set({"quota"}, KvValue::object({{"reserved", 2}})).set({"partial"}, true);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv->get({"partial"})).versionstamp);
  EXPECT_EQ(entries[0].value, KvValue::object({{"reserved", 1}}));
  kv->close();
}

ASYNC_TEST("native check budget preserves paired analytics sums with two and ten caller checks") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  const std::vector<std::pair<std::string, std::int64_t>> counters{
    {"sample_count", 1}, {"input_tokens", 100}, {"cached_input_tokens", 25}, {"usage_reported_sample_count", 1}, {"request_cache_hit_sample_count", 1}};
  const std::vector<int> checkCounts{2, 10};
  for (const int checkCount : checkCounts) {
    const std::string prefix = "analytics-" + std::to_string(checkCount);
    std::vector<KvKey> fenceKeys;
    for (int index = 0; index < checkCount; ++index) fenceKeys.push_back({prefix, "fence", index});
    const auto fences = co_await kv->getMany(fenceKeys);
    auto operation = kv->atomic();
    operation->check(fences).set({prefix, "marker"}, true).set({prefix, "cardinality"}, 1);
    for (const auto& [counter, amount] : counters) operation->sum({prefix, "dimension", counter}, amount).sum({prefix, "aggregate", counter}, amount);
    EXPECT((co_await operation->commit()).ok);
    for (const auto& [counter, amount] : counters) {
      EXPECT_EQ((co_await kv->get({prefix, "dimension", counter})).value, KvValue(KvU64(amount)));
      EXPECT_EQ((co_await kv->get({prefix, "aggregate", counter})).value, KvValue(KvU64(amount)));
    }
  }
  kv->close();
}

ASYNC_TEST("maximum caller checks retain stale CAS and late wrong-type rollback without partial counters") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  std::vector<KvKey> fenceKeys;
  for (int index = 0; index < 10; ++index) fenceKeys.push_back({"max-fence", index});
  const auto fences = co_await kv->getMany(fenceKeys);
  co_await kv->set(fenceKeys[9], "changed");
  auto stale = kv->atomic();
  stale->check(fences).set({"stale-marker"}, true).sum({"stale-sum"}, 1);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv->get({"stale-marker"})).versionstamp);
  EXPECT(!(co_await kv->get({"stale-sum"})).versionstamp);
  const auto current = co_await kv->getMany(fenceKeys);
  co_await kv->set({"late-wrong-type"}, BigInt(7));
  auto failed = kv->atomic();
  failed->check(current).set({"late-marker"}, true);
  for (int index = 0; index < 9; index += 1) failed->sum({"late-sum", index}, 1);
  failed->sum({"late-wrong-type"}, 1);
  EXPECT_EQ(co_await outcome(failed->commit()), std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT(!(co_await kv->get({"late-marker"})).versionstamp);
  for (int index = 0; index < 9; index += 1) EXPECT(!(co_await kv->get({"late-sum", index})).versionstamp);
  EXPECT_EQ((co_await kv->get({"late-wrong-type"})).value, KvValue(BigInt(7)));
  kv->close();
}

namespace {
  // `kv.list = listOverride; kv.atomic = atomicOverride;` on the returned facade.
  struct CountingFacade : rt::ForwardingKv {
    using rt::ForwardingKv::ForwardingKv;
    int lists = 0;
    int atomics = 0;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      lists += 1;
      return rt::ForwardingKv::list(std::move(selector), std::move(options));
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      atomics += 1;
      return rt::ForwardingKv::atomic();
    }
  };
}

ASYNC_TEST("native facade permits captured list and atomic instance overrides without recursion") {
  std::shared_ptr<rt::Kv> native = co_await openFixture();
  CountingFacade kv(*native);
  co_await kv.set({"facade"}, "root");
  co_await kv.set({"facade", 1}, "child");
  rt::KvListOptions one;
  one.limit = 1;
  auto iterator = kv.list(prefixed(KvKey{"facade"}), one);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*iterator)), vals("child"));
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
  EXPECT(rt::includes(co_await outcome(partial->commit()), "KvU64"));
  EXPECT(!(co_await kv.get({"facade-partial"})).versionstamp);
  KvError error;
  error.message = "fixture";
  error.cause = std::make_shared<KvValue>("unsupported");
  auto valuePartial = kv.atomic();
  valuePartial->set({"facade-value-partial"}, true).set({"facade-value"}, error);
  EXPECT(rt::includes(co_await outcome(valuePartial->commit()), "Error.cause"));
  EXPECT(!(co_await kv.get({"facade-value-partial"})).versionstamp);
  EXPECT_EQ(kv.atomics, 4);
  native->close();
}

ASYNC_TEST("native TTL retains its absolute deadline and expires across restart") {
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> kv = co_await openFixture(path);
  const double before = rt::dateNow();
  co_await kv->set({"expiring"}, "challenge", {.expireIn = 20});
  const double after = rt::dateNow();
  co_await kv->set({"durable"}, "ledger");
  storage::sqlite_readonly::DatabaseOptions options;
  options.readonly = true;
  Database raw(path, options);
  const std::optional<json::object> row = raw.query("SELECT expiration_ms FROM kv WHERE expiration_ms >= 0").get();
  raw.close(true);
  ASSERT(row.has_value());
  const double expiry = *rt::getNumber(*row, "expiration_ms");
  EXPECT(expiry >= before + 20);
  EXPECT(expiry <= after + 20);
  // Native TTL permits delayed visibility; startup sweeps expired rows directly.
  kv->close();
  rt::test::clock().advance(after + 25 - rt::dateNow());
  std::shared_ptr<rt::Kv> reopened = co_await openFixture(path);
  EXPECT(!(co_await reopened->get({"expiring"})).versionstamp);
  EXPECT_EQ((co_await reopened->get({"durable"})).value, KvValue("ledger"));
  reopened->close();
}

ASYNC_TEST("native list preserves heterogeneous key order and resumable ranges") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  const std::vector<KvKeyPart> parts{KvBytes{0}, KvBytes{2}, KvBytes{10}, "a", "\xc3\xa9", BigInt(-2), BigInt(0), BigInt(2), -2, 0, 2, false, true};
  std::vector<KvValue> indexes;
  for (std::size_t index = 0; index < parts.size(); index += 1) {
    co_await kv->set({"ordered", parts[index]}, index);
    indexes.emplace_back(index);
  }
  auto all = kv->list(prefixed(KvKey{"ordered"}));
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*all)), indexes);
  rt::KvListOptions paged;
  paged.limit = 3;
  paged.batchSize = 2;
  auto first = kv->list(prefixed(KvKey{"ordered"}), paged);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*first)), vals(0, 1, 2));
  rt::KvListOptions resume;
  resume.cursor = first->cursor();
  auto resumed = kv->list(prefixed(KvKey{"ordered"}), resume);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*resumed)), std::vector<KvValue>(indexes.begin() + 3, indexes.end()));
  rt::KvListSelector bounds;
  bounds.start = KvKey{"ordered", -2};
  bounds.end = KvKey{"ordered", 2};
  auto range = kv->list(bounds);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*range)), vals(8, 9));
  rt::KvListOptions backwards;
  backwards.limit = 2;
  backwards.reverse = true;
  auto reverse = kv->list(prefixed(KvKey{"ordered"}), backwards);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*reverse)), vals(12, 11));
  kv->close();
}

ASYNC_TEST("prefix selectors exclude the exact prefix and native cursors retain cross-range behavior") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  co_await kv->set({"prefix"}, "root");
  co_await kv->set({"prefix", 1}, "child");
  co_await kv->set({"prefix", 2}, "next");
  rt::KvListOptions one;
  one.limit = 1;
  auto first = kv->list(prefixed(KvKey{"prefix"}), one);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*first)), vals("child"));
  rt::KvListOptions crossing;
  crossing.cursor = first->cursor();
  auto other = kv->list(prefixed(KvKey{"other"}), crossing);
  EXPECT((co_await rt::kvListToArray(*other)).empty());
  rt::KvListSelector outside = prefixed(KvKey{"prefix"});
  outside.end = KvKey{"outside", 1};
  EXPECT(rt::includes(thrown([&] { (void)kv->list(outside); }), "keyspace"));
  rt::KvListSelector bounded = prefixed(KvKey{"prefix"});
  bounded.end = KvKey{"prefix", 2};
  auto rows = kv->list(bounded);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*rows)), vals("child"));
  kv->close();
}

ASYNC_TEST("native cursor lifecycle clears empty and exhausted scans but preserves limited continuations") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  auto empty = kv->list(prefixed(KvKey{"empty-cursor"}));
  EXPECT(rt::includes(thrown([&] { (void)empty->cursor(); }), "before first iteration"));
  EXPECT((co_await rt::kvListToArray(*empty)).empty());
  EXPECT_EQ(empty->cursor(), std::string(""));
  co_await kv->set({"cursor", 1}, "first");
  auto single = kv->list(prefixed(KvKey{"cursor"}));
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*single)), vals("first"));
  EXPECT_EQ(single->cursor(), std::string(""));
  co_await kv->set({"cursor", 2}, "second");
  rt::KvListOptions one;
  one.limit = 1;
  auto limited = kv->list(prefixed(KvKey{"cursor"}), one);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*limited)), vals("first"));
  const std::string continuation = limited->cursor();
  EXPECT(continuation != "");
  rt::KvListOptions resume;
  resume.cursor = continuation;
  auto resumed = kv->list(prefixed(KvKey{"cursor"}), resume);
  EXPECT_EQ(resumed->cursor(), continuation);
  EXPECT_EQ(valuesOf(co_await rt::kvListToArray(*resumed)), vals("second"));
  EXPECT_EQ(resumed->cursor(), std::string(""));
  kv->close();
}

ASYNC_TEST("unsigned sums wrap, retain real U64 values, and roll back wrong-type mutations") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  co_await kv->set({"sum"}, KvU64(std::numeric_limits<std::uint64_t>::max()));
  auto wrap = kv->atomic();
  wrap->sum({"sum"}, 1);
  EXPECT((co_await wrap->commit()).ok);
  EXPECT_EQ((co_await kv->get({"sum"})).value, KvValue(KvU64(0)));
  auto missing = kv->atomic();
  missing->sum({"missing-sum"}, 5);
  co_await missing->commit();
  EXPECT((co_await kv->get({"missing-sum"})).value.isU64());
  co_await kv->set({"wrong-type"}, BigInt(5));
  auto rollback = kv->atomic();
  rollback->set({"must-roll-back"}, "partial").sum({"wrong-type"}, 1);
  EXPECT(co_await outcome(rollback->commit()) != std::string("resolved"));
  EXPECT(!(co_await kv->get({"must-roll-back"})).versionstamp);
  EXPECT_EQ((co_await kv->get({"wrong-type"})).value, KvValue(BigInt(5)));
  kv->close();
}

ASYNC_TEST("snapshot reads preserve absent values and clone caller-owned buffers") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  KvBytes owned{1, 2, 3};
  co_await kv->set({"bytes"}, owned);
  owned[0] = 99;
  auto entries = co_await kv->getMany(keys(KvKey{"bytes"}, KvKey{"missing"}));
  const KvValue expected(KvBytes{1, 2, 3});
  EXPECT_EQ(entries[0].value, expected);
  const KvEntryMaybe<> absent{KvKey{"missing"}, nullptr, std::nullopt};
  EXPECT(entries[1] == absent);
  entries[0].value.as<KvBytes>()[1] = 99;
  EXPECT_EQ((co_await kv->get({"bytes"})).value, expected);
  kv->close();
}

ASYNC_TEST("concurrent native arithmetic retains updates while caller CAS failure remains final") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  co_await kv->set({"guard-counter"}, KvU64(0));
  auto first = kv->atomic();
  first->sum({"guard-counter"}, 1);
  auto second = kv->atomic();
  second->sum({"guard-counter"}, 1);
  rt::Promise<KvCommitResult> one(first->commit());
  rt::Promise<KvCommitResult> two(second->commit());
  auto both = rt::all(one, two);
  const auto committed = co_await both.wait();
  const auto& a = std::get<0>(committed);
  const auto& b = std::get<1>(committed);
  EXPECT(a.ok && b.ok);
  EXPECT_EQ((co_await kv->get({"guard-counter"})).value, KvValue(KvU64(2)));
  const KvEntryMaybe<> stale = co_await kv->get({"caller-fence"});
  co_await kv->set({"caller-fence"}, true);
  auto fenced = kv->atomic();
  fenced->check(stale).sum({"guard-counter"}, 1);
  EXPECT_EQ(co_await fenced->commit(), conflict);
  EXPECT_EQ((co_await kv->get({"guard-counter"})).value, KvValue(KvU64(2)));
  kv->close();
}

ASYNC_TEST("numeric key order distinguishes signed zeros and retains infinity and canonical NaN") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  const std::vector<double> numbers{-INF, -2, -0.0, 0, 2, INF, NAN_};
  std::vector<KvValue> indexes;
  for (std::size_t index = 0; index < numbers.size(); index += 1) {
    co_await kv->set({"numeric", numbers[index]}, index);
    indexes.emplace_back(index);
  }
  auto iterator = kv->list(prefixed(KvKey{"numeric"}));
  const auto rows = co_await rt::kvListToArray(*iterator);
  EXPECT_EQ(valuesOf(rows), indexes);
  EXPECT(std::signbit(rows[2].key[1].number()));
  EXPECT(!std::signbit(rows[3].key[1].number()));
  kv->close();
}

ASYNC_TEST("Error cause graphs fail before any atomic mutation can lose persisted fields") {
  std::shared_ptr<rt::Kv> kv = co_await openFixture();
  KvError inner;
  inner.message = "inner";
  KvError error;
  error.message = "outer";
  error.cause = std::make_shared<KvValue>(inner);
  const KvValue graph = KvValue::object({{"records", KvMap{{"failure", error}}}});
  auto operation = kv->atomic();
  operation->set({"cause-partial"}, "must not publish").set({"cause"}, graph);
  EXPECT(rt::includes(co_await outcome(operation->commit()), "Error.cause"));
  EXPECT(!(co_await kv->get({"cause-partial"})).versionstamp);
  EXPECT(!(co_await kv->get({"cause"})).versionstamp);
  kv->close();
}

ASYNC_TEST("two native handles share CAS fencing and monotonic persisted revisions") {
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> first = co_await openFixture(path);
  std::shared_ptr<rt::Kv> second = co_await openFixture(path);
  const KvEntryMaybe<> absent = co_await first->get({"claim"});
  auto a = first->atomic();
  a->check(absent).set({"claim"}, "first");
  auto b = second->atomic();
  b->check(absent).set({"claim"}, "second");
  rt::Promise<KvCommitResult> ra(a->commit());
  rt::Promise<KvCommitResult> rb(b->commit());
  auto both = rt::all(ra, rb);
  const auto committed = co_await both.wait();
  const auto& one = std::get<0>(committed);
  const auto& two = std::get<1>(committed);
  EXPECT_EQ(static_cast<int>(one.ok) + static_cast<int>(two.ok), 1);
  const KvEntryMaybe<> before = co_await first->get({"claim"});
  co_await second->delete_({"claim"});
  co_await second->set({"claim"}, "replacement");
  const KvEntryMaybe<> after = co_await first->get({"claim"});
  EXPECT(after.versionstamp != before.versionstamp);
  EXPECT(after.versionstamp.value_or("") > before.versionstamp.value_or(""));
  auto staleOwner = first->atomic();
  staleOwner->check(before).set({"stale-owner"}, true);
  EXPECT_EQ(co_await staleOwner->commit(), conflict);
  first->close();
  second->close();
}

ASYNC_TEST("two native handles preserve ten caller checks under mixed counter-type updates") {
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> first = co_await openFixture(path);
  std::shared_ptr<rt::Kv> second = co_await openFixture(path);
  std::vector<KvKey> fenceKeys;
  for (int index = 0; index < 10; ++index) fenceKeys.push_back({"mixed-fence", index});
  const auto fences = co_await first->getMany(fenceKeys);
  co_await first->set({"mixed-counter"}, KvU64(0));
  auto writer = [second]() -> rt::Task<void> {
    for (int index = 0; index < 40; index += 1) {
      co_await second->set({"mixed-counter"}, BigInt(3));
      co_await second->set({"mixed-counter"}, KvU64(0));
    }
  };
  auto outcomes = std::make_shared<std::vector<bool>>();
  auto committer = [first, fences, outcomes]() -> rt::Task<void> {
    for (int index = 0; index < 20; index += 1) {
      auto operation = first->atomic();
      operation->check(fences).set({"mixed-marker", index}, true).sum({"mixed-side", index}, 1).sum({"mixed-counter"}, 1);
      bool ok = false;
      try {
        const KvCommitResult result = co_await operation->commit();
        EXPECT(result.ok);
        ok = true;
      } catch (const rt::TypeError& error) {
        if (!rt::includes(error.message, "KvU64")) throw;
      }
      outcomes->push_back(ok);
    }
  };
  rt::Promise<void> writing(rt::async(writer));
  rt::Promise<void> committing(rt::async(committer));
  auto both = rt::all(writing, committing);
  co_await both.wait();
  EXPECT_EQ(outcomes->size(), 20u);
  for (std::size_t index = 0; index < outcomes->size(); index += 1) {
    EXPECT_EQ((co_await first->get({"mixed-marker", index})).value, (*outcomes)[index] ? KvValue(true) : KvValue(nullptr));
    EXPECT_EQ((co_await first->get({"mixed-side", index})).value, (*outcomes)[index] ? KvValue(KvU64(1)) : KvValue(nullptr));
  }
  first->close();
  second->close();
}

// The worker of "separate Bun processes preserve each quota increment under contention".
TEST("storage child: quota") {
  if (rt::env::get("UOS_STORAGE_BUN_KV_CHILD") != std::optional<std::string>("quota")) return;
  const std::string path = *rt::env::get("UOS_STORAGE_BUN_KV_PATH");
  rt::blockOn(rt::async([path]() -> rt::Task<void> {
    std::shared_ptr<rt::Kv> kv = co_await storage::kv::openKv(path);
    for (int n = 0; n < 20; n += 1) {
      bool committed = false;
      for (int retry = 0; retry < 200; retry += 1) {
        const KvEntryMaybe<> entry = co_await kv->get({"count"});
        const double next = (entry.value.isNumber() ? entry.value.asNumber() : 0) + 1;
        auto operation = kv->atomic();
        operation->check(entry).set({"count"}, next);
        if ((co_await operation->commit()).ok) {
          committed = true;
          break;
        }
      }
      if (!committed) throw rt::JsError("quota contention did not settle");
    }
    kv->close();
  }));
}

ASYNC_TEST("separate Bun processes preserve each quota increment under contention") {
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> kv = co_await openFixture(path);
  co_await kv->set({"count"}, 0);
  std::vector<rt::process::Subprocess> children;
  children.push_back(spawnChild("quota", path));
  children.push_back(spawnChild("quota", path));
  for (const rt::process::Subprocess& child : children) {
    const rt::Response errors{rt::BodyInit(child.stderr())};
    auto reading = errors.text();
    const std::string stderrText = co_await std::move(reading);
    auto exited = child.exited();
    const int code = co_await exited.wait();
    EXPECT_EQ(code, 0);
    EXPECT_EQ(stderrText, std::string(""));
  }
  EXPECT_EQ((co_await kv->get({"count"})).value, KvValue(40));
  kv->close();
}

// The worker of "an acknowledged transaction survives abrupt writer process termination".
TEST("storage child: crash") {
  if (rt::env::get("UOS_STORAGE_BUN_KV_CHILD") != std::optional<std::string>("crash")) return;
  const std::string path = *rt::env::get("UOS_STORAGE_BUN_KV_PATH");
  rt::blockOn(rt::async([path]() -> rt::Task<void> {
    std::shared_ptr<rt::Kv> kv = co_await storage::kv::openKv(path);
    auto operation = kv->atomic();
    operation->set({"reservation"}, KvValue::object({{"request", "fixture"}, {"amount", BigInt(3)}})).set({"billing"}, KvValue::object({{"pending", true}}));
    co_await operation->commit();
    std::printf("committed\n");
    std::fflush(stdout);
    co_await rt::sleep(60'000);
  }));
}

ASYNC_TEST("an acknowledged transaction survives abrupt writer process termination") {
  const Fixtures fixtures;
  const std::string path = fixtures.databasePath();
  std::shared_ptr<rt::Kv> initialized = co_await openFixture(path);
  initialized->close();
  const rt::process::Subprocess child = spawnChild("crash", path);
  rt::ReadableStreamDefaultReader<std::vector<std::uint8_t>> reader = child.stdout().getReader();
  auto reading = reader.read();
  const rt::ReadableStreamReadResult<std::vector<std::uint8_t>> chunk = co_await std::move(reading);
  ASSERT(!chunk.done);
  EXPECT(rt::includes(std::string(chunk.value->begin(), chunk.value->end()), "committed"));
  auto cancelling = reader.cancel();
  co_await std::move(cancelling);
  child.kill("SIGKILL");
  auto exited = child.exited();
  co_await exited.wait();
  EXPECT_EQ(child.signalCode(), std::optional<std::string>("SIGKILL"));
  std::shared_ptr<rt::Kv> recovered = co_await openFixture(path);
  const auto entries = co_await recovered->getMany(keys(KvKey{"reservation"}, KvKey{"billing"}));
  EXPECT_EQ(entries[0].value, KvValue::object({{"request", "fixture"}, {"amount", BigInt(3)}}));
  EXPECT_EQ(entries[1].value, KvValue::object({{"pending", true}}));
  EXPECT_EQ(entries[0].versionstamp, entries[1].versionstamp);
  recovered->close();
}
