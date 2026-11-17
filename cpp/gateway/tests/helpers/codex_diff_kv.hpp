#pragma once
// Shared pieces of the b5 codex differential tests (C++ only; no TS helper): the deterministic KV the
// bun generators use (scratch diffkv.ts: versionstamps are one global commit counter padded to 20
// digits, every value is stored as written), UUID masking, and the JSON a TS RoutingAccount prints.
#include "uos/codex/routing_state.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <string>
#include <vector>

namespace uos::tests::helpers::codex_diff_kv {
  // `JSON.stringify(key)` for the string keys these tests use.
  std::string keyOf(const rt::KvKey& key);

  class DiffKv : public rt::Kv {
  public:
    struct Row {
      rt::KvKey key;
      rt::KvValue value;
      std::string versionstamp;
    };
    rt::OrderedMap<std::string, Row> rows;
    double version = 0;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override {}

    std::string stamp();
    rt::KvEntryMaybe<> entryOf(const rt::KvKey& key) const;
    // `kv.row(key)`: the stored value as JSON, null when absent.
    json::value row(const rt::KvKey& key) const;
  };

  // JSON of a value with every random v4 UUID string replaced by "<uuid>" (the generators' `plain`).
  json::value plain(json::value value);

  // `JSON.parse(JSON.stringify(account))` of a TS RoutingAccount: required nullable members print null,
  // optional members are omitted when undefined.
  json::value routingAccountJson(const codex::routing_state::RoutingAccount& account);
  json::value routingAccountJson(const std::optional<codex::routing_state::RoutingAccount>& account);

  // Compares `actual[name]` with `expected[name]` (rt::deepEqual), printing both on a mismatch.
  bool sameVector(const json::object& expected, const json::object& actual, std::string_view name);
}
