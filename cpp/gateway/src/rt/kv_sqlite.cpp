// rt::SqliteKv (uos/rt/kv_sqlite.hpp): the SDK layer (kv_sdk) over a C++ port of denokv's
// SQLite backend at 9fe65cc with atomic-write rollback. See packaging/DENOKV-LICENSE. The
// migrations, snapshot reads, atomic writes with a savepoint each, the expiry sweep and the
// queue's insert and cleanup, run by one worker thread per handle.
#include "kv_sdk.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/sqlite.hpp"
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <random>
#include <thread>

namespace uos::rt {
  namespace {
    using kv_sdk::VE_BYTES;
    using kv_sdk::VE_LE64;
    using kv_sdk::VE_V8;

    // ---- backend.rs -----------------------------------------------------------------------------

    constexpr std::string_view STATEMENT_INC_AND_GET_DATA_VERSION = "update data_version set version = version + ? where k = 0 returning version";
    constexpr std::string_view STATEMENT_GET_DATA_VERSION = "select version from data_version where k = 0";
    constexpr std::string_view STATEMENT_KV_RANGE_SCAN = "select k, v, v_encoding, version from kv where k >= ? and k < ? order by k asc limit ?";
    constexpr std::string_view STATEMENT_KV_RANGE_SCAN_REVERSE = "select k, v, v_encoding, version from kv where k >= ? and k < ? order by k desc limit ?";
    constexpr std::string_view STATEMENT_KV_POINT_GET_VALUE_ONLY = "select v, v_encoding from kv where k = ?";
    constexpr std::string_view STATEMENT_KV_POINT_GET_VERSION_ONLY = "select version from kv where k = ?";
    constexpr std::string_view STATEMENT_KV_POINT_SET =
      "insert into kv (k, v, v_encoding, version, expiration_ms) values (:k, :v, :v_encoding, :version, :expiration_ms) on conflict(k) do update set v = :v, "
      "v_encoding = :v_encoding, version = :version, expiration_ms = :expiration_ms";
    constexpr std::string_view STATEMENT_KV_POINT_DELETE = "delete from kv where k = ?";
    constexpr std::string_view STATEMENT_DELETE_ALL_EXPIRED = "delete from kv where expiration_ms >= 0 and expiration_ms <= ? returning k";
    constexpr std::string_view STATEMENT_EARLIEST_EXPIRATION = "select expiration_ms from kv where expiration_ms >= 0 order by expiration_ms limit 1";
    constexpr std::string_view STATEMENT_QUEUE_ADD_READY = "insert into queue (ts, id, data, backoff_schedule, keys_if_undelivered) values(?, ?, ?, ?, ?)";
    constexpr std::string_view STATEMENT_QUEUE_REMOVE_RUNNING = "delete from queue_running where id = ?";
    constexpr std::string_view STATEMENT_QUEUE_GET_RUNNING_BY_ID = "select deadline, id, data, backoff_schedule, keys_if_undelivered from queue_running where id = ?";
    constexpr std::string_view STATEMENT_QUEUE_GET_RUNNING_PAST_DEADLINE = "select id from queue_running where deadline <= ? order by deadline limit 100";
    constexpr std::string_view STATEMENT_CREATE_MIGRATION_TABLE = "\ncreate table if not exists migration_state(\n  k integer not null primary key,\n  version integer not null\n)\n";

    // MIGRATIONS, statement by statement in backend.rs's text: SQLite keeps each CREATE's
    // text in sqlite_master and ALTER TABLE splices the column into it, so the schema the
    // conversion tools compare comes out identical.
    const std::array<std::vector<std::string_view>, 3>& migrations() {
      static const std::array<std::vector<std::string_view>, 3> statements = {
        std::vector<std::string_view>{
          "create table data_version (\n  k integer primary key,\n  version integer not null\n)",
          "insert into data_version (k, version) values (0, 0)",
          "create table kv (\n  k blob primary key,\n  v blob not null,\n  v_encoding integer not null,\n  version integer not null\n) without rowid",
        },
        std::vector<std::string_view>{
          "create table queue (\n  ts integer not null,\n  id text not null,\n  data blob not null,\n  backoff_schedule text not null,\n  keys_if_undelivered blob not "
          "null,\n\n  primary key (ts, id)\n)",
          "create table queue_running(\n  deadline integer not null,\n  id text not null,\n  data blob not null,\n  backoff_schedule text not null,\n  "
          "keys_if_undelivered blob not null,\n\n  primary key (deadline, id)\n)",
        },
        std::vector<std::string_view>{
          "alter table kv add column seq integer not null default 0",
          "alter table data_version add column seq integer not null default 0",
          "alter table kv add column expiration_ms integer not null default -1",
          "create index kv_expiration_ms_idx on kv (expiration_ms)",
        },
      };
      return statements;
    }

    // The default backoff schedule of a message enqueued without one.
    constexpr std::array<std::uint32_t, 5> DEFAULT_BACKOFF_SCHEDULE = {100, 1000, 5000, 30000, 60000};

