#include "uos/storage/kv.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/storage/native_kv.hpp"
#include "uos/storage/sqlite_readonly.hpp"

#include <exception>
#include <optional>
#include <utility>

namespace uos::storage::kv {
  namespace {
    /** Inspect the marker before the native backend can migrate or mutate a file. */
    void verifyCodec(const std::string& path) {
      // `table ? query(...).get()?.value : null`: only a string can equal the codec.
      const std::optional<std::string> codec = sqlite_readonly::inspectSqliteReadOnly(path, [](sqlite_readonly::Database& database) -> std::optional<std::string> {
        const std::optional<json::object> table = database.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'uos_kv_metadata'").get();
        if (!table) return std::nullopt;
        const std::optional<json::object> row = database.query("SELECT value FROM uos_kv_metadata WHERE key = 'codec'").get();
        if (!row) return std::nullopt;
        const std::optional<std::string_view> value = rt::getString(*row, "value");
        return value ? std::optional<std::string>(*value) : std::nullopt;
      });
      if (codec != BUN_KV_CODEC) {
        throw rt::JsError("KV codec is missing or incompatible; convert the legacy database offline before opening it with Bun");
      }
    }

    void prepareDatabase(const std::string& path) {
      try {
        const rt::fs::Stats file = rt::fs::lstatSync(path);
        if (!file.isFile()) throw rt::JsError("KV database must be a regular file, not a symlink or directory");
        verifyCodec(path);
        return;
      } catch (const rt::fs::ErrnoError& error) {
        if (error.code != "ENOENT") throw;
      }

      rt::fs::MkdirOptions directoryOptions;
      directoryOptions.recursive = true;
      rt::fs::mkdirSync(rt::path::dirname(path), directoryOptions);
      // Exclusive creation cannot adopt a legacy file planted between stat and open.
      const int descriptor = rt::fs::openSync(path, "wx", 0600);
      rt::fs::closeSync(descriptor);
      rt::sqlite::DatabaseOptions options;
      options.strict = true;
      const rt::sqlite::Database database(path, options);
      rt::Reason failure;
      try {
        database
          .transaction([&database] {
            database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
            database.run("INSERT INTO uos_kv_metadata (key, value) VALUES (?, ?)", "codec", BUN_KV_CODEC);
            database.run("INSERT INTO uos_kv_metadata (key, value) VALUES (?, ?)", "created_bun_version", rt::bun::version);
          })
          .immediate();
      } catch (...) {
        failure = std::current_exception();
      }
      database.close(true);
      if (failure) std::rethrow_exception(failure);
    }

    rt::KvBytes encodeV8(const rt::KvValue& value) {
      assertPortableKvValue(value);
      return rt::kvJscSerialize(value);
    }

    // The linked SQLite store uses the explicitly selected value codec.
    rt::Task<std::shared_ptr<rt::Kv>> openLinkedNativeKv(std::string path, NativeKvOpenOptions options) {
      const std::string& implementation = options.implementation;
      if (!(implementation.empty() || implementation == "in-memory" || implementation == "sqlite" || implementation == "remote"))
        throw rt::TypeError("Bad 'opts.implementation': " + implementation);
      if (path.empty() || implementation == "in-memory") throw rt::JsError("The @deno/kv in-memory implementation is not linked into the C++ runtime");
      if (static_cast<bool>(options.encodeV8) != static_cast<bool>(options.decodeV8)) throw rt::JsError("Provide both 'encodeV8' or 'decodeV8', or neither");
      if (!options.encodeV8)
        throw rt::JsError("Bun provides v8.serialize/deserialize, but it uses an incompatible format (JavaScriptCore). Provide explicit 'encodeV8' and 'decodeV8' "
                          "functions via options. See https://www.npmjs.com/package/@deno/kv#other-runtimes");
      static const rt::Regex REMOTE("^https?:\\/\\/", "i");
      if (REMOTE.test(path) || implementation == "remote") throw rt::JsError("The @deno/kv remote implementation is not linked into the C++ runtime");
      rt::SqliteKvOptions store;
      store.encodeV8 = std::move(options.encodeV8);
      store.decodeV8 = std::move(options.decodeV8);
      std::shared_ptr<rt::Kv> kv;
      try {
        kv = std::make_shared<rt::SqliteKv>(path, std::move(store));
      } catch (const rt::sqlite::SQLiteError& error) {
        // Preserve the storage facade's error contract.
        throw rt::JsError(error.message);
      }
      co_return kv;
    }
  }

  NativeKvOpener& openNativeKv() {
    static NativeKvOpener opener = openLinkedNativeKv;
    return opener;
  }

  V8Deserialize& deserialize() {
    static V8Deserialize reader = rt::kvJscDeserialize;
    return reader;
  }

  rt::Task<std::shared_ptr<rt::Kv>> openKv(std::string path) {
    static const rt::Regex REMOTE_URL("^https?:\\/\\/", "i");
    if (path.empty() || REMOTE_URL.test(path)) throw rt::JsError("Bun KV requires an explicit local SQLite path or :memory:");
    const std::string target = path == ":memory:" ? path : rt::path::resolve(path);
    std::function<rt::Task<std::shared_ptr<rt::Kv>>()> open = [target]() -> rt::Task<std::shared_ptr<rt::Kv>> {
      if (target != ":memory:") prepareDatabase(target);
      NativeKvOpenOptions options;
      options.implementation = "sqlite";
      options.encodeV8 = encodeV8;
      options.decodeV8 = [](std::span<const std::uint8_t> bytes) { return deserialize()(bytes); };
      rt::Task<std::shared_ptr<rt::Kv>> opening = openNativeKv()(target, std::move(options));
      std::shared_ptr<rt::Kv> native = co_await std::move(opening);
      std::shared_ptr<rt::Kv> guarded = native_kv::guardNativeKv(std::move(native));
      co_return guarded;
    };
    rt::Task<std::shared_ptr<rt::Kv>> retried = native_kv::retrySqliteBusy<std::shared_ptr<rt::Kv>>(std::move(open));
    std::shared_ptr<rt::Kv> kv = co_await std::move(retried);
    co_return kv;
  }
}
