#pragma once
// Port of tests/helpers/counting-kv.ts.
//
// A test-only in-memory Kv substitute. It records every public KV command and
// each atomic commit while keeping the same CAS boundary used by the V3 ledger
// tests. Seeds are deliberately uncounted so a scenario budget starts at
// ingress rather than fixture construction.
//
// Timing: like the TS (whose methods return `Promise.resolve(...)`), every call
// does its work when it starts and its result arrives one microtask later; a
// list iterator advances one entry per microtask. Hold instances in a
// std::shared_ptr (`std::make_shared<CountingKv>()`): the module code under test
// takes `std::shared_ptr<rt::Kv>`.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::tests::helpers::counting_kv {
  // `StoredEntry`
  struct StoredEntry {
    rt::KvKey key;
    rt::KvValue value;
    std::string versionstamp;
  };

  // "mandatory_correctness" | "optional_telemetry" | "background"
  using KvMeasurementClassification = std::string;

  struct KvMeasurementContext {
    std::string authKind;
    std::string outcome;
  };

  // "get" | "getMany" | "set" | "delete" | "list" | "atomic.commit"
  using KvCommandName = std::string;

  struct KvCommandRecord {
    std::optional<std::string> scenario;
    KvCommandName command;
    KvMeasurementClassification classification;
    std::vector<rt::KvKey> keys;
    double keyCount;
    double atomicChecks;
    double atomicMutations;
    // "committed" | "conflict" | "failed" | null
    std::optional<std::string> atomicResult;
  };

  struct KvOperationBudget {
    std::string scenario;
    std::string auth_kind;
    std::string outcome;
    double commands;
    double read_commands;
    double write_mutations;
    double atomic_commits;
    double atomic_checks;
    double atomic_mutations;
    double mandatory_correctness_commands;
    double optional_telemetry_commands;
    double background_commands;
    double serialized_request_bytes;
    double upstream_response_bytes;
    std::optional<double> latency_ms;
    friend bool operator==(const KvOperationBudget&, const KvOperationBudget&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const KvOperationBudget& value);
  KvOperationBudget tag_invoke(const json::value_to_tag<KvOperationBudget>&, const json::value& value);

  // The helper's private `encodeKey`, which the TS tests repeat as `JSON.stringify(key)`:
  // the map key of `entries`. A bigint part throws TypeError "JSON.stringify cannot
  // serialize BigInt." as JSON.stringify does.
  std::string encodeKey(const rt::KvKey& key);

  /**
   * Categories are intentionally conservative. A key that is not known to be
   * optional stays outside the optional-telemetry bucket, so test reports never
   * present a correctness write as safely removable telemetry.
   */
  KvMeasurementClassification classifyKvKey(const rt::KvKey& key);

  class CountingAtomicOperation;
  class CountingListIterator;

  // Not final: tests extend it (`class ListSelectorCountingKv extends CountingKv`).
  class CountingKv : public rt::Kv {
  public:
    std::vector<KvCommandRecord> commands;
    rt::OrderedMap<std::string, StoredEntry> entries;

    void clearData();
    void clearMeasurements();
    void seed(const rt::KvKey& key, const rt::KvValue& value);
    // Returns the function that ends the measurement.
    std::function<void()> beginMeasurement(const KvMeasurementContext& context);
    void setLatency(double milliseconds);
    void recordSerializedRequestBytes(double bytes);
    void recordUpstreamResponseBytes(double bytes);
    std::vector<KvOperationBudget> budgets() const;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    // Selects `{ prefix }` entries (a selector without a prefix selects every entry; `start`,
    // `end` and every option are ignored, as in the TS) in `encodeKey` localeCompare order.
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    // The TS operation has check (one entry per call: further entries are ignored), set,
    // delete, sum and commit; min, max and mutate are not functions (TypeError).
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override;

  private:
    friend class CountingAtomicOperation;

    struct RecordDetails {
      std::optional<double> writeMutations{};
      std::optional<double> atomicChecks{};
      std::optional<double> atomicMutations{};
      // "committed" | "conflict" | "failed"
      std::optional<std::string> atomicResult{};
    };

    KvMeasurementClassification classifyCommands(const std::vector<rt::KvKey>& keys) const;
    std::string nextVersionstamp();
    const KvOperationBudget* activeBudget() const;
    // "serialized_request_bytes" | "upstream_response_bytes"
    void addBytes(const std::string& field, double bytes);
    void record(const KvCommandName& command, const KvMeasurementClassification& classification, const std::vector<rt::KvKey>& keys, const RecordDetails& details);
    void record(const KvCommandName& command, const KvMeasurementClassification& classification, const std::vector<rt::KvKey>& keys);

    double nextVersion_ = 0;
    std::optional<std::string> activeScenario_;
    rt::OrderedMap<std::string, KvOperationBudget> budgets_;
  };
}