    // lib.rs: the sweep and cleanup cadence.
    constexpr double QUEUE_RUNNING_CLEANUP_INTERVAL_MS = 60'000;

    // rusqlite's default busy timeout.
    constexpr std::string_view BUSY_TIMEOUT = "PRAGMA busy_timeout = 5000";

    constexpr int SQLITE_BUSY = 5;
    constexpr int SQLITE_READONLY = 8;

    int primaryCode(const sqlite::SQLiteError& error) { return error.errno_ & 0xFF; }

    // Errors cross the napi boundary as plain Errors with the backend's message.
    std::exception_ptr napiError(std::exception_ptr error) {
      try {
        std::rethrow_exception(error);
      } catch (const sqlite::SQLiteError& failure) {
        return std::make_exception_ptr(JsError(failure.message));
      } catch (const JsError&) {
        return error;
      } catch (const std::exception& failure) {
        return std::make_exception_ptr(JsError(failure.what()));
      }
    }

    double systemNowMs() {
      return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    }

    KvBytes versionstampBytes(std::int64_t version) {
      KvBytes bytes(10, 0);
      const auto bits = static_cast<std::uint64_t>(version);
      for (int index = 0; index < 8; ++index) bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(bits >> (56 - 8 * index));
      return bytes;
    }

    KvBytes encodeLe64(std::uint64_t value) {
      KvBytes out(8);
      for (std::size_t index = 0; index < 8; ++index) out[index] = static_cast<std::uint8_t>(value >> (8 * index));
      return out;
    }

    std::uint64_t decodeLe64(const KvBytes& bytes) {
      std::uint64_t value = 0;
      for (std::size_t index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
      return value;
    }

    // rusqlite's typed column reads: InvalidColumnType for another storage class.
    const sqlite::Value& column(const sqlite::Row& row, std::string_view name) {
      const sqlite::Value* value = row.get(name);
      if (!value) throw JsError("Invalid column name: " + std::string(name));
      return *value;
    }
    [[noreturn]] void invalidColumnType(const sqlite::Value& value, std::size_t index, std::string_view name) {
      static constexpr std::array<std::string_view, 5> types = {"Null", "Integer", "Real", "Text", "Blob"};
      throw JsError("Invalid column type " + std::string(types[value.index()]) + " at index: " + std::to_string(index) + ", name: " + std::string(name));
    }
    KvBytes blobColumn(const sqlite::Row& row, std::string_view name, std::size_t index) {
      const sqlite::Value& value = column(row, name);
      if (const sqlite::Bytes* bytes = sqlite::getBytes(&value)) return *bytes;
      invalidColumnType(value, index, name);
    }
    KvBytes takeBlobColumn(sqlite::Row& row, std::string_view name, std::size_t index) {
      auto* value = row.get(name);
      if (!value) throw JsError("Invalid column name: " + std::string(name));
      if (auto* bytes = std::get_if<sqlite::Bytes>(value)) return std::move(*bytes);
      invalidColumnType(*value, index, name);
    }
    std::int64_t integerColumn(const sqlite::Row& row, std::string_view name, std::size_t index) {
      const sqlite::Value& value = column(row, name);
      if (const std::int64_t* integer = sqlite::getBigInt(&value)) return *integer;
      invalidColumnType(value, index, name);
    }
    std::string textColumn(const sqlite::Row& row, std::string_view name, std::size_t index) {
      const sqlite::Value& value = column(row, name);
      if (const std::string* text = sqlite::getString(&value)) return *text;
      invalidColumnType(value, index, name);
    }

    // serde_json::to_string of a Vec<Vec<u8>> or a [u32]/[u64] slice.
    std::string jsonArray(const std::vector<KvBytes>& keys) {
      std::string out = "[";
      for (std::size_t index = 0; index < keys.size(); ++index) {
        out += index ? ",[" : "[";
        for (std::size_t byte = 0; byte < keys[index].size(); ++byte) out += (byte ? "," : "") + std::to_string(keys[index][byte]);
        out += "]";
      }
      return out + "]";
    }
    template <class T> std::string jsonArray(std::span<const T> numbers) {
      std::string out = "[";
      for (std::size_t index = 0; index < numbers.size(); ++index) out += (index ? "," : "") + std::to_string(numbers[index]);
      return out + "]";
    }

    // serde_json::from_str::<Option<Vec<u64>>> / ::<Vec<Vec<u8>>> of the queue's columns.
    std::uint64_t jsonUnsigned(const json::value& value, std::uint64_t maximum, std::string_view what) {
      if (value.is_int64() && value.as_int64() >= 0 && static_cast<std::uint64_t>(value.as_int64()) <= maximum) return static_cast<std::uint64_t>(value.as_int64());
      if (value.is_uint64() && value.as_uint64() <= maximum) return value.as_uint64();
      throw JsError("invalid " + std::string(what) + " in queue message: " + json::serialize(value));
    }
    std::vector<std::uint64_t> parseBackoffSchedule(const std::string& text) {
      if (text.size() > limits::kJsonDocumentBytes) throw JsError("Queue backoff schedule exceeds JSON limit");
      const json::value parsed = json::parse(text);
      std::vector<std::uint64_t> schedule;
      if (parsed.is_null()) return schedule;
      if (!parsed.is_array()) throw JsError("invalid backoff schedule in queue message: " + text);
      for (const json::value& item : parsed.as_array()) schedule.push_back(jsonUnsigned(item, std::numeric_limits<std::uint64_t>::max(), "backoff interval"));
      return schedule;
    }
    std::vector<KvBytes> parseKeys(const std::string& text) {
      if (text.size() > limits::kJsonDocumentBytes) throw JsError("Queue undelivered keys exceed JSON limit");
      const json::value parsed = json::parse(text);
      if (!parsed.is_array()) throw JsError("invalid undelivered keys in queue message: " + text);
      std::vector<KvBytes> keys;
      for (const json::value& key : parsed.as_array()) {
        if (!key.is_array()) throw JsError("invalid undelivered keys in queue message: " + text);
        KvBytes bytes;
        for (const json::value& byte : key.as_array()) bytes.push_back(static_cast<std::uint8_t>(jsonUnsigned(byte, 0xFF, "key byte")));
        keys.push_back(std::move(bytes));
      }
      return keys;
    }

    // Uuid::new_v4().to_string()
    std::string uuidV4() {
      std::array<unsigned char, 16> bytes{};
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw JsError("Failed to generate a random queue message id");
      bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
      bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
      static constexpr char digits[] = "0123456789abcdef";
      std::string out;
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) out += '-';
        out += digits[bytes[index] >> 4];
        out += digits[bytes[index] & 15];
      }
      return out;
    }

