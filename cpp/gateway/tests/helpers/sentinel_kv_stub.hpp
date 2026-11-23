#pragma once
// Port of tests/helpers/sentinel-kv-stub.ts.
/**
 * In-memory `Kv` substitute for the sentinel replay coverage tests.
 *
 * This specialized fake keeps the
 * versionstamp/CAS boundary the production code depends on, hands out
 * versionstamps in strictly increasing order, returns the same
 * `{ key, value: null, versionstamp: null }` shape for absent keys, and orders
 * `list` results the way Deno KV does (byte arrays, then strings, then
 * numbers) with a resumable cursor.
 *
 * Deliberately unsupported: TTLs (`expireIn` is accepted and ignored, because
 * neither the real KV nor this store exposes remaining TTL to a reader),
 * `watch`, `enqueue`/`listenQueue` and the `sum`/`min`/`max` mutations are not
 * used by any sentinel module and throw when called, so a future caller
 * cannot mistake silence for support.
 */
// Every call does its work when it is made and settles one microtask later (`Promise.resolve(result)`), as
// the TS methods do. Values are stored and read back through rt::kvStructuredClone (`structuredClone`).
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uos::tests::helpers::sentinel_kv_stub {
  // `type StoredEntry = Readonly<{ key: KvKey; value: unknown; versionstamp: string }>`
  struct StoredEntry {
    rt::KvKey key;
    rt::KvValue value;
    std::string versionstamp;
  };

  /** Internal storage shared with the atomic-operation implementation. */
  // `entries` is keyed by the key's JSON text (`keyText`).
  struct SentinelKvStorage {
    rt::OrderedMap<std::string, StoredEntry> entries;
    std::function<std::string()> nextVersionstamp;
  };

  class SentinelKvStub final : public rt::Kv {
  public:
    SentinelKvStub();
    SentinelKvStorage storage;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override;

  private:
    double sequence_ = 0;
    std::string nextVersionstamp();
  };

  /** Native Bun storage is required by the migrated acceptance suite. */
  bool hasRuntimeKv();

  /** Open real disposable storage; tests needing injected faults construct SentinelKvStub directly. */
  // `openKv(":memory:")`: an rt::MemoryKv on the test clock (rt::dateNow()).
  rt::Task<std::shared_ptr<rt::Kv>> openSentinelTestKv();
}
