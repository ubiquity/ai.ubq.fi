// Links: tests/provider_selection_health_differential_test.cpp src/provider/presentation.cpp src/provider/selection.cpp src/provider/health.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/kv.cpp src/storage/kv.cpp src/storage/key_compatibility.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp
// Differential cases for src/provider/presentation.ts, src/provider/selection.ts and src/provider/health.ts (no
// direct bun test of these modules is portable yet). The vectors (tests/provider_selection_health_differential_vectors.inc)
// were recorded from the TS modules under bun 1.4.0 with an in-memory Deno KV whose operations complete one macrotask
// later and, for the selection KV scenario, Date.now fixed. The KV scenarios repeat the generator's steps one for one.
// Inputs tag `undefined` as {"$undefined": true} and non-finite numbers as {"$num": text}.
#include "uos/kv.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace presentation = provider::presentation;
namespace selection = provider::selection;
namespace health = provider::health;

namespace {
  const char* const VECTORS =
#include "tests/provider_selection_health_differential_vectors.inc"
    ;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTORS).as_object();
    return parsed;
  }
  const json::value& vector(std::string_view key) { return vectors().at(key); }

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, label + "\n      actual:   " + json::serialize(actual) + "\n      expected: " + json::serialize(expected), false);
  }
#define EXPECT_JSON(actual, expected, label) expectJson((actual), (expected), (label), __FILE__, __LINE__)

  bool isUndefinedTag(const json::value& value) {
    const json::object* object = value.if_object();
    return object && object->size() == 1 && object->contains("$undefined");
  }
  const json::value* numberTag(const json::value& value) {
    const json::object* object = value.if_object();
    return object && object->size() == 1 ? rt::get(*object, "$num") : nullptr;
  }

  // A tagged input as JSON; undefined (only reachable inside arrays and members here) becomes null,
  // which every guard under test reads exactly like undefined.
  json::value untag(const json::value& value) {
    if (isUndefinedTag(value)) return nullptr;
    if (const json::value* number = numberTag(value)) return rt::makeNumber(rt::stringToNumber(number->as_string()));
    if (const json::array* array = value.if_array()) {
      json::array out;
      for (const json::value& item : *array) out.push_back(untag(item));
      return out;
    }
    if (const json::object* object = value.if_object()) {
      json::object out;
      for (const auto& [key, item] : *object) {
        if (!isUndefinedTag(item)) out[key] = untag(item);
      }
      return out;
    }
    return value;
  }

  // A tagged input as a stored value (undefined members stay present-but-undefined).
  rt::KvValue storedOf(const json::value& value) {
    if (isUndefinedTag(value)) return rt::KvValue();
    if (const json::value* number = numberTag(value)) return rt::KvValue(rt::stringToNumber(number->as_string()));
    if (const json::array* array = value.if_array()) {
      rt::KvArray out;
      for (const json::value& item : *array) out.push_back(storedOf(item));
      return rt::KvValue(std::move(out));
    }
    if (const json::object* object = value.if_object()) {
      rt::KvObject out;
      for (const auto& [key, item] : *object) out.set(std::string(key), storedOf(item));
      return rt::KvValue(std::move(out));
    }
    return rt::KvValue(value);
  }

  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(json::string(*value)) : json::value(nullptr); }
  template <class T> json::value plain(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }

  std::optional<selection::ProviderSelection> selectionOf(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return json::value_to<selection::ProviderSelection>(value);
  }

  json::value eligibilityJson(const selection::CodexAccountEligibility& eligibility) {
    json::object out;
    std::visit([&out](const auto& arm) { out["kind"] = json::string(arm.kind); }, eligibility);
    if (const auto* only = std::get_if<selection::CodexAccountEligibilityOnly>(&eligibility)) out["hashes"] = json::value_from(only->hashes);
    return out;
  }

  struct CatalogProvider {
    std::string id;
  };
  struct CatalogEntry {
    std::string model;
    std::vector<CatalogProvider> providers;
  };
  json::value catalogJson(const std::vector<CatalogEntry>& entries) {
    json::array out;
    for (const CatalogEntry& entry : entries) {
      json::array providers;
      for (const CatalogProvider& provider : entry.providers) {
        json::object item;
        item["id"] = json::string(provider.id);
        providers.push_back(std::move(item));
      }
      json::object row;
      row["model"] = json::string(entry.model);
      row["providers"] = std::move(providers);
      out.push_back(std::move(row));
    }
    return out;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) out.emplace_back(json::string(part.string()));
      else out.push_back(rt::makeNumber(part.number()));
    }
    return out;
  }

  // `[...kv.rows.values()]` of the generator's fake store, sorted by the key's JSON text.
  rt::Task<json::value> rowsOf(std::shared_ptr<rt::Kv> kv) {
    const rt::KvListSelector selector{.prefix = rt::KvKey{"uos_ai"}};
    auto iterator = kv->list(selector);
    std::vector<std::pair<std::string, json::value>> rows;
    while (auto entry = co_await iterator->next()) {
      json::object row;
      row["key"] = keyJson(entry->key);
      row["value"] = entry->value.toJson();
      rows.emplace_back(rt::jsonStringify(row["key"]), std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    json::array out;
    for (auto& [text, row] : rows) out.push_back(std::move(row));
    co_return out;
  }

  // The generator's `kv.get = ...` / `kv.set = ...` overrides on one store.
  class ScenarioKv final : public rt::ForwardingKv {
  public:
    explicit ScenarioKv(std::shared_ptr<rt::Kv> inner_) : rt::ForwardingKv(*inner_), owned(std::move(inner_)) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      if (countReads) reads += 1;
      if (failGets) throw rt::JsError("kv down");
      auto task = rt::ForwardingKv::get(std::move(key), options);
      rt::KvEntryMaybe<> entry = co_await std::move(task);
      co_return entry;
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      if (failSets) throw rt::JsError("write refused");
      auto task = rt::ForwardingKv::set(std::move(key), std::move(value), options);
      rt::KvCommitResult result = co_await std::move(task);
      co_return result;
    }
    std::shared_ptr<rt::Kv> owned;
    bool countReads = false;
    bool failGets = false;
    bool failSets = false;
    int reads = 0;
  };

  std::shared_ptr<rt::Kv> memoryKv() {
    return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  }

  std::function<double()> clockAt(double value) {
    return [value] { return value; };
  }

  // `await kv.set(PROVIDER_SELECTION_KV_KEY, { provider_ids: [id], updated_at_ms })`
  rt::Task<void> replaceSelection(std::shared_ptr<rt::Kv> kv, std::string id, double updatedAtMs) {
    json::object value;
    value["provider_ids"] = json::array{json::string(id)};
    value["updated_at_ms"] = rt::makeNumber(updatedAtMs);
    const rt::KvValue stored(value);
    auto task = kv->set(selection::PROVIDER_SELECTION_KV_KEY, stored);
    co_await std::move(task);
  }
}