    struct Write {
      std::vector<kv_sdk::CheckMessage> checks;
      std::vector<kv_sdk::MutationMessage> mutations;
      std::vector<kv_sdk::EnqueueMessage> enqueues;
    };

    // SqliteBackend: one connection, used by the worker thread only (after the constructor,
    // which runs on the opening thread as napi.open does).
    class Backend {
    public:
      Backend(const std::string& path, const SqliteKvOptions& options) : db_(path, connectionOptions()), versionIncrement_(options.versionIncrement) {
#if defined(__APPLE__)
        // The native KV addon uses bundled SQLite, whose close drains/removes
        // WAL sidecars, independently of Bun's Darwin bun:sqlite default.
        db_.fileControl(sqlite::constants::SQLITE_FCNTL_PERSIST_WAL, 0);
#endif
        db_.run(BUSY_TIMEOUT);
        // SqliteBackend::new logs a failure to switch to WAL and carries on.
        try {
          db_.run("PRAGMA journal_mode = wal");
        } catch (const sqlite::SQLiteError&) {
        }
        db_.run("PRAGMA synchronous = FULL");
        // Keep a small per-connection pager and avoid mapping the state file.
        db_.run("PRAGMA cache_size = -2000");
        db_.run("PRAGMA mmap_size = 0");
        if (const auto page = db_.query("PRAGMA page_size").get())
          pageBytes_ = static_cast<std::size_t>(integerColumn(*page, "page_size", 0));
        try {
          migrate();
        } catch (const sqlite::SQLiteError& error) {
          // A database SQLite opened read-only is served without migrations (is_readonly).
          if (primaryCode(error) != SQLITE_READONLY) throw;
          readonly = true;
        }
      }

      // Writes fail with Error "Can not write to a read-only database." (WriteDisabled).
      bool readonly = false;

      std::vector<kv_sdk::RangeEntries> snapshotRead(const std::vector<kv_sdk::ReadRange>& ranges) {
        return runTx([&] {
          std::vector<kv_sdk::RangeEntries> responses;
          responses.reserve(ranges.size());
          for (const kv_sdk::ReadRange& range : ranges) {
            const sqlite::Statement statement = db_.query(range.reverse ? STATEMENT_KV_RANGE_SCAN_REVERSE : STATEMENT_KV_RANGE_SCAN);
            std::vector<sqlite::Binding> parameters;
            parameters.emplace_back(range.start);
            parameters.emplace_back(range.end);
            parameters.emplace_back(static_cast<std::int64_t>(range.limit));
            kv_sdk::RangeEntries entries;
            statement.forEachRow(parameters, [&](sqlite::Row row) {
              KvBytes key = takeBlobColumn(row, "k", 0);
              KvBytes value = takeBlobColumn(row, "v", 1);
              const std::int64_t encoding = integerColumn(row, "v_encoding", 2);
              checkEncoding(encoding, value);
              const std::int64_t version = integerColumn(row, "version", 3);
              entries.push_back(kv_sdk::NativeEntry{std::move(key), kv_sdk::EncodedValue{std::move(value), static_cast<int>(encoding)}, static_cast<std::uint64_t>(version)});
            });
            responses.push_back(std::move(entries));
          }
          // The snapshot's versionstamp, which napi discards.
          (void)db_.query(STATEMENT_GET_DATA_VERSION).get();
          return responses;
        });
      }

