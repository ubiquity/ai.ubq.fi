// Links: tests/cache_kv_migration_differential_test.cpp
//        tests/helpers/counting_kv.cpp src/kv_conversion/profile_json.cpp src/cache/kv_migration_paid_fallback.cpp src/authority/native_paid.cpp
//        src/cache/kv_migration_projection.cpp src/authority/native_paid_policy.cpp src/config.cpp src/utils.cpp
//        src/release.cpp src/types.cpp src/paid_fallback/ledger_state.cpp src/authority/native_paid_contracts.cpp
//        src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp
// Credential-free Bun 1.4.0 differentials: 664 guard vectors, five ledger identities and 29 named
// inventories/projections. Inputs and outputs are profile-json typed values. Network is not used.
#include "uos/cache/kv_migration_paid_fallback.hpp"
#include "uos/cache/kv_migration_projection.hpp"
#include "uos/config.hpp"
#include "uos/kv_conversion/profile_json.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "tests/helpers/counting_kv.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace paid = uos::cache::kv_migration_paid_fallback;
namespace projection = uos::cache::kv_migration_projection;
namespace profile = uos::kv_conversion::profile_json;
namespace counting = uos::tests::helpers::counting_kv;

namespace {
#include "tests/cache_kv_migration_differential_vectors.inc"
  constexpr double NOW = 1800000000000;
  const json::value& vectors() {
    static const json::value value = [] {
      const auto compressed = rt::fromBase64(MIGRATION_VECTORS_GZIP_BASE64);
      const auto bytes = rt::gzip::gunzipSync(compressed);
      return rt::jsonParse(rt::decodeUtf8(bytes));
    }(); return value;
  }
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c8-isolated"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = saved; }
  };
  json::value errorResult(const rt::Reason& reason) {
    return json::object{{"error", json::object{{"name", rt::errorName(reason).value_or("Error")}, {"message", rt::errorMessage(reason).value_or(rt::reasonToString(reason))}}}};
  }
  void equal(const json::value& actual, const json::value& expected, const std::string& name) {
    if (!rt::deepEqual(actual, expected)) rt::test::report(__FILE__, __LINE__, name + "\nactual: " + rt::jsonStringify(actual) + "\nexpected: " + rt::jsonStringify(expected), false);
  }
  bool guard(const std::string& name, const rt::KvValue& value) {
    if (name == "isSafeUsageCount") return paid::isSafeUsageCount(value);
    if (name == "isApiKeyId") return paid::isApiKeyId(value);
    if (name == "isApiKeyHash") return paid::isApiKeyHash(value);
    if (name == "isPositiveSafeInteger") return paid::isPositiveSafeInteger(value);
    if (name == "isFiniteNonNegativeNumber") return paid::isFiniteNonNegativeNumber(value);
    if (name == "isPendingPaidFallbackLedgerRecord") return paid::isPendingPaidFallbackLedgerRecord(value);
    if (name == "hasStrictApiKeyHashCorePolicy") return paid::hasStrictApiKeyHashCorePolicy(value);
    if (name == "hasStrictApiKeyCorePolicy") return paid::hasStrictApiKeyCorePolicy(value);
    if (name == "isPaidFallbackRequestV3") return paid::isPaidFallbackRequestV3(value);
    throw std::logic_error("unknown guard " + name);
  }
  class RefusingAtomic final : public rt::ForwardingAtomicOperation {
  public:
    using rt::ForwardingAtomicOperation::ForwardingAtomicOperation;
    rt::Task<rt::KvCommitResult> commit() override { co_await rt::microtask(); co_return rt::KvCommitResult{}; }
  };
  class ScenarioKv final : public counting::CountingKv {
  public:
    bool failCommits = false;
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      auto inner = counting::CountingKv::atomic();
      if (failCommits) return std::make_unique<RefusingAtomic>(std::move(inner));
      return inner;
    }
  };
  json::value rowsOf(const ScenarioKv& kv) {
    std::vector<json::value> rows;
    for (const auto& entry : kv.entries.values()) rows.emplace_back(json::object{{"key", json::value_from(profile::keyToJson(entry.key))}, {"value", profile::valueToJson(entry.value)}});
    rt::sort(rows, [](const json::value& a, const json::value& b) { return static_cast<double>(rt::localeCompare(rt::jsonStringify(a.at("key")), rt::jsonStringify(b.at("key")))); });
    json::array result; for (const auto& row : rows) result.push_back(row); return result;
  }
  json::value inventoryJson(const paid::PaidFallbackV3Inventory& value) {
    return json::object{{"windows", rt::makeNumber(value.windows)}, {"requests", rt::makeNumber(value.requests)}, {"pending", rt::makeNumber(value.pending)},
      {"reconciliationLeases", rt::makeNumber(value.reconciliationLeases)}, {"deletionGuards", rt::makeNumber(value.deletionGuards)}, {"errors", json::value_from(value.errors)}};
  }
  std::size_t currentScenario = 0;
  rt::Task<void> runScenario() {
    CredentialScope scope; rt::globalDateNow() = [] { return NOW; };
    const auto& row = vectors().at("scenarios").as_array()[currentScenario]; const std::string name(*rt::getString(row, "name"));
    auto kv = std::make_shared<ScenarioKv>();
    for (const auto& entry : row.at("rows").as_array()) kv->seed(profile::toKey(entry.at("key").as_array()), profile::toValue(entry.at("value")));
    kv->failCommits = *rt::getBool(row, "fail");
    std::vector<uos::cache::kv_migration_usage::StrictApiKeyPair> pairs;
    for (const auto& pair : row.at("pairs").as_array()) pairs.push_back({.record = profile::toValue(pair.at("record")), .hashRecord = profile::toValue(pair.at("hashRecord"))});
    rt::OrderedSet<std::string> known; for (const auto& key : row.at("known").as_array()) known.add(std::string(key.as_string()));
    rt::OrderedSet<std::string> unlimited; for (const auto& key : row.at("unlimited").as_array()) unlimited.add(std::string(key.as_string()));
    json::value result; const std::string op(*rt::getString(row, "op"));
    try {
      if (op == "paid") {
        auto task = paid::inspectPaidFallbackV3(kv, known, unlimited, rt::toNumber(row.at("now"))); const auto inspected = co_await std::move(task);
        result = json::object{{"value", inventoryJson(inspected)}};
      } else if (op == "project") {
        auto task = projection::projectLegacyPaidFallbackV3(kv, pairs, NOW); const auto projected = co_await std::move(task);
        result = json::object{{"value", json::object{{"projected", rt::makeNumber(projected.projected)}, {"pending", rt::makeNumber(projected.pending)}}}};
      } else {
        auto task = projection::inspectPendingPaidFallbackLedgers(kv, pairs); const auto inspected = co_await std::move(task);
        json::array candidates;
        for (const auto& item : inspected.candidates) candidates.emplace_back(json::object{{"value", profile::valueToJson(item.value)}, {"ledgerKey", json::value_from(profile::keyToJson(item.ledgerKey))}, {"existing", profile::valueToJson(item.existing.value)}});
        result = json::object{{"value", json::object{{"errors", json::value_from(inspected.errors)}, {"candidates", candidates}}}};
      }
    } catch (...) { result = errorResult(std::current_exception()); }
    equal(result, row.at("expected"), name + " result"); equal(rowsOf(*kv), row.at("expectedRows"), name + " durable rows");
  }
  const bool registeredScenarios = [] {
    const auto& scenarios = vectors().at("scenarios").as_array();
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
      const std::string name = "Bun differential: " + std::string(*rt::getString(scenarios[index], "name"));
      rt::test::registry().push_back({name, [index] { currentScenario = index; rt::test::runAsync(runScenario); }});
    }
    return true;
  }();
}
TEST("paid migration guards match all 664 credential-free Bun vectors") {
  CredentialScope scope;
  for (const auto& row : vectors().at("guards").as_array()) {
    const std::string name(*rt::getString(row, "fn")); const rt::KvValue value = profile::toValue(row.at("input")); json::value actual;
    try { actual = json::object{{"value", guard(name, value)}}; } catch (...) { actual = errorResult(std::current_exception()); }
    equal(actual, row.at("expected"), name + " " + rt::jsonStringify(row.at("input")));
  }
}
TEST("paid migration ledger identities match Bun") {
  CredentialScope scope;
  for (const auto& row : vectors().at("identity").as_array()) {
    const rt::KvEntryMaybe<> entry{.key = profile::toKey(row.at("key").as_array()), .value = profile::toValue(row.at("input")), .versionstamp = "1"};
    const auto reference = paid::pendingPaidFallbackLedgerReferenceFromEntry(entry, paid::PAID_FALLBACK_LEDGER_PREFIX);
    json::object result; rt::objectSet(result, "reference", rt::orNull(reference)); result["matches"] = paid::paidFallbackLedgerEntryMatchesIdentity(entry, profile::toValue(row.at("expectedRecord")));
    equal(result, row.at("expected"), "ledger identity " + rt::jsonStringify(row.at("key")));
  }
}