TEST("presentation: tiers, the gateway row and the roster table match the TS literals") {
  const json::object& expected = vector("presentation").as_object();
  json::array tiers;
  for (const presentation::ProviderTier& tier : presentation::PROVIDER_TIERS) {
    json::object item;
    item["id"] = json::string(tier.id);
    item["label"] = json::string(tier.label);
    tiers.push_back(std::move(item));
  }
  EXPECT_JSON(tiers, expected.at("tiers"), "PROVIDER_TIERS");
  EXPECT_JSON(json::value_from(presentation::GATEWAY_PROVIDER_PRESENTATION), expected.at("gateway"), "GATEWAY_PROVIDER_PRESENTATION");
  json::object table;
  for (const auto& [id, row] : presentation::PROVIDER_PRESENTATION) table[id] = json::value_from(row);
  EXPECT_JSON(table, expected.at("table"), "PROVIDER_PRESENTATION");
  EXPECT_EQ(rt::jsonStringify(table), rt::jsonStringify(expected.at("table")));
  EXPECT_EQ(std::string(presentation::GATEWAY_PROVIDER_ID), std::string("ubiquity"));
  for (const json::value& lookup : expected.at("lookups").as_array()) {
    const std::string id(lookup.at("id").as_string());
    EXPECT_JSON(json::value_from(presentation::providerPresentation(id)), lookup.at("output"), "providerPresentation(" + id + ")");
  }
  EXPECT(json::value_to<presentation::ProviderPresentation>(json::value_from(presentation::GATEWAY_PROVIDER_PRESENTATION)) ==
         presentation::GATEWAY_PROVIDER_PRESENTATION);
}