      // atomic_write_batched for a batch of one (batch_timeout None): the write runs inside
      // a savepoint of the batch transaction; a failed write rolls back to it, the
      // transaction still commits, and the write reports its error.
      std::optional<std::uint64_t> atomicWrite(const Write& write) {
        if (readonly) throw JsError("Can not write to a read-only database.");
        std::exception_ptr failure;
        const std::optional<std::int64_t> version = runTx([&] {
          failure = nullptr;
          db_.run("SAVEPOINT denokv_atomic_write");
          try {
            const std::optional<std::int64_t> committed = atomicWriteOnce(write);
            db_.run("RELEASE denokv_atomic_write");
            return committed;
          } catch (...) {
            failure = std::current_exception();
          }
          db_.run("ROLLBACK TO denokv_atomic_write");
          db_.run("RELEASE denokv_atomic_write");
          return std::optional<std::int64_t>();
        });
        if (failure) std::rethrow_exception(failure);
        if (!version) return std::nullopt;
        return static_cast<std::uint64_t>(*version);
      }

      // collect_expired: deletes `expiration_ms <= now` (advancing data_version by one when
      // it deleted anything) and returns the earliest expiry left.
      std::optional<double> collectExpired(double nowMs) {
        return runTx([&] {
          const std::vector<sqlite::Row> deleted = db_.query(STATEMENT_DELETE_ALL_EXPIRED).all(static_cast<std::int64_t>(nowMs));
          if (!deleted.empty()) incrementDataVersion(1);
          const std::optional<sqlite::Row> earliest = db_.query(STATEMENT_EARLIEST_EXPIRATION).get();
          return earliest ? std::optional<double>(static_cast<double>(integerColumn(*earliest, "expiration_ms", 0))) : std::nullopt;
        });
      }

      // queue_cleanup: requeues the messages a dead dequeuer left in queue_running.
      void queueCleanup(double nowMs) {
        for (;;) {
          const bool done = runTx([&] {
            const std::vector<sqlite::Row> entries = db_.query(STATEMENT_QUEUE_GET_RUNNING_PAST_DEADLINE).all(static_cast<std::int64_t>(nowMs));
            for (const sqlite::Row& entry : entries) requeueMessage(textColumn(entry, "id", 0), nowMs);
            return entries.empty();
          });
          if (done) break;
        }
      }

      std::uint64_t randomBelow(std::uint64_t bound) { return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(random_); }
      double randomUnit() { return std::uniform_real_distribution<double>(0, 1)(random_); }

    private:
      static sqlite::DatabaseOptions connectionOptions() {
        sqlite::DatabaseOptions options;
        options.create = true;
        options.safeIntegers = true;
        return options;
      }

      // run_tx with sqlite_retry_loop: a deferred transaction, committed when the body
      // returns, rolled back when it throws, and retried 5..19 ms later after SQLITE_BUSY.
      template <class Body> auto runTx(Body&& body) -> decltype(body()) {
        for (;;) {
          try {
            db_.run("BEGIN DEFERRED");
            auto result = body();
            db_.run("COMMIT");
            db_.reportPageCacheMemory(pageBytes_);
            return result;
          } catch (const sqlite::SQLiteError& error) {
            rollback();
            if (primaryCode(error) != SQLITE_BUSY) throw;
          } catch (...) {
            rollback();
            throw;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5 + randomBelow(15)));
        }
      }

      void rollback() {
        if (!db_.inTransaction()) return;
        try {
          db_.run("ROLLBACK");
        } catch (const sqlite::SQLiteError&) {
        }
      }

      void migrate() {
        runTx([&] {
          db_.run(STATEMENT_CREATE_MIGRATION_TABLE);
          const std::optional<sqlite::Row> row = db_.query("select version from migration_state where k = 0").get();
          const std::int64_t current = row ? integerColumn(*row, "version", 0) : 0;
          for (std::size_t index = 0; index < migrations().size(); ++index) {
            const auto version = static_cast<std::int64_t>(index + 1);
            if (version <= current) continue;
            for (const std::string_view statement : migrations()[index]) db_.run(statement);
            db_.query("replace into migration_state (k, version) values(?, ?)").run(std::int64_t{0}, version);
          }
          return 0;
        });
      }

      // decode_value: UnknownValueEncoding, and a KvU64 is exactly 8 bytes.
      static void checkEncoding(std::int64_t encoding, const KvBytes& value) {
        if (encoding != VE_V8 && encoding != VE_LE64 && encoding != VE_BYTES) throw JsError("The value encoding " + std::to_string(encoding) + " is unknown.");
        if (encoding == VE_LE64 && value.size() != 8) throw JsError("The KvU64 value has " + std::to_string(value.size()) + " bytes, not 8.");
      }

      std::uint64_t nextIncrement() { return versionIncrement_ ? versionIncrement_() : 1 + randomBelow(9); }

      std::int64_t incrementDataVersion(std::uint64_t increment) {
        const std::optional<sqlite::Row> row = db_.query(STATEMENT_INC_AND_GET_DATA_VERSION).get(static_cast<std::int64_t>(increment));
        if (!row) throw JsError("Query returned no rows");
        return integerColumn(*row, "version", 0);
      }

