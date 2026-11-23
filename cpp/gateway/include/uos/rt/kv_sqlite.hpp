#pragma once
// rt::SqliteKv: the durable `#kv` store. The @deno/kv 0.14 SDK (the layer MemoryKv
// shares) over a C++ reimplementation of the native addon's SQLite backend (denokv
// 9fe65cc with the repository's atomic-write rollback patch), reading and writing the
// production file format byte for byte: the same schema text and migrations, tuple keys,
// value encodings (1 Bun/JavaScriptCore bytes, 2 KvU64 little-endian, 3 raw bytes),
// versions, expiry sweeps and queue rows. docs/rt/kv-durable.md is the reference.
//
// It is the SDK's store, not the guarded one: src/storage/kv.ts wraps it with the
// native-kv.ts guard (storage::native_kv::guardNativeKv) as it wraps the native addon,
// which adds the busy retries, the arithmetic TypeError and the prefix-list fix.
#include "uos/rt/kv.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace uos::rt {
  struct SqliteKvOptions {
    // ProtoBasedKv's encodeV8 / decodeV8. Empty: Bun's node:v8 codec behind
    // assertPortableKvValue (kvJscSerialize) and kvJscDeserialize, what src/storage/kv.ts passes.
    std::function<KvBytes(const KvValue& value)> encodeV8{};
    std::function<KvValue(std::span<const std::uint8_t> bytes)> decodeV8{};
    // Date.now() in epoch milliseconds: expireIn and enqueue deadlines, and the backend's
    // clock for sweeps (taken when an operation starts). Empty: rt::dateNow(), and the
    // backend also sweeps while idle on the system clock, as the native store does.
    std::function<double()> now{};
    // The commit version step. Empty: uniform 1..9, the native store's random step.
    std::function<std::uint64_t()> versionIncrement{};
    // Expired rows stay readable until a sweep deletes `expiration_ms <= now`: at open,
    // then at the earliest expiry the last sweep saw or `sweepIntervalMs` after it,
    // whichever is first, plus a random delay below `sweepJitterMs` (denokv: 60 s, 1 s).
    // 0 and 0 sweep before every operation.
    double sweepIntervalMs = 60'000;
    double sweepJitterMs = 1'000;
  };

  // One open handle (napi.open + the SDK's NapiBasedKv): its own SQLite connection, used by
  // one worker thread (the addon's `sw-0`), so SQLite never blocks the event loop; each
  // operation completes as a posted handler once its transaction is done. Two handles (or
  // processes) on one file contend through SQLite locking: a writer waits up to 5 s for
  // the lock, then fails with Error "database is locked", which the guard retries.
  class SqliteKv final : public Kv, public KvEnqueue {
  public:
    // `napi.open(path)`: opens the SQLite database at `path` (created when missing; a
    // plain path, ":memory:" for a private in-memory database), switches it to WAL with
    // synchronous FULL and a 5 s busy timeout and applies the native store's migrations,
    // on the calling thread as the addon does. The worker then requeues abandoned queue
    // messages and sweeps expired rows before it runs the first operation. Throws
    // rt::sqlite::SQLiteError when SQLite cannot open or migrate the file.
    explicit SqliteKv(std::string path, SqliteKvOptions options = {});
    // Closes the handle when close() was not called (joining the worker thread).
    ~SqliteKv() override;

    using Kv::get;
    using Kv::getMany;
    Task<KvEntryMaybe<>> get(KvKey key, KvGetOptions options = {}) override;
    Task<std::vector<KvEntryMaybe<>>> getMany(std::vector<KvKey> keys, KvGetOptions options = {}) override;
    // BaseKv.set: one atomic write; Error "set failed" if it reports a failed check.
    Task<KvCommitResult> set(KvKey key, KvValue value, KvSetOptions options = {}) override;
    // BaseKv.delete: Error "delete failed" likewise.
    Task<void> delete_(KvKey key) override;
    // BaseKv.list: the SDK's GenericKvListIterator (its cursor stays at the last entry
    // once the scan ends; the guard's iterator clears it).
    std::unique_ptr<KvListIterator> list(KvListSelector selector, KvListOptions options = {}) override;
    // BaseKv.atomic: commit errors are the native store's (Error "Failed to perform 'sum'
    // mutation on a non-U64 value in the database", "database is locked", ...).
    std::unique_ptr<AtomicOperation> atomic() override;
    // BaseKv.enqueue: a queue row due at Date.now() + delay with the SDK's backoff
    // schedule [100, 200, 400, 800]; Error "enqueue failed" for a failed check.
    Task<void> enqueue(KvValue message, KvEnqueueOptions options) override;
    // BaseKv.enqueue's result.
    Task<KvCommitResult> enqueueValue(KvValue message, KvEnqueueOptions options);
    // BaseKv.close: Error "Cannot call '.close' after '.close' is called" the second
    // time. Waits for the operation the worker is running; operations still queued fail
    // with Error "Database is closed.", later native calls with Error "db not found".
    void close() override;

    // The path given to the constructor.
    const std::string& path() const noexcept;

    struct State;

  private:
    std::shared_ptr<State> state_;
  };
}