TEST("selection: id guards over unknown values") {
  for (const json::value& row : vector("guards").as_array()) {
    const json::value& input = row.at("input");
    const json::value value = untag(input);
    const json::value* argument = isUndefinedTag(input) ? nullptr : &value;
    const std::string label = rt::jsonStringify(input);
    EXPECT_EQ(selection::isCodexSubscriptionSelectionId(argument), row.at("isCodexSubscriptionSelectionId").as_bool());
    EXPECT_JSON(orNull(selection::codexSubscriptionHashFromSelectionId(argument)), row.at("codexSubscriptionHashFromSelectionId"), label);
    EXPECT_EQ(selection::isProviderSelectionId(argument), row.at("isProviderSelectionId").as_bool());
    EXPECT_EQ(selection::isSelectableProviderId(argument), row.at("isSelectableProviderId").as_bool());
    if (value.is_string()) {
      const std::string_view text(value.get_string());
      EXPECT_EQ(selection::isCodexSubscriptionSelectionId(text), row.at("isCodexSubscriptionSelectionId").as_bool());
      EXPECT_JSON(orNull(selection::codexSubscriptionHashFromSelectionId(text)), row.at("codexSubscriptionHashFromSelectionId"), label);
      EXPECT_EQ(selection::isProviderSelectionId(text), row.at("isProviderSelectionId").as_bool());
      EXPECT_EQ(selection::isSelectableProviderId(text), row.at("isSelectableProviderId").as_bool());
    }
  }
  EXPECT_EQ(selection::codexSubscriptionSelectionId("abc"), std::string(vector("selectionId").as_string()));
}

TEST("selection: submitted ids normalise to roster order then sorted subscriptions") {
  for (const json::value& row : vector("normalizeIds").as_array()) {
    const json::value input = untag(row.at("input"));
    EXPECT_JSON(json::value_from(selection::normalizeSelectedProviderIds(input.as_array())), row.at("output"), rt::jsonStringify(row.at("input")));
    std::vector<std::string> strings;
    bool allStrings = true;
    for (const json::value& item : input.as_array()) {
      if (item.is_string()) strings.emplace_back(item.get_string());
      else allStrings = false;
    }
    if (allStrings) EXPECT_JSON(json::value_from(selection::normalizeSelectedProviderIds(strings)), row.at("output"), "strings " + rt::jsonStringify(input));
  }
}

TEST("selection: stored selections validate and normalise") {
  for (const json::value& row : vector("normalizeSelection").as_array()) {
    const rt::KvValue stored = storedOf(row.at("input"));
    EXPECT_JSON(plain(selection::normalizeProviderSelection(stored)), row.at("output"), rt::jsonStringify(row.at("input")));
  }
  // An array hole is skipped by both `some` and `for...of` + getString.
  rt::KvObject holey;
  holey.set("provider_ids", rt::KvValue(rt::KvArray{rt::KvValue("surplus"), rt::KvValue(rt::KvHole{}), rt::KvValue("codex")}));
  holey.set("updated_at_ms", rt::KvValue(3));
  const std::optional<selection::ProviderSelection> read = selection::normalizeProviderSelection(rt::KvValue(std::move(holey)));
  ASSERT(read.has_value());
  EXPECT_EQ(read->provider_ids, (std::vector<std::string>{"codex", "surplus"}));
}