      void setRow(const KvBytes& key, const KvBytes& value, int encoding, std::int64_t version, std::int64_t expirationMs) {
        std::vector<sqlite::Binding> parameters;
        parameters.emplace_back(key);
        parameters.emplace_back(value);
        parameters.emplace_back(std::int64_t{encoding});
        parameters.emplace_back(version);
        parameters.emplace_back(expirationMs);
        const sqlite::Changes changed = db_.query(STATEMENT_KV_POINT_SET).run(parameters);
        if (changed.changes != 1) throw JsError("kv point set changed " + numberToString(changed.changes) + " rows");
      }

      // mutate_le64
      void mutateLe64(const kv_sdk::MutationMessage& mutation, std::string_view operation, std::int64_t version) {
        if (mutation.encoding != VE_LE64) throw JsError("Failed to perform '" + std::string(operation) + "' mutation on a non-U64 operand");
        const std::uint64_t operand = decodeLe64(mutation.data);
        const std::optional<sqlite::Row> old = db_.query(STATEMENT_KV_POINT_GET_VALUE_ONLY).get(mutation.key);
        std::uint64_t next = operand;
        if (old) {
          const KvBytes value = blobColumn(*old, "v", 0);
          const std::int64_t encoding = integerColumn(*old, "v_encoding", 1);
          checkEncoding(encoding, value);
          if (encoding != VE_LE64) throw JsError("Failed to perform '" + std::string(operation) + "' mutation on a non-U64 value in the database");
          const std::uint64_t current = decodeLe64(value);
          if (operation == "sum") next = current + operand;
          else if (operation == "min") next = std::min(current, operand);
          else next = std::max(current, operand);
        }
        setRow(mutation.key, encodeLe64(next), VE_LE64, version, -1);
      }

      std::optional<std::int64_t> atomicWriteOnce(const Write& write) {
        bool checksFailed = false;
        for (const kv_sdk::CheckMessage& check : write.checks) {
          const std::optional<sqlite::Row> row = db_.query(STATEMENT_KV_POINT_GET_VERSION_ONLY).get(check.key);
          const KvBytes real = row ? versionstampBytes(integerColumn(*row, "version", 0)) : KvBytes();
          if (real != check.versionstamp) checksFailed = true;
        }
        if (checksFailed) return std::nullopt;
        const std::int64_t version = incrementDataVersion(nextIncrement());
        for (const kv_sdk::MutationMessage& mutation : write.mutations) {
          switch (mutation.type) {
            case KvMutationType::set:
              setRow(mutation.key, mutation.data, mutation.encoding, version, mutation.expireAtMs > 0 ? static_cast<std::int64_t>(mutation.expireAtMs) : -1);
              break;
            case KvMutationType::delete_:
              db_.query(STATEMENT_KV_POINT_DELETE).run(mutation.key);
              break;
            case KvMutationType::sum:
              // sum_v8 handles a V8-encoded operand, and Bun's bytes are not V8's.
              if (mutation.encoding != VE_LE64) throw JsError("Some of the parameters are not valid V8 values");
              mutateLe64(mutation, "sum", version);
              break;
            case KvMutationType::min:
              mutateLe64(mutation, "min", version);
              break;
            case KvMutationType::max:
              mutateLe64(mutation, "max", version);
              break;
          }
        }
        for (const kv_sdk::EnqueueMessage& enqueue : write.enqueues) {
          const std::string backoff = enqueue.backoffSchedule.empty() ? jsonArray(std::span<const std::uint32_t>(DEFAULT_BACKOFF_SCHEDULE))
                                                                      : jsonArray(std::span<const std::uint32_t>(enqueue.backoffSchedule));
          std::vector<sqlite::Binding> parameters;
          parameters.emplace_back(static_cast<std::int64_t>(enqueue.deadlineMs));
          parameters.emplace_back(uuidV4());
          parameters.emplace_back(enqueue.payload);
          parameters.emplace_back(backoff);
          parameters.emplace_back(jsonArray(enqueue.keysIfUndelivered));
          const sqlite::Changes changed = db_.query(STATEMENT_QUEUE_ADD_READY).run(parameters);
          if (changed.changes != 1) throw JsError("queue insert changed " + numberToString(changed.changes) + " rows");
        }
        return version;
      }

      // requeue_message
      void requeueMessage(const std::string& id, double nowMs) {
        const std::optional<sqlite::Row> message = db_.query(STATEMENT_QUEUE_GET_RUNNING_BY_ID).get(id);
        if (!message) return;
        const KvBytes data = blobColumn(*message, "data", 2);
        const std::string backoffText = textColumn(*message, "backoff_schedule", 3);
        const std::string keysText = textColumn(*message, "keys_if_undelivered", 4);
        const std::vector<std::uint64_t> schedule = parseBackoffSchedule(backoffText);
        if (!schedule.empty()) {
          std::vector<sqlite::Binding> parameters;
          parameters.emplace_back(static_cast<std::int64_t>(nowMs) + static_cast<std::int64_t>(schedule.front()));
          parameters.emplace_back(id);
          parameters.emplace_back(data);
          parameters.emplace_back(jsonArray(std::span<const std::uint64_t>(schedule).subspan(1)));
          parameters.emplace_back(keysText);
          db_.query(STATEMENT_QUEUE_ADD_READY).run(parameters);
        } else if (!keysText.empty()) {
          const std::vector<KvBytes> keys = parseKeys(keysText);
          const std::int64_t version = incrementDataVersion(nextIncrement());
          for (const KvBytes& key : keys) setRow(key, data, VE_V8, version, -1);
        }
        db_.query(STATEMENT_QUEUE_REMOVE_RUNNING).run(id);
      }

