#include "tests/helpers/codex_diff_kv.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <cstdio>
#include <utility>

namespace uos::tests::helpers::codex_diff_kv {
  namespace {
    class DiffAtomic final : public rt::AtomicOperation {
    public:
      explicit DiffAtomic(DiffKv& owner_) : owner(owner_) {}
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        for (rt::AtomicCheck& item : items) checks.push_back(std::move(item));
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("op.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("op.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("op.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("op.max is not a function"); }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
        mutations.push_back(Mutation{false, std::move(key), std::move(value)});
        return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override {
        mutations.push_back(Mutation{true, std::move(key), rt::KvValue()});
        return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override {
        rt::KvCommitResult result;
        for (const rt::AtomicCheck& check : checks) {
          const DiffKv::Row* row = owner.rows.get(keyOf(check.key));
          const std::optional<std::string> current = row != nullptr ? std::optional<std::string>(row->versionstamp) : std::nullopt;
          const std::optional<std::string> wanted = check.versionstamp && !check.versionstamp->empty() ? check.versionstamp : std::nullopt;
          if (current != wanted) co_return result;
        }
        const std::string versionstamp = owner.stamp();
        for (const Mutation& mutation : mutations) {
          if (mutation.remove) owner.rows.delete_(keyOf(mutation.key));
          else owner.rows.set(keyOf(mutation.key), DiffKv::Row{mutation.key, mutation.value, versionstamp});
        }
        result.ok = true;
        result.versionstamp = versionstamp;
        co_return result;
      }

    private:
      struct Mutation {
        bool remove;
        rt::KvKey key;
        rt::KvValue value;
      };
      DiffKv& owner;
      std::vector<rt::AtomicCheck> checks;
      std::vector<Mutation> mutations;
    };

    void mask(json::value& value) {
      static const rt::Regex UUID("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", "");
      if (value.is_string()) {
        if (UUID.test(std::string_view(value.get_string()))) value = "<uuid>";
      } else if (value.is_array()) {
        for (json::value& item : value.get_array()) mask(item);
      } else if (value.is_object()) {
        for (auto& member : value.get_object()) mask(member.value());
      }
    }

    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
    json::value stringOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
  }

  std::string keyOf(const rt::KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index > 0) out += ",";
      const rt::KvKeyPart& part = key[index];
      out += part.isString() ? rt::jsonQuote(part.string()) : part.toString();
    }
    return out + "]";
  }

  std::string DiffKv::stamp() {
    version += 1;
    return rt::padStart(rt::numberToString(version), 20, "0");
  }

  rt::KvEntryMaybe<> DiffKv::entryOf(const rt::KvKey& key) const {
    rt::KvEntryMaybe<> entry;
    entry.key = key;
    if (const Row* row = rows.get(keyOf(key))) {
      entry.value = row->value;
      entry.versionstamp = row->versionstamp;
    } else {
      entry.value = rt::KvValue(nullptr);
    }
    return entry;
  }

  rt::Task<rt::KvEntryMaybe<>> DiffKv::get(rt::KvKey key, rt::KvGetOptions) { co_return entryOf(key); }

  rt::Task<std::vector<rt::KvEntryMaybe<>>> DiffKv::getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions) {
    std::vector<rt::KvEntryMaybe<>> entries;
    for (const rt::KvKey& key : keys) entries.push_back(entryOf(key));
    co_return entries;
  }

  rt::Task<rt::KvCommitResult> DiffKv::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) {
    rt::KvCommitResult result;
    result.versionstamp = stamp();
    rows.set(keyOf(key), Row{key, value, result.versionstamp});
    result.ok = true;
    co_return result;
  }

  rt::Task<void> DiffKv::delete_(rt::KvKey key) {
    stamp();
    rows.delete_(keyOf(key));
    co_return;
  }

  std::unique_ptr<rt::KvListIterator> DiffKv::list(rt::KvListSelector, rt::KvListOptions) { throw rt::TypeError("kv.list is not a function"); }

  std::unique_ptr<rt::AtomicOperation> DiffKv::atomic() { return std::make_unique<DiffAtomic>(*this); }

  json::value DiffKv::row(const rt::KvKey& key) const {
    const Row* stored = rows.get(keyOf(key));
    return stored != nullptr ? stored->value.toJson() : json::value(nullptr);
  }

  json::value plain(json::value value) {
    mask(value);
    return value;
  }

  json::value routingAccountJson(const codex::routing_state::RoutingAccount& account) {
    json::object out;
    out["auth"] = json::value_from(account.auth);
    out["slot"] = rt::makeNumber(account.slot);
    out["accountIdHash"] = account.accountIdHash;
    out["credentialVersion"] = account.credentialVersion;
    out["quotaHeadroom"] = numberOrNull(account.quotaHeadroom);
    out["probeRequired"] = account.probeRequired;
    out["probeGeneration"] = numberOrNull(account.probeGeneration);
    out["probeToken"] = stringOrNull(account.probeToken);
    rt::objectSet(out, "probeCircuit", account.probeCircuit);
    rt::objectSet(out, "overageFallback", account.overageFallback);
    if (account.routingGeneration) out["routingGeneration"] = rt::makeNumber(*account.routingGeneration);
    if (account.activeGeneration) out["activeGeneration"] = rt::makeNumber(*account.activeGeneration);
    rt::objectSet(out, "activePoolVersionstamp", account.activePoolVersionstamp);
    rt::objectSet(out, "activeTransitionReason", account.activeTransitionReason);
    rt::objectSet(out, "requestedModel", account.requestedModel);
    return plain(json::value(std::move(out)));
  }

  json::value routingAccountJson(const std::optional<codex::routing_state::RoutingAccount>& account) {
    return account ? routingAccountJson(*account) : json::value(nullptr);
  }

  bool sameVector(const json::object& expected, const json::object& actual, std::string_view name) {
    const json::value* want = rt::get(expected, name);
    const json::value* got = rt::get(actual, name);
    if (want != nullptr && got != nullptr && rt::deepEqual(*want, *got)) return true;
    std::fprintf(stderr, "    vector %.*s differs\n      expected: %s\n      actual:   %s\n", static_cast<int>(name.size()), name.data(),
                 want != nullptr ? rt::jsonStringify(*want).c_str() : "<missing>", got != nullptr ? rt::jsonStringify(*got).c_str() : "<missing>");
    return false;
  }
}