TEST("selection: enforcement, eligibility and catalog filtering") {
  for (const json::value& row : vector("enforcement").as_array()) {
    const std::optional<selection::ProviderSelection> value = selectionOf(row.at("selection"));
    const std::string label = rt::jsonStringify(row.at("selection"));
    EXPECT_EQ(selection::providerSelectionIsActive(value), row.at("active").as_bool());
    const std::vector<std::string> providers = {"codex", "claude_code", "gemini", "surplus", "openlux", "deepseek", "cerebras", "lithos", "openrouter", "ubiquity", "unknown"};
    json::array enabled;
    for (const std::string& provider : providers) enabled.emplace_back(selection::isProviderEnabled(provider, value));
    EXPECT_JSON(enabled, row.at("enabled"), label);
    EXPECT_JSON(eligibilityJson(selection::codexAccountEligibility(value)), row.at("eligibility"), label);
    json::array subscriptions;
    for (const std::string& hash : {std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c'), std::string("x")})
      subscriptions.emplace_back(selection::isCodexSubscriptionEnabled(hash, value));
    EXPECT_JSON(subscriptions, row.at("subscriptionEnabled"), label);
  }
  std::vector<CatalogEntry> catalog;
  for (const json::value& entry : vector("catalog").as_array()) {
    CatalogEntry row{.model = std::string(entry.at("model").as_string()), .providers = {}};
    for (const json::value& provider : entry.at("providers").as_array()) row.providers.push_back(CatalogProvider{.id = std::string(provider.at("id").as_string())});
    catalog.push_back(std::move(row));
  }
  for (const json::value& row : vector("catalogFilter").as_array()) {
    const std::optional<selection::ProviderSelection> value = selectionOf(row.at("selection"));
    EXPECT_JSON(catalogJson(selection::filterCatalogEntriesByProviderSelection(catalog, value)), row.at("output"), rt::jsonStringify(row.at("selection")));
  }
}

ASYNC_TEST("selection: the subscription hash is the unsalted account digest, cached per account") {
  auto first = selection::codexSubscriptionHash("acct-123");
  const std::string hash = co_await std::move(first);
  EXPECT_EQ(hash, std::string(vector("subscriptionHash").as_string()));
  auto second = selection::codexSubscriptionHash("acct-123");
  const std::string cached = co_await std::move(second);
  EXPECT_EQ(cached, std::string(vector("subscriptionHashCached").as_string()));
}

ASYNC_TEST("selection: store, load and the routing cache over the KV") {
  constexpr double NOW = 1'700'000'000'000;
  const json::object& expected = vector("selectionKv").as_object();
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [] { return NOW; };
  auto scenario = std::make_shared<ScenarioKv>(memoryKv());
  std::shared_ptr<rt::Kv> kv = scenario;
  kv::setKvForTest(kv);
  selection::resetProviderSelectionCacheForTest();
  const auto selectionJson = [](const std::optional<selection::ProviderSelection>& value) { return plain(value); };

  auto loadNull = selection::loadProviderSelection(nullptr);
  const auto loadedNull = co_await std::move(loadNull);
  EXPECT_JSON(selectionJson(loadedNull), expected.at("loadNull"), "loadNull");
  auto loadEmpty = selection::loadProviderSelection(kv);
  const auto loadedEmpty = co_await std::move(loadEmpty);
  EXPECT_JSON(selectionJson(loadedEmpty), expected.at("loadEmpty"), "loadEmpty");
  std::vector<std::string> ids = {"surplus", "codex", "codex:" + std::string(64, 'a'), "bogus"};
  auto store = selection::storeProviderSelection(kv, ids);
  const auto stored = co_await std::move(store);
  EXPECT_JSON(selectionJson(stored), expected.at("store"), "store");
  auto row = kv->get(selection::PROVIDER_SELECTION_KV_KEY);
  const rt::KvEntryMaybe<> storedRow = co_await std::move(row);
  EXPECT_JSON(storedRow.value.toJson(), expected.at("stored"), "stored");
  auto load = selection::loadProviderSelection(kv);
  const auto loaded = co_await std::move(load);
  EXPECT_JSON(selectionJson(loaded), expected.at("load"), "load");

  auto lithos = replaceSelection(kv, "lithos", 5);
  co_await std::move(lithos);
  auto beforeExpiry = selection::loadProviderSelectionCached(NOW + 4'999);
  const auto cachedBeforeExpiry = co_await std::move(beforeExpiry);
  EXPECT_JSON(selectionJson(cachedBeforeExpiry), expected.at("cachedBeforeExpiry"), "cachedBeforeExpiry");
  auto atExpiry = selection::loadProviderSelectionCached(NOW + 5'000);
  const auto cachedAtExpiry = co_await std::move(atExpiry);
  EXPECT_JSON(selectionJson(cachedAtExpiry), expected.at("cachedAtExpiry"), "cachedAtExpiry");
  auto deepseek = replaceSelection(kv, "deepseek", 6);
  co_await std::move(deepseek);
  auto afterReload = selection::loadProviderSelectionCached(NOW + 9'999);
  const auto cachedAfterReload = co_await std::move(afterReload);
  EXPECT_JSON(selectionJson(cachedAfterReload), expected.at("cachedAfterReload"), "cachedAfterReload");
  auto reload = selection::loadProviderSelectionCached(NOW + 10'000);
  const auto reloaded = co_await std::move(reload);
  EXPECT_JSON(selectionJson(reloaded), expected.at("reloaded"), "reloaded");

  selection::resetProviderSelectionCacheForTest();
  scenario->countReads = true;
  rt::Promise<std::optional<selection::ProviderSelection>> first(selection::loadProviderSelectionCached(NOW + 20'000));
  rt::Promise<std::optional<selection::ProviderSelection>> second(selection::loadProviderSelectionCached(NOW + 20'000));
  const auto firstValue = co_await first.wait();
  const auto secondValue = co_await second.wait();
  json::array concurrent{selectionJson(firstValue), selectionJson(secondValue)};
  EXPECT_JSON(concurrent, expected.at("concurrent"), "concurrent");
  EXPECT_EQ(static_cast<double>(scenario->reads), rt::toNumber(expected.at("concurrentReads")));

  scenario->failGets = true;
  auto keepsStale = selection::loadProviderSelectionCached(NOW + 30'000);
  const auto failedKeepsStale = co_await std::move(keepsStale);
  EXPECT_JSON(selectionJson(failedKeepsStale), expected.at("failedKeepsStale"), "failedKeepsStale");
  selection::resetProviderSelectionCacheForTest();
  auto failOpen = selection::loadProviderSelectionCached(NOW + 30'000);
  const auto failedOpen = co_await std::move(failOpen);
  EXPECT_JSON(selectionJson(failedOpen), expected.at("failedOpen"), "failedOpen");

  scenario->failGets = false;
  scenario->failSets = true;
  std::vector<std::string> codexOnly = {"codex"};
  auto rejected = selection::storeProviderSelection(kv, codexOnly);
  const auto storeRejected = co_await std::move(rejected);
  EXPECT_JSON(selectionJson(storeRejected), expected.at("storeRejected"), "storeRejected");
  scenario->failSets = false;

  selection::resetProviderSelectionCacheForTest();
  auto openlux = replaceSelection(kv, "openlux", 7);
  co_await std::move(openlux);
  rt::Promise<std::optional<selection::ProviderSelection>> loading(selection::loadProviderSelectionCached(NOW + 40'000));
  std::vector<std::string> cerebras = {"cerebras"};
  auto racingStore = selection::storeProviderSelection(kv, cerebras);
  const auto storedDuringLoad = co_await std::move(racingStore);
  const auto racingLoad = co_await loading.wait();
  EXPECT_JSON(selectionJson(racingLoad), expected.at("racingLoad"), "racingLoad");
  EXPECT_JSON(selectionJson(storedDuringLoad), expected.at("racingStore"), "racingStore");
  auto after = selection::loadProviderSelectionCached(NOW + 40'001);
  const auto afterRace = co_await std::move(after);
  EXPECT_JSON(selectionJson(afterRace), expected.at("afterRace"), "afterRace");

  selection::resetProviderSelectionCacheForTest();
  kv::setKvForTest(nullptr);
  rt::globalDateNow() = realNow;
}

TEST("health: stored observations validate as the TS guard does") {
  for (const json::value& row : vector("parseObservation").as_array()) {
    const rt::KvValue stored = storedOf(row.at("input"));
    // The TS returns the validated value itself; the typed record keeps the declared members
    // (nothing reads another one), so undeclared stored members are dropped from the expectation.
    json::value expected = row.at("output");
    if (json::object* object = expected.if_object()) {
      json::object declared;
      for (const std::string_view key : {"event", "status", "observed_at_ms", "provider_request_id"}) {
        if (const json::value* member = rt::get(*object, key)) declared[key] = *member;
      }
      expected = std::move(declared);
    }
    EXPECT_JSON(plain(health::parseProviderHealthObservation(stored)), expected, rt::jsonStringify(row.at("input")));
  }
}

ASYNC_TEST("health: transitions, throttling, correlation ids and views over the KV") {
  constexpr double T = 1'800'000'000'000;
  std::shared_ptr<rt::Kv> kv = memoryKv();
  kv::setKvForTest(kv);
  health::resetProviderHealthThrottleForTest();
  const json::array& steps = vector("health").as_array();
  std::size_t index = 0;
  const json::value undefinedResult = json::object{{"$undefined", true}};
  const auto expectStep = [&](std::string_view label, const json::value& result, const json::value& rows) {
    ASSERT(index < steps.size());
    const json::value& step = steps[index++];
    EXPECT_EQ(std::string(step.at("label").as_string()), std::string(label));
    EXPECT_JSON(result, step.at("result"), std::string(label) + " result");
    EXPECT_JSON(rows, step.at("rows"), std::string(label) + " rows");
  };
  // One record / read step: awaits it, then compares the result and the whole store.
  const auto recordStep = [&](std::string_view label, rt::Promise<void> recording) -> rt::Task<void> {
    co_await recording.wait();
    auto listing = rowsOf(kv);
    const json::value rows = co_await std::move(listing);
    expectStep(label, undefinedResult, rows);
  };
  const auto viewStep = [&](std::string_view label, rt::Task<health::ProviderHealthView> reading) -> rt::Task<void> {
    const health::ProviderHealthView view = co_await std::move(reading);
    auto listing = rowsOf(kv);
    const json::value rows = co_await std::move(listing);
    expectStep(label, json::value_from(view), rows);
  };

#define RECORD_STEP(label, call) \
  { \
    const rt::Promise<void> recording_ = (call); \
    co_await recordStep(label, recording_); \
  }
#define VIEW_STEP(label, call) \
  { \
    auto reading_ = (call); \
    co_await viewStep(label, std::move(reading_)); \
  }
  VIEW_STEP("unknown", health::getCodexProviderHealth("acct", clockAt(T)));
  RECORD_STEP("success", health::recordCodexProviderHealth("acct", "success", 200, clockAt(T + 0.9)));
  VIEW_STEP("view1", health::getCodexProviderHealth("acct", clockAt(T + 1'000)));
  RECORD_STEP("throttled", health::recordCodexProviderHealth("acct", "success", 200, clockAt(T + 30'000)));
  RECORD_STEP("firstRequestId", health::recordCodexProviderHealth("acct", "success", 200, clockAt(T + 31'000), std::string("  req-1 ")));
  RECORD_STEP("laterRequestIdThrottled", health::recordCodexProviderHealth("acct", "success", 200, clockAt(T + 32'000), std::string("req-2")));
  RECORD_STEP("heartbeat", health::recordCodexProviderHealth("acct", "success", 200, clockAt(T + 31'000 + 60'000), std::string("req-3")));
  RECORD_STEP("statusChange", health::recordCodexProviderHealth("acct", "success", 201, clockAt(T + 100'000)));
  RECORD_STEP("authInvalid", health::recordCodexProviderHealth("acct", "auth_invalid", 401, clockAt(T + 110'000)));
  VIEW_STEP("view2", health::getCodexProviderHealth("acct", clockAt(T + 120'000)));
  RECORD_STEP("quota", health::recordCodexProviderHealth("acct", "quota_exhausted", 429, clockAt(T + 130'000)));
  RECORD_STEP("refreshFailed401", health::recordCodexProviderHealth("acct", "refresh_failed", 401, clockAt(T + 140'000)));
  VIEW_STEP("view3", health::getCodexProviderHealth("acct", clockAt(T + 150'000)));
  RECORD_STEP("refreshFailed500", health::recordCodexProviderHealth("acct", "refresh_failed", 500, clockAt(T + 160'000)));
  VIEW_STEP("view4", health::getCodexProviderHealth("acct", clockAt(T + 170'000)));
  RECORD_STEP("refreshSuccess", health::recordCodexProviderHealth("acct", "refresh_success", std::nullopt, clockAt(T + 180'000)));
  RECORD_STEP("upstream", health::recordCodexProviderHealth("acct", "upstream_error", 502, clockAt(T + 190'000), std::string("\x01" "bad")));
  RECORD_STEP("reachable", health::recordCodexProviderHealth("acct", "reachable", 200, clockAt(T + 200'000)));
  VIEW_STEP("view5", health::getCodexProviderHealth("acct", clockAt(T + 200'000 + 30 * 60'000)));
  VIEW_STEP("view5stale", health::getCodexProviderHealth("acct", clockAt(T + 200'001 + 30 * 60'000)));
  const std::function<double()> throwing = []() -> double { throw rt::JsError("clock"); };
  RECORD_STEP("nowThrows", health::recordCodexProviderHealth("acct", "success", 200, throwing));
  RECORD_STEP("openrouter", health::recordOpenRouterProviderHealth("success", 200, clockAt(T + 1)));
  RECORD_STEP("metered", health::recordMeteredProviderHealth("quota_exhausted", 402, clockAt(T + 2)));
  RECORD_STEP("surplus", health::recordSurplusProviderHealth("auth_invalid", 401, clockAt(T + 3)));
  RECORD_STEP("cerebras", health::recordCerebrasProviderHealth("reachable", std::nullopt, clockAt(T + 4)));
  RECORD_STEP("deepseek", health::recordDeepSeekProviderHealth("upstream_error", 500, clockAt(T + 5), std::string("ds-1")));
  RECORD_STEP("lithos", health::recordLithosProviderHealth("refresh_success", 200, clockAt(T + 6)));
  RECORD_STEP("claude", health::recordClaudeCodeProviderHealth("success", 200, clockAt(T + 7)));
  {
    json::array views;
    auto metered = health::getMeteredProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView meteredView = co_await std::move(metered);
    views.push_back(json::value_from(meteredView));
    auto openrouter = health::getOpenRouterProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView openrouterView = co_await std::move(openrouter);
    views.push_back(json::value_from(openrouterView));
    auto surplus = health::getSurplusProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView surplusView = co_await std::move(surplus);
    views.push_back(json::value_from(surplusView));
    auto cerebras = health::getCerebrasProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView cerebrasView = co_await std::move(cerebras);
    views.push_back(json::value_from(cerebrasView));
    auto deepseek = health::getDeepSeekProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView deepseekView = co_await std::move(deepseek);
    views.push_back(json::value_from(deepseekView));
    auto lithos = health::getLithosProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView lithosView = co_await std::move(lithos);
    views.push_back(json::value_from(lithosView));
    auto claude = health::getClaudeCodeProviderHealth(clockAt(T + 10));
    const health::ProviderHealthView claudeView = co_await std::move(claude);
    views.push_back(json::value_from(claudeView));
    auto listing = rowsOf(kv);
    const json::value rows = co_await std::move(listing);
    expectStep("views", views, rows);
  }

  // Identical cold-burst observations share one transition promise; a different event appends.
  health::resetProviderHealthThrottleForTest();
  const rt::Promise<void> a = health::recordCodexProviderHealth("burst", "success", 200, clockAt(T + 300'000));
  const rt::Promise<void> b = health::recordCodexProviderHealth("burst", "success", 200, clockAt(T + 300'001));
  const rt::Promise<void> c = health::recordCodexProviderHealth("burst", "upstream_error", 500, clockAt(T + 300'002));
  const rt::Promise<void> d = health::recordCodexProviderHealth("burst", "upstream_error", 500, clockAt(T + 300'003), std::string("req-x"));
  const rt::Promise<void> e = health::recordCodexProviderHealth("burst", "upstream_error", 500, clockAt(T + 300'004), std::string("req-y"));
  {
    ASSERT(index < steps.size());
    const json::value& step = steps[index++];
    EXPECT_EQ(std::string(step.at("label").as_string()), std::string("burstIdentity"));
    const json::object& identity = step.at("result").as_object();
    EXPECT_EQ(a == b, identity.at("ab").as_bool());
    EXPECT_EQ(b == c, identity.at("bc").as_bool());
    EXPECT_EQ(c == d, identity.at("cd").as_bool());
    EXPECT_EQ(d == e, identity.at("de").as_bool());
  }
  std::vector<rt::Promise<void>> burst = {a, b, c, d, e};
  co_await rt::all(std::move(burst)).wait();
  {
    ASSERT(index < steps.size());
    const json::value& step = steps[index++];
    EXPECT_EQ(std::string(step.at("label").as_string()), std::string("burstRows"));
    auto listing = rowsOf(kv);
    const json::value rows = co_await std::move(listing);
    EXPECT_JSON(rows, step.at("rows"), "burstRows");
  }
  VIEW_STEP("burstView", health::getCodexProviderHealth("burst", clockAt(T + 300'010)));
  EXPECT_EQ(index, steps.size());
  health::resetProviderHealthThrottleForTest();
  kv::setKvForTest(nullptr);
}

ASYNC_TEST("health: a reentrant clock can clear a pending transition") {
  kv::setKvForTest(memoryKv());
  health::resetProviderHealthThrottleForTest();
  const auto cleanup = rt::scopeExit([] {
    health::resetProviderHealthThrottleForTest();
    kv::setKvForTest(nullptr);
  });
  const auto first = health::recordCodexProviderHealth("reentrant", "success", 200, clockAt(1000));
  EXPECT(first.pending());
  const auto second = health::recordCodexProviderHealth("reentrant", "auth_invalid", 401, [] {
    health::resetProviderHealthThrottleForTest();
    return 2000.0;
  });
  co_await first.wait();
  co_await second.wait();

  auto reading = health::getCodexProviderHealth("reentrant", clockAt(2000));
  const auto view = co_await std::move(reading);
  EXPECT_EQ(view.state, std::string("invalid"));
  EXPECT_EQ(view.last_success_at_ms, std::optional<double>(1000));
  EXPECT_EQ(view.last_401_at_ms, std::optional<double>(2000));
}

ASYNC_TEST("health: without a KV a record resolves and every view is unknown") {
  kv::setKvForTest(nullptr);
  health::resetProviderHealthThrottleForTest();
  const json::object& expected = vector("healthNoKv").as_object();
  const rt::Promise<void> recorded = health::recordCodexProviderHealth("acct", "success", 200, clockAt(5));
  co_await recorded.wait();
  auto view = health::getCodexProviderHealth("acct", clockAt(5));
  const health::ProviderHealthView codex = co_await std::move(view);
  EXPECT_JSON(json::value_from(codex), expected.at("view"), "view");
  auto claude = health::getClaudeCodeProviderHealth(clockAt(5));
  const health::ProviderHealthView claudeView = co_await std::move(claude);
  EXPECT_JSON(json::value_from(claudeView), expected.at("claude"), "claude");
  EXPECT(json::value_to<health::ProviderHealthView>(json::value_from(claudeView)) == claudeView);
  health::resetProviderHealthThrottleForTest();
}