      std::size_t pageBytes_ = 0;
      sqlite::Database db_;
      std::function<std::uint64_t()> versionIncrement_;
      std::mt19937_64 random_{std::random_device{}()};
    };

    // ---- The worker thread (lib.rs sqlite_thread) -------------------------------------------------

    template <class Signature> class MoveOnlyFunction;
    template <class Result, class... Arguments> class MoveOnlyFunction<Result(Arguments...)> {
    public:
      MoveOnlyFunction() = default;
      template <class F>
        requires(!std::is_same_v<std::decay_t<F>, MoveOnlyFunction>)
      MoveOnlyFunction(F function) : impl_(std::make_unique<Impl<F>>(std::move(function))) {}
      Result operator()(Arguments... arguments) { return impl_->call(std::forward<Arguments>(arguments)...); }

    private:
      struct Base {
        virtual ~Base() = default;
        virtual Result call(Arguments... arguments) = 0;
      };
      template <class F> struct Impl final : Base {
        F function;
        explicit Impl(F value) : function(std::move(value)) {}
        Result call(Arguments... arguments) override { return function(std::forward<Arguments>(arguments)...); }
      };
      std::unique_ptr<Base> impl_;
    };

    struct Job {
      // Date.now() when the operation started, the backend's clock for its sweep.
      double nowMs = 0;
      // Runs on the worker thread; `backend` is null once the database is closed (or
      // its worker failed), and the job then reports Error "Database is closed.".
      MoveOnlyFunction<void(Backend* backend)> run;
    };

    class Worker {
    public:
      Worker(std::unique_ptr<Backend> backend, const SqliteKvOptions& options, double openedAtMs)
          : backend_(std::move(backend)), sweepIntervalMs_(options.sweepIntervalMs), sweepJitterMs_(options.sweepJitterMs), idleSweeps_(!options.now),
            openedAtMs_(openedAtMs) {
        thread_ = std::thread([this] { loop(); });
      }

      ~Worker() { shutdown(); }

      Worker(const Worker&) = delete;
      Worker& operator=(const Worker&) = delete;

      void submit(Job job) {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
      }

      // Sqlite::close: the running job finishes, queued ones fail, the connection closes.
      void shutdown() {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          stopping_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
      }

    private:
      void loop() {
        // sqlite_thread's start: queue_cleanup, then collect_expired.
        maintain(openedAtMs_, true);
        for (;;) {
          Job job;
          {
            std::unique_lock<std::mutex> lock(mutex_);
            while (jobs_.empty() && !stopping_) {
              if (!idleSweeps_ || !backend_ || backend_->readonly) {
                wake_.wait(lock);
                continue;
              }
              const double now = systemNowMs();
              const double due = std::min(expiryDeadlineMs_, cleanupDeadlineMs_);
              if (due <= now) {
                lock.unlock();
                maintain(now, false);
                lock.lock();
                continue;
              }
              wake_.wait_for(lock, std::chrono::milliseconds(static_cast<std::int64_t>(std::ceil(due - now))));
            }
            if (stopping_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
          }
          maintain(job.nowMs, false);
          job.run(backend_.get());
        }
        std::deque<Job> rest;
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          rest.swap(jobs_);
        }
        for (Job& job : rest) job.run(nullptr);
        backend_.reset();
      }

      // The timer branch of sqlite_thread: queue cleanup and the expiry sweep when due. A
      // failure is fatal to the handle, as the native thread's panic is.
      void maintain(double nowMs, bool startup) {
        if (!backend_ || backend_->readonly) return;
        try {
          if (startup || nowMs >= cleanupDeadlineMs_) {
            backend_->queueCleanup(nowMs);
            cleanupDeadlineMs_ = nowMs + QUEUE_RUNNING_CLEANUP_INTERVAL_MS * (0.9 + backend_->randomUnit() * 0.2);
          }
          if (startup || nowMs >= expiryDeadlineMs_) {
            const std::optional<double> earliest = backend_->collectExpired(nowMs);
            const double fallback = nowMs + sweepIntervalMs_;
            expiryDeadlineMs_ = std::min(earliest.value_or(fallback), fallback) + backend_->randomUnit() * sweepJitterMs_;
          }
        } catch (const sqlite::SQLiteError& error) {
          if (primaryCode(error) == SQLITE_READONLY) backend_->readonly = true;
          else backend_.reset();
        } catch (...) {
          backend_.reset();
        }
      }

      std::unique_ptr<Backend> backend_;
      const double sweepIntervalMs_;
      const double sweepJitterMs_;
      const bool idleSweeps_;
      const double openedAtMs_;
      double expiryDeadlineMs_ = 0;
      double cleanupDeadlineMs_ = 0;
      std::mutex mutex_;
      std::condition_variable wake_;
      std::deque<Job> jobs_;
      bool stopping_ = false;
      std::thread thread_;
    };

    // The io_context behind the executor a coroutine runs on (rt::Loop's, or a test's).
    boost::asio::io_context& ioContextOf(const boost::asio::any_io_executor& executor) {
      return static_cast<boost::asio::io_context&>(boost::asio::query(executor, boost::asio::execution::context));
    }

    template <class Result> struct Outcome {
      std::optional<Result> value;
      std::exception_ptr error;
    };

    // Runs `body` on the worker and resumes the caller from a handler posted to its
    // io_context, which counts as outstanding work until then (the loop neither ends nor
    // lets a fake clock jump while SQLite works).
    template <class Result> Task<Result> runJob(std::shared_ptr<Worker> worker, double nowMs, std::function<Result(Backend&)> body) {
      auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::io_context& io = ioContextOf(executor);
      auto outcome = std::make_shared<Outcome<Result>>();
      auto initiation = [worker, nowMs, outcome, body = std::move(body), &io](auto handler) mutable {
        auto work = boost::asio::make_work_guard(io);
        Job job;
        job.nowMs = nowMs;
        job.run = [outcome, body = std::move(body), handler = std::move(handler), work = std::move(work), &io](Backend* backend) mutable {
          if (!backend) {
            outcome->error = std::make_exception_ptr(JsError("Database is closed."));
          } else {
            try {
              outcome->value.emplace(body(*backend));
            } catch (...) {
              outcome->error = napiError(std::current_exception());
            }
          }
          boost::asio::post(io, [handler = std::move(handler), work = std::move(work)]() mutable {
            work.reset();
            std::move(handler)();
          });
        };
        worker->submit(std::move(job));
      };
      co_await boost::asio::async_initiate<const boost::asio::use_awaitable_t<>&, void()>(std::move(initiation), boost::asio::use_awaitable);
      if (outcome->error) std::rethrow_exception(outcome->error);
      Result result = std::move(*outcome->value);
      co_return result;
    }

    // A rejection the napi function reports for a request it refuses before reaching the
    // database: one macrotask later, like any settled native promise.
    Task<void> nextMacrotask() {
      auto executor = co_await boost::asio::this_coro::executor;
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
    }

    // The NapiInterface functions of one open database (napi/src/lib.rs).
    class SqliteNative final : public kv_sdk::Native {
    public:
      SqliteNative(std::shared_ptr<Worker> worker, std::function<double()> now) : worker_(std::move(worker)), now_(std::move(now)) {}

      // The database id was removed (napi.close).
      bool closed = false;

      void close() {
        closed = true;
        worker_->shutdown();
      }

      Task<std::vector<kv_sdk::RangeEntries>> snapshotRead(std::vector<kv_sdk::ReadRange> ranges) override {
        std::exception_ptr refused;
        try {
          kv_sdk::validateSnapshotRead(ranges);
          if (closed) throw JsError("db not found");
        } catch (...) {
          refused = std::current_exception();
        }
        if (refused) {
          co_await nextMacrotask();
          std::rethrow_exception(refused);
        }
        std::function<std::vector<kv_sdk::RangeEntries>(Backend&)> body = [ranges = std::move(ranges)](Backend& backend) { return backend.snapshotRead(ranges); };
        auto reading = runJob(worker_, now_(), std::move(body));
        std::vector<kv_sdk::RangeEntries> entries = co_await std::move(reading);
        co_return entries;
      }

      Task<std::optional<std::uint64_t>> atomicWrite(std::vector<kv_sdk::CheckMessage> checks, std::vector<kv_sdk::MutationMessage> mutations,
                                                     std::vector<kv_sdk::EnqueueMessage> enqueues) override {
        const double now = now_();
        std::exception_ptr refused;
        try {
          kv_sdk::validateAtomicWrite(checks, mutations, enqueues, now, {});
          if (closed) throw JsError("db not found");
        } catch (...) {
          refused = std::current_exception();
        }
        if (refused) {
          co_await nextMacrotask();
          std::rethrow_exception(refused);
        }
        auto write = std::make_shared<Write>();
        write->checks = std::move(checks);
        write->mutations = std::move(mutations);
        write->enqueues = std::move(enqueues);
        std::function<std::optional<std::uint64_t>(Backend&)> body = [write](Backend& backend) { return backend.atomicWrite(*write); };
        auto writing = runJob(worker_, now, std::move(body));
        std::optional<std::uint64_t> version = co_await std::move(writing);
        co_return version;
      }

    private:
      std::shared_ptr<Worker> worker_;
      std::function<double()> now_;
    };

    // ---- BaseKv over the native database ------------------------------------------------------------

    Task<KvCommitResult> commitOpen(kv_sdk::StorePtr store, std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations) {
      store->checkOpen("commit");
      std::vector<kv_sdk::Enqueue> enqueues;
      const KvCommitResult result = co_await kv_sdk::commit(store, std::move(checks), std::move(mutations), std::move(enqueues));
      co_return result;
    }

    std::unique_ptr<AtomicOperation> sdkAtomic(const kv_sdk::StorePtr& store) {
      kv_sdk::StorePtr owner = store;
      return std::make_unique<GenericAtomicOperation>(
        [owner](std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations) { return commitOpen(owner, std::move(checks), std::move(mutations)); });
    }

    Task<KvCommitResult> sdkSet(kv_sdk::StorePtr store, KvKey key, KvValue value, KvSetOptions options) {
      store->checkOpen("set");
      std::unique_ptr<AtomicOperation> operation = sdkAtomic(store);
      operation->set(std::move(key), std::move(value), options);
      const KvCommitResult result = co_await operation->commit();
      if (!result.ok) throw JsError("set failed");
      co_return result;
    }

    Task<void> sdkDelete(kv_sdk::StorePtr store, KvKey key) {
      store->checkOpen("delete");
      std::unique_ptr<AtomicOperation> operation = sdkAtomic(store);
      operation->delete_(std::move(key));
      const KvCommitResult result = co_await operation->commit();
      if (!result.ok) throw JsError("delete failed");
    }

    Task<KvCommitResult> sdkEnqueue(kv_sdk::StorePtr store, KvValue value, KvEnqueueOptions options) {
      store->checkOpen("enqueue");
      store->checkOpen("commit");
      std::vector<kv_sdk::Enqueue> enqueues;
      enqueues.push_back(kv_sdk::Enqueue{std::move(value), std::move(options)});
      std::vector<AtomicCheck> checks;
      std::vector<KvMutation> mutations;
      const KvCommitResult result = co_await kv_sdk::commit(store, std::move(checks), std::move(mutations), std::move(enqueues));
      if (!result.ok) throw JsError("enqueue failed");
      co_return result;
    }
  }

  struct SqliteKv::State {
    std::string path;
    kv_sdk::StorePtr store;
    std::shared_ptr<SqliteNative> native;
  };

  SqliteKv::SqliteKv(std::string path, SqliteKvOptions options) : state_(std::make_shared<State>()) {
    state_->path = std::move(path);
    std::function<double()> now = options.now ? options.now : std::function<double()>([] { return dateNow(); });
    auto backend = std::make_unique<Backend>(state_->path, options);
    auto worker = std::make_shared<Worker>(std::move(backend), options, now());
    state_->native = std::make_shared<SqliteNative>(std::move(worker), now);
    auto store = std::make_shared<kv_sdk::Store>();
    store->native = state_->native;
    if (options.encodeV8) {
      store->encodeV8 = std::move(options.encodeV8);
    } else {
      store->encodeV8 = [](const KvValue& value) {
        assertPortableKvValue(value);
        return kvJscSerialize(value);
      };
    }
    store->decodeV8 = options.decodeV8 ? std::move(options.decodeV8) : std::function<KvValue(std::span<const std::uint8_t>)>(kvJscDeserialize);
    store->now = std::move(now);
    state_->store = std::move(store);
  }

  SqliteKv::~SqliteKv() {
    if (state_->native->closed) return;
    state_->store->closed = true;
    state_->native->close();
  }

  Task<KvEntryMaybe<>> SqliteKv::get(KvKey key, KvGetOptions) { return kv_sdk::get(state_->store, std::move(key)); }

  Task<std::vector<KvEntryMaybe<>>> SqliteKv::getMany(std::vector<KvKey> keys, KvGetOptions) { return kv_sdk::getMany(state_->store, std::move(keys)); }

  Task<KvCommitResult> SqliteKv::set(KvKey key, KvValue value, KvSetOptions options) { return sdkSet(state_->store, std::move(key), std::move(value), options); }

  Task<void> SqliteKv::delete_(KvKey key) { return sdkDelete(state_->store, std::move(key)); }

  std::unique_ptr<KvListIterator> SqliteKv::list(KvListSelector selector, KvListOptions options) {
    return std::make_unique<kv_sdk::GenericListIterator>(kv_sdk::list(state_->store, selector, options));
  }

  std::unique_ptr<AtomicOperation> SqliteKv::atomic() { return sdkAtomic(state_->store); }

  Task<KvCommitResult> SqliteKv::enqueueValue(KvValue message, KvEnqueueOptions options) { return sdkEnqueue(state_->store, std::move(message), std::move(options)); }

  Task<void> SqliteKv::enqueue(KvValue message, KvEnqueueOptions options) {
    auto enqueuing = sdkEnqueue(state_->store, std::move(message), std::move(options));
    co_await std::move(enqueuing);
  }

  void SqliteKv::close() {
    state_->store->checkOpen("close");
    state_->store->closed = true;
    state_->native->close();
  }

  const std::string& SqliteKv::path() const noexcept { return state_->path; }
}
