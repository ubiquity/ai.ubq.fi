#include "tests/helpers/counting_kv.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <utility>

namespace uos::tests::helpers::counting_kv {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // `AtomicMutation`: { kind: "set"; key; value } | { kind: "delete"; key } | { kind: "sum"; key; value: bigint }
    struct AtomicMutation {
      std::string kind;
      rt::KvKey key;
      rt::KvValue value{};
      rt::BigInt amount{};
    };

    // `clone(value)`: a KvU64 is copied as itself, everything else through structuredClone
    // (the value itself when structuredClone throws).
    rt::KvValue clone(const rt::KvValue& value) {
      if (value.isU64()) return value;
      try {
        return rt::kvStructuredClone(value);
      } catch (const rt::JsError&) {
        return value;
      }
    }

    // `Object.is(part, key[index])`: a Uint8Array part is an object, so a stored (cloned)
    // key never shares it with the prefix.
    bool startsWithKey(const rt::KvKey& key, const rt::KvKey& prefix) {
      for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (index >= key.size() || prefix[index].isBytes() || key[index].isBytes()) return false;
        if (!(prefix[index] == key[index])) return false;
      }
      return true;
    }

    // `keyPartText(part)`: String(part); a Uint8Array as its bytes joined by ",".
    std::string keyPartText(const rt::KvKeyPart& part) { return part.toString(); }

    KvMeasurementClassification atomicClassification(const std::vector<rt::AtomicCheck>& checks, const std::vector<AtomicMutation>& mutations) {
      std::vector<KvMeasurementClassification> classifications;
      for (const rt::AtomicCheck& entry : checks) classifications.push_back(classifyKvKey(entry.key));
      for (const AtomicMutation& mutation : mutations) classifications.push_back(classifyKvKey(mutation.key));
      if (rt::includes(classifications, std::string("mandatory_correctness"))) return "mandatory_correctness";
      if (rt::includes(classifications, std::string("optional_telemetry"))) return "optional_telemetry";
      return "background";
    }

    KvOperationBudget emptyBudget(const KvMeasurementContext& context) {
      return KvOperationBudget{
        .scenario = context.authKind + ":" + context.outcome,
        .auth_kind = context.authKind,
        .outcome = context.outcome,
        .commands = 0,
        .read_commands = 0,
        .write_mutations = 0,
        .atomic_commits = 0,
        .atomic_checks = 0,
        .atomic_mutations = 0,
        .mandatory_correctness_commands = 0,
        .optional_telemetry_commands = 0,
        .background_commands = 0,
        .serialized_request_bytes = 0,
        .upstream_response_bytes = 0,
        .latency_ms = std::nullopt,
      };
    }

    std::vector<rt::KvKey> keysOf(const std::vector<rt::AtomicCheck>& checks, const std::vector<AtomicMutation>& mutations) {
      std::vector<rt::KvKey> keys;
      for (const rt::AtomicCheck& check : checks) keys.push_back(check.key);
      for (const AtomicMutation& mutation : mutations) keys.push_back(mutation.key);
      return keys;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvOperationBudget& value) {
    out = Writer()
            .required("scenario", value.scenario)
            .required("auth_kind", value.auth_kind)
            .required("outcome", value.outcome)
            .required("commands", value.commands)
            .required("read_commands", value.read_commands)
            .required("write_mutations", value.write_mutations)
            .required("atomic_commits", value.atomic_commits)
            .required("atomic_checks", value.atomic_checks)
            .required("atomic_mutations", value.atomic_mutations)
            .required("mandatory_correctness_commands", value.mandatory_correctness_commands)
            .required("optional_telemetry_commands", value.optional_telemetry_commands)
            .required("background_commands", value.background_commands)
            .required("serialized_request_bytes", value.serialized_request_bytes)
            .required("upstream_response_bytes", value.upstream_response_bytes)
            .orNull("latency_ms", value.latency_ms)
            .done();
  }

  KvOperationBudget tag_invoke(const json::value_to_tag<KvOperationBudget>&, const json::value& value) {
    const Reader in(value, "KvOperationBudget");
    return KvOperationBudget{
      .scenario = in.required<std::string>("scenario"),
      .auth_kind = in.required<std::string>("auth_kind"),
      .outcome = in.required<std::string>("outcome"),
      .commands = in.required<double>("commands"),
      .read_commands = in.required<double>("read_commands"),
      .write_mutations = in.required<double>("write_mutations"),
      .atomic_commits = in.required<double>("atomic_commits"),
      .atomic_checks = in.required<double>("atomic_checks"),
      .atomic_mutations = in.required<double>("atomic_mutations"),
      .mandatory_correctness_commands = in.required<double>("mandatory_correctness_commands"),
      .optional_telemetry_commands = in.required<double>("optional_telemetry_commands"),
      .background_commands = in.required<double>("background_commands"),
      .serialized_request_bytes = in.required<double>("serialized_request_bytes"),
      .upstream_response_bytes = in.required<double>("upstream_response_bytes"),
      .latency_ms = in.orNull<double>("latency_ms"),
    };
  }

  std::string encodeKey(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.push_back(json::value(json::string(part.string())));
      else if (part.isNumber()) parts.push_back(rt::makeNumber(part.number()));
      else if (part.isBool()) parts.push_back(json::value(part.boolean()));
      else if (part.isBigInt()) throw rt::TypeError("JSON.stringify cannot serialize BigInt.");
      else {
        // A Uint8Array serializes as an object of its indexed bytes.
        json::object bytes;
        const rt::KvBytes& data = part.bytes();
        for (std::size_t index = 0; index < data.size(); ++index) bytes[std::to_string(index)] = rt::makeNumber(static_cast<std::int64_t>(data[index]));
        parts.push_back(json::value(std::move(bytes)));
      }
    }
    return rt::jsonStringify(json::value(std::move(parts)));
  }

  KvMeasurementClassification classifyKvKey(const rt::KvKey& key) {
    std::string joined;
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index) joined += "/";
      joined += keyPartText(key[index]);
    }
    joined = rt::toLowerCase(joined);
    if (rt::includes(joined, "prompt_cache") || rt::includes(joined, "telemetry") || rt::includes(joined, "provider_health") || rt::includes(joined, "provider_capacity")) {
      return "optional_telemetry";
    }
    if (rt::includes(joined, "api_key_usage") || rt::includes(joined, "api_keys") || rt::includes(joined, "runtime_config") || rt::includes(joined, "codex_auth") ||
        rt::includes(joined, "kernel") || rt::includes(joined, "idempotency")) {
      return "mandatory_correctness";
    }
    return "background";
  }

  // The object `atomic()` returns: a closure over the helper's rows.
  class CountingAtomicOperation final : public rt::AtomicOperation {
  public:
    explicit CountingAtomicOperation(CountingKv& kv_) : kv(kv_) {}

    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;

    // `check: (entry) => ...` reads only its first argument.
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      if (items.empty()) throw rt::TypeError("undefined is not an object (evaluating 'entry.key')");
      checks.push_back(rt::AtomicCheck(items.front().key, items.front().versionstamp));
      return *this;
    }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("operation.mutate is not a function"); }
    rt::AtomicOperation& sum(rt::KvKey key, const rt::BigInt& n) override {
      mutations.push_back(AtomicMutation{.kind = "sum", .key = std::move(key), .value = {}, .amount = n});
      return *this;
    }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      mutations.push_back(AtomicMutation{.kind = "set", .key = std::move(key), .value = clone(value), .amount = {}});
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      mutations.push_back(AtomicMutation{.kind = "delete", .key = std::move(key), .value = {}, .amount = {}});
      return *this;
    }

    rt::Task<rt::KvCommitResult> commit() override {
      const KvMeasurementClassification classification = atomicClassification(checks, mutations);
      bool changed = false;
      for (const rt::AtomicCheck& check : checks) {
        const StoredEntry* current = kv.entries.get(encodeKey(check.key));
        const std::optional<std::string> currentVersionstamp = current ? std::optional<std::string>(current->versionstamp) : std::nullopt;
        if (currentVersionstamp != check.versionstamp) {
          changed = true;
          break;
        }
      }
      rt::KvCommitResult result;
      if (changed) {
        CountingKv::RecordDetails details;
        details.atomicChecks = static_cast<double>(checks.size());
        details.atomicMutations = static_cast<double>(mutations.size());
        details.atomicResult = "conflict";
        kv.record("atomic.commit", classification, keysOf(checks, mutations), details);
        result.ok = false;
      } else {
        const std::string versionstamp = kv.nextVersionstamp();
        for (const AtomicMutation& mutation : mutations) {
          const std::string encoded = encodeKey(mutation.key);
          if (mutation.kind == "delete") {
            kv.entries.delete_(encoded);
          } else if (mutation.kind == "set") {
            kv.entries.set(encoded, StoredEntry{mutation.key, clone(mutation.value), versionstamp});
          } else {
            const StoredEntry* current = kv.entries.get(encoded);
            const rt::BigInt currentValue = current && current->value.isU64() ? current->value.asU64().toBigInt() : rt::BigInt(0);
            kv.entries.set(encoded, StoredEntry{mutation.key, rt::KvValue(rt::KvU64(currentValue + mutation.amount)), versionstamp});
          }
        }
        CountingKv::RecordDetails details;
        details.writeMutations = static_cast<double>(mutations.size());
        details.atomicChecks = static_cast<double>(checks.size());
        details.atomicMutations = static_cast<double>(mutations.size());
        details.atomicResult = "committed";
        kv.record("atomic.commit", classification, keysOf(checks, mutations), details);
        result.ok = true;
        result.versionstamp = versionstamp;
      }
      co_await rt::microtask();
      co_return result;
    }

  private:
    CountingKv& kv;
    std::vector<rt::AtomicCheck> checks;
    std::vector<AtomicMutation> mutations;
  };

  // The generator `list` returns: a snapshot of the selected rows, cloned as they are yielded.
  class CountingListIterator final : public rt::KvListIterator {
  public:
    explicit CountingListIterator(std::vector<StoredEntry> selected_) : selected(std::move(selected_)) {}

    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      std::optional<rt::KvEntry<>> entry;
      if (!done && position < selected.size()) {
        const StoredEntry& stored = selected[position];
        position += 1;
        entry = rt::KvEntry<>{stored.key, clone(stored.value), stored.versionstamp};
      } else {
        done = true;
      }
      co_await rt::microtask();
      co_return entry;
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override {
      done = true;
      co_await rt::microtask();
    }

  private:
    std::vector<StoredEntry> selected;
    std::size_t position = 0;
    bool done = false;
  };

  void CountingKv::clearData() {
    entries.clear();
    nextVersion_ = 0;
  }

  void CountingKv::clearMeasurements() {
    commands.clear();
    budgets_.clear();
    activeScenario_.reset();
  }

  void CountingKv::seed(const rt::KvKey& key, const rt::KvValue& value) {
    const std::string encoded = encodeKey(key);
    entries.set(encoded, StoredEntry{key, clone(value), nextVersionstamp()});
  }

  std::function<void()> CountingKv::beginMeasurement(const KvMeasurementContext& context) {
    if (activeScenario_) throw rt::JsError("measurement already active: " + *activeScenario_);
    const std::string scenario = context.authKind + ":" + context.outcome;
    if (!budgets_.has(scenario)) budgets_.set(scenario, emptyBudget(context));
    activeScenario_ = scenario;
    return [this, scenario] {
      if (activeScenario_ != scenario) throw rt::JsError("measurement ended out of order: " + scenario);
      activeScenario_.reset();
    };
  }

  void CountingKv::setLatency(double milliseconds) {
    const KvOperationBudget* budget = activeBudget();
    if (!budget) return;
    KvOperationBudget next = *budget;
    next.latency_ms = rt::Math::max(0, rt::Math::round(milliseconds));
    budgets_.set(next.scenario, next);
  }

  void CountingKv::recordSerializedRequestBytes(double bytes) { addBytes("serialized_request_bytes", bytes); }

  void CountingKv::recordUpstreamResponseBytes(double bytes) { addBytes("upstream_response_bytes", bytes); }

  std::vector<KvOperationBudget> CountingKv::budgets() const {
    std::vector<KvOperationBudget> values = rt::arrayFrom(budgets_.values());
    rt::sort(values, [](const KvOperationBudget& left, const KvOperationBudget& right) { return rt::localeCompare(left.scenario, right.scenario); });
    return values;
  }

  rt::Task<rt::KvEntryMaybe<>> CountingKv::get(rt::KvKey key, rt::KvGetOptions) {
    record("get", classifyKvKey(key), std::vector<rt::KvKey>{key});
    const StoredEntry* entry = entries.get(encodeKey(key));
    rt::KvEntryMaybe<> result{key, entry ? clone(entry->value) : rt::KvValue(nullptr), entry ? std::optional<std::string>(entry->versionstamp) : std::nullopt};
    co_await rt::microtask();
    co_return result;
  }

  rt::Task<std::vector<rt::KvEntryMaybe<>>> CountingKv::getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions) {
    const KvMeasurementClassification classification = classifyCommands(keys);
    record("getMany", classification, keys);
    std::vector<rt::KvEntryMaybe<>> results;
    for (const rt::KvKey& key : keys) {
      const StoredEntry* entry = entries.get(encodeKey(key));
      results.push_back(rt::KvEntryMaybe<>{key, entry ? clone(entry->value) : rt::KvValue(nullptr),
                                           entry ? std::optional<std::string>(entry->versionstamp) : std::nullopt});
    }
    co_await rt::microtask();
    co_return results;
  }

  rt::Task<rt::KvCommitResult> CountingKv::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) {
    RecordDetails details;
    details.writeMutations = 1;
    record("set", classifyKvKey(key), std::vector<rt::KvKey>{key}, details);
    const std::string versionstamp = nextVersionstamp();
    entries.set(encodeKey(key), StoredEntry{key, clone(value), versionstamp});
    rt::KvCommitResult result;
    result.ok = true;
    result.versionstamp = versionstamp;
    co_await rt::microtask();
    co_return result;
  }

  rt::Task<void> CountingKv::delete_(rt::KvKey key) {
    RecordDetails details;
    details.writeMutations = 1;
    record("delete", classifyKvKey(key), std::vector<rt::KvKey>{key}, details);
    entries.delete_(encodeKey(key));
    nextVersionstamp();
    co_await rt::microtask();
  }

  std::unique_ptr<rt::KvListIterator> CountingKv::list(rt::KvListSelector selector, rt::KvListOptions) {
    const rt::KvKey prefix = selector.prefix ? *selector.prefix : rt::KvKey();
    record("list", classifyKvKey(prefix), std::vector<rt::KvKey>{prefix});
    std::vector<StoredEntry> selected;
    for (const StoredEntry& entry : entries.values()) {
      if (startsWithKey(entry.key, prefix)) selected.push_back(entry);
    }
    rt::sort(selected, [](const StoredEntry& left, const StoredEntry& right) { return rt::localeCompare(encodeKey(left.key), encodeKey(right.key)); });
    return std::make_unique<CountingListIterator>(std::move(selected));
  }

  std::unique_ptr<rt::AtomicOperation> CountingKv::atomic() { return std::make_unique<CountingAtomicOperation>(*this); }

  void CountingKv::close() {}

  KvMeasurementClassification CountingKv::classifyCommands(const std::vector<rt::KvKey>& keys) const {
    for (const rt::KvKey& key : keys) {
      if (classifyKvKey(key) == "mandatory_correctness") return "mandatory_correctness";
    }
    for (const rt::KvKey& key : keys) {
      if (classifyKvKey(key) == "optional_telemetry") return "optional_telemetry";
    }
    return "background";
  }

  std::string CountingKv::nextVersionstamp() {
    nextVersion_ += 1;
    return rt::padStart(rt::numberToString(nextVersion_), 20, "0");
  }

  const KvOperationBudget* CountingKv::activeBudget() const {
    return activeScenario_ && !activeScenario_->empty() ? budgets_.get(*activeScenario_) : nullptr;
  }

  void CountingKv::addBytes(const std::string& field, double bytes) {
    const KvOperationBudget* budget = activeBudget();
    if (!budget) return;
    const double amount = rt::Math::max(0, rt::Math::trunc(bytes));
    KvOperationBudget next = *budget;
    if (field == "serialized_request_bytes") next.serialized_request_bytes = budget->serialized_request_bytes + amount;
    else next.upstream_response_bytes = budget->upstream_response_bytes + amount;
    budgets_.set(next.scenario, next);
  }

  void CountingKv::record(const KvCommandName& command, const KvMeasurementClassification& classification, const std::vector<rt::KvKey>& keys) {
    record(command, classification, keys, RecordDetails());
  }

  void CountingKv::record(const KvCommandName& command, const KvMeasurementClassification& classification, const std::vector<rt::KvKey>& keys,
                          const RecordDetails& details) {
    commands.push_back(KvCommandRecord{
      .scenario = activeScenario_,
      .command = command,
      .classification = classification,
      .keys = keys,
      .keyCount = static_cast<double>(keys.size()),
      .atomicChecks = details.atomicChecks.value_or(0),
      .atomicMutations = details.atomicMutations.value_or(0),
      .atomicResult = details.atomicResult,
    });
    const KvOperationBudget* budget = activeBudget();
    if (!budget) return;
    const bool readCommand = command == "get" || command == "getMany" || command == "list";
    KvOperationBudget next = *budget;
    next.commands = budget->commands + 1;
    next.read_commands = budget->read_commands + (readCommand ? static_cast<double>(keys.size()) : 0);
    next.write_mutations = budget->write_mutations + details.writeMutations.value_or(0);
    next.atomic_commits = budget->atomic_commits + (command == "atomic.commit" ? 1 : 0);
    next.atomic_checks = budget->atomic_checks + details.atomicChecks.value_or(0);
    next.atomic_mutations = budget->atomic_mutations + details.atomicMutations.value_or(0);
    next.mandatory_correctness_commands = budget->mandatory_correctness_commands + (classification == "mandatory_correctness" ? 1 : 0);
    next.optional_telemetry_commands = budget->optional_telemetry_commands + (classification == "optional_telemetry" ? 1 : 0);
    next.background_commands = budget->background_commands + (classification == "background" ? 1 : 0);
    budgets_.set(next.scenario, next);
  }
}
