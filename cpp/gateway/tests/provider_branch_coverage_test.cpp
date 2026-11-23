// Links: tests/provider_branch_coverage_test.cpp src/config.cpp src/provider/capacity_contract.cpp src/provider/capacity_parse.cpp src/release.cpp src/utils.cpp
// Port of tests/provider-branch-coverage.test.ts (every case).
//
// Branch coverage for the provider capacity parser.
//
// Every guard in this module has a rejection path and an acceptance path, so
// each case below feeds the malformed value and the well-formed value and
// asserts the parsed result (or null) the gateway stores and routes on.
//
// The TS fixtures `{ ...codexSource(), key: undefined }` set a member to undefined; the C++ fixtures delete it instead,
// which every guard here reads identically (`=== undefined`). The `as unknown as Kv` stubs that only implement `get` are a
// ForwardingKv over an empty MemoryKv overriding `get`.
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace parse = provider::capacity_parse;
namespace contract = provider::capacity_contract;
using contract::ProviderCapacityCodexSource;
using contract::ProviderCapacityMeteredSource;
using contract::ProviderCapacitySource;
using contract::ProviderCapacityWindow;

namespace {
  const std::string SHA256(64, 'a');

  // The marker for a `key: undefined` override.
  const json::value& undefinedMember() {
    static const json::value marker = json::object{{"$undefined", true}};
    return marker;
  }

  json::object withOverrides(json::object base, const json::object& overrides) {
    for (const auto& [key, value] : overrides) {
      if (value == undefinedMember()) rt::objectDelete(base, key);
      else rt::objectSet(base, key, value);
    }
    return base;
  }

  json::value number(double value) { return rt::makeNumber(value); }

  json::object windowJson(json::value limitWindowSeconds, json::value usedPercent, json::value resetAtMs) {
    json::object window;
    window["limit_window_seconds"] = std::move(limitWindowSeconds);
    window["used_percent"] = std::move(usedPercent);
    window["reset_at_ms"] = std::move(resetAtMs);
    return window;
  }

  /** Narrow the snapshot's source union so each provider's fields stay typed. */
  std::vector<ProviderCapacityCodexSource> codexSources(const std::array<ProviderCapacitySource, 3>& sources) {
    std::vector<ProviderCapacityCodexSource> out;
    for (const ProviderCapacitySource& source : sources) {
      if (const auto* codex = std::get_if<ProviderCapacityCodexSource>(&source)) out.push_back(*codex);
    }
    return out;
  }
  std::vector<ProviderCapacityMeteredSource> meteredSources(const std::array<ProviderCapacitySource, 3>& sources) {
    std::vector<ProviderCapacityMeteredSource> out;
    for (const ProviderCapacitySource& source : sources) {
      if (const auto* metered = std::get_if<ProviderCapacityMeteredSource>(&source)) out.push_back(*metered);
    }
    return out;
  }

  json::object codexSource(const json::object& overrides = {}) {
    json::object base;
    base["source"] = "codex";
    base["label"] = "codex@example.test";
    base["slot"] = 1;
    base["account_cohort_id"] = json::string(SHA256);
    base["state"] = "available";
    base["source_observed_at_ms"] = number(1'700'000'000'000);
    base["snapshot_at_ms"] = number(1'700'000'000'000);
    base["failure_kind"] = nullptr;
    base["failure_status"] = nullptr;
    json::object windows;
    windows["primary"] = windowJson(number(18'000), number(12.5), number(1'700'000'600'000));
    windows["secondary"] = nullptr;
    base["windows"] = std::move(windows);
    base["additional_rate_limits"] = json::array();
    return withOverrides(std::move(base), overrides);
  }

  json::object meteredWallet() {
    return rt::jsonParse(R"({"balance_credits":5,"baseline_credits":10,"refill_cycle_remaining_percent":50,"refill_cycle_used_percent":50,)"
                         R"("unlimited_quota":false,"total_available":5,"total_granted":10,"total_used":5,"cycle_started_at_ms":1700000000000,)"
                         R"("last_credit_at_ms":1700000000000,"confidence":"provisional","cache_state":"fresh","reset_at_ms":null})")
      .as_object();
  }

  json::object meteredSource(const json::object& overrides = {}) {
    json::object base;
    base["source"] = "metered";
    base["label"] = "Metered fallback";
    base["state"] = "available";
    base["source_observed_at_ms"] = number(1'700'000'000'000);
    base["snapshot_at_ms"] = number(1'700'000'000'000);
    base["wallet"] = meteredWallet();
    return withOverrides(std::move(base), overrides);
  }

  json::object storedSnapshot(const json::object& overrides = {}) {
    json::object base;
    base["snapshot_at_ms"] = number(1'700'000'000'000);
    base["stale_after_ms"] = 900'000;
    json::array sources;
    sources.emplace_back(codexSource({{"slot", 1}}));
    sources.emplace_back(codexSource({{"slot", 2}, {"account_cohort_id", nullptr}}));
    sources.emplace_back(meteredSource());
    base["sources"] = std::move(sources);
    return withOverrides(std::move(base), overrides);
  }

  json::array arrayOf(std::initializer_list<json::value> items) {
    json::array out;
    for (const json::value& item : items) out.push_back(item);
    return out;
  }

  rt::KvValue stored(const json::value& value) { return rt::KvValue(value); }

  json::value windowValue(const std::optional<ProviderCapacityWindow>& window) { return window ? json::value_from(*window) : json::value(nullptr); }

  json::value limitsJson(const std::vector<contract::ProviderCapacityAdditionalRateLimit>& limits) { return json::value_from(limits); }

  void expectJson(const json::value& actual, const json::value& expected, const char* label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, std::string(label) + "\n      actual:   " + json::serialize(actual) + "\n      expected: " + json::serialize(expected), false);
  }
#define EXPECT_JSON(actual, expected) expectJson((actual), (expected), #actual, __FILE__, __LINE__)

  // Owns the store a ForwardingKv wraps; a base so it is constructed before the ForwardingKv base.
  struct EmptyKvOwner {
    std::shared_ptr<rt::Kv> empty = std::make_shared<rt::MemoryKv>([] { return 0.0; });
  };

  // `{ get: (key) => ... } as unknown as Kv`
  class GetOnlyKv final : private EmptyKvOwner, public rt::ForwardingKv {
  public:
    explicit GetOnlyKv(std::function<rt::Task<rt::KvEntryMaybe<>>(rt::KvKey)> get_)
        : EmptyKvOwner(), rt::ForwardingKv(*empty), getter(std::move(get_)) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override { return getter(std::move(key)); }

  private:
    std::function<rt::Task<rt::KvEntryMaybe<>>(rt::KvKey)> getter;
  };
}

TEST("capacity parse: an account label falls back to the slot for a blank email") {
  EXPECT_EQ(parse::codexAccountLabel(1), std::string("Codex account 1"));
  EXPECT_EQ(parse::codexAccountLabel(2, std::nullopt), std::string("Codex account 2"));
  EXPECT_EQ(parse::codexAccountLabel(1, std::string("   ")), std::string("Codex account 1"));
  EXPECT_EQ(parse::codexAccountLabel(1, std::string("  codex@example.test  ")), std::string("codex@example.test"));
}

TEST("capacity parse: a clock that answers with an unusable value falls back to the wall clock") {
  const double fixed = parse::safeNow([] { return 1'700'000'000'123.9; });
  EXPECT_EQ(fixed, 1'700'000'000'123.0);
  const double before = rt::dateNow();
  const double fallback = parse::safeNow([] { return -1.0; });
  const double after = rt::dateNow();
  EXPECT(fallback >= before && fallback <= after);
  EXPECT(parse::safeNow([] { return rt::Number::NaN; }) >= before);
  EXPECT_EQ(parse::isSafeTimestamp(rt::KvValue(0)), true);
  EXPECT_EQ(parse::isSafeTimestamp(rt::KvValue(-1)), false);
  EXPECT_EQ(parse::isSafeTimestamp(rt::KvValue(std::string("0"))), false);
  EXPECT_EQ(parse::isSafeTimestamp(rt::KvValue(1.5)), false);
}

TEST("capacity parse: the usage URL and last-available key are built from the contract") {
  const rt::Regex usagePath("\\/backend-api\\/wham\\/usage$");
  EXPECT(usagePath.test(parse::codexUsageUrl()));
  const rt::KvKey key = parse::providerCapacityLastAvailableKey(2);
  ASSERT(!key.empty());
  EXPECT(key.back().isNumber() && key.back().number() == 2);
}

TEST("capacity parse: a codex usage payload requires a rate-limit object and keeps partial windows") {
  const json::value null = nullptr;
  EXPECT(!parse::parseCodexUsage(null).has_value());
  EXPECT(!parse::parseCodexUsage(json::value(json::object())).has_value());
  EXPECT(!parse::parseCodexUsage(rt::jsonParse(R"({"rate_limit":"not-an-object"})")).has_value());

  // Every field missing leaves a null window rather than a bogus one.
  const auto empty = parse::parseCodexUsage(rt::jsonParse(R"({"rate_limit":{}})"));
  ASSERT(empty.has_value());
  EXPECT(*empty == (parse::ParseCodexUsageResult{.primary = std::nullopt, .secondary = std::nullopt, .additional_rate_limits = {}}));

  const auto parsed = parse::parseCodexUsage(rt::jsonParse(R"({
    "rate_limit": {
      "primary_window": { "limit_window_seconds": 18000, "used_percent": 0, "reset_at": 1700000600 },
      "secondary_window": { "limit_window_seconds": 604800, "used_percent": 100, "reset_at": 1800000000 }
    },
    "additional_rate_limits": [
      { "limit_name": " Spark ", "metered_feature": " spark ", "rate_limit": { "primary_window": { "used_percent": 0 } } },
      { "limit_name": "  ", "rate_limit": {} },
      "skip"
    ]
  })"));
  ASSERT(parsed.has_value());
  EXPECT_JSON(windowValue(parsed->primary), rt::jsonParse(R"({ "limit_window_seconds": 18000, "used_percent": 0, "reset_at_ms": 1700000600000 })"));
  EXPECT_JSON(windowValue(parsed->secondary), rt::jsonParse(R"({ "limit_window_seconds": 604800, "used_percent": 100, "reset_at_ms": 1800000000000 })"));
  EXPECT_JSON(limitsJson(parsed->additional_rate_limits), rt::jsonParse(R"([
    {
      "limit_name": "Spark",
      "metered_feature": "spark",
      "windows": { "primary": { "limit_window_seconds": null, "used_percent": 0, "reset_at_ms": null }, "secondary": null }
    }
  ])"));

  // Percent, window and reset bounds each reject their own field.
  json::object primary;
  primary["limit_window_seconds"] = -1;
  primary["used_percent"] = 101;
  primary["reset_at"] = -1;
  json::object secondary;
  secondary["limit_window_seconds"] = 1.5;
  secondary["used_percent"] = rt::makeNumber(rt::Number::NaN);
  secondary["reset_at"] = rt::makeNumber(rt::Number::MAX_SAFE_INTEGER);
  json::object rateLimit;
  rateLimit["primary_window"] = std::move(primary);
  rateLimit["secondary_window"] = std::move(secondary);
  json::object rejectedInput;
  rejectedInput["rate_limit"] = std::move(rateLimit);
  const auto rejected = parse::parseCodexUsage(json::value(std::move(rejectedInput)));
  ASSERT(rejected.has_value());
  EXPECT(*rejected == (parse::ParseCodexUsageResult{.primary = std::nullopt, .secondary = std::nullopt, .additional_rate_limits = {}}));

  // An additional limit with no usable window is dropped entirely.
  const auto noWindow = parse::parseCodexUsage(rt::jsonParse(R"({ "rate_limit": {}, "additional_rate_limits": [{ "limit_name": "x", "rate_limit": {} }] })"));
  ASSERT(noWindow.has_value());
  EXPECT(noWindow->additional_rate_limits.empty());
  // So is one whose name or feature is not a string, or whose rate_limit is not an object.
  const auto mixed = parse::parseCodexUsage(rt::jsonParse(R"({
    "rate_limit": {},
    "additional_rate_limits": [
      { "limit_name": 5, "rate_limit": { "primary_window": { "used_percent": 0 } } },
      { "limit_name": "x", "metered_feature": 5, "rate_limit": { "primary_window": { "used_percent": 0 } } },
      { "limit_name": "x", "rate_limit": "later" }
    ]
  })"));
  ASSERT(mixed.has_value());
  EXPECT_JSON(limitsJson(mixed->additional_rate_limits),
              rt::jsonParse(R"([{ "limit_name": "x", "metered_feature": null, "windows": { "primary": { "limit_window_seconds": null, "used_percent": 0, "reset_at_ms": null }, "secondary": null } }])"));
  const auto notArray = parse::parseCodexUsage(rt::jsonParse(R"({ "rate_limit": {}, "additional_rate_limits": "none" })"));
  ASSERT(notArray.has_value());
  EXPECT(notArray->additional_rate_limits.empty());
}

TEST("capacity parse: an unanchored additional window is filtered out of the routing view") {
  const double snapshotAtMs = 1'700'000'000'000;
  const auto limit = [](std::string name, std::optional<ProviderCapacityWindow> primary, std::optional<ProviderCapacityWindow> secondary) {
    return contract::ProviderCapacityAdditionalRateLimit{
      .limit_name = std::move(name), .metered_feature = std::nullopt, .windows = {.primary = std::move(primary), .secondary = std::move(secondary)}};
  };
  const auto window = [](double seconds, double used, double resetAtMs) {
    return ProviderCapacityWindow{.limit_window_seconds = seconds, .used_percent = used, .reset_at_ms = resetAtMs};
  };
  // A window whose reset is exactly "snapshot plus one full window" was computed
  // from the snapshot rather than anchored to a real quota cycle.
  const auto unanchored = limit("Unanchored", window(18'000, 0, snapshotAtMs + 18'000'000), std::nullopt);
  const auto used = limit("Used", window(18'000, 5, snapshotAtMs + 18'000'000), std::nullopt);
  const auto anchored = limit("Anchored", window(18'000, 0, snapshotAtMs + 60'000), std::nullopt);
  // Both windows are unanchored, so the row has no surviving window at all.
  const auto partial = limit("Partial", window(18'000, 0, snapshotAtMs + 18'000'000), window(18'000, 0, snapshotAtMs + 18'000'000));

  // An unanchored row is dropped while the used and genuinely anchored rows survive.
  EXPECT(parse::additionalRateLimitsForRouting({unanchored, used, anchored}, snapshotAtMs) ==
         (std::vector<contract::ProviderCapacityAdditionalRateLimit>{used, anchored}));
  // A row with no surviving window at all disappears.
  EXPECT(parse::additionalRateLimitsForRouting({partial}, snapshotAtMs).empty());
  // An unanchored secondary window is cleared while the primary stays.
  const auto bothWindows = limit("Both", window(18'000, 0, snapshotAtMs + 60'000), window(18'000, 0, snapshotAtMs + 18'000'000));
  EXPECT(parse::additionalRateLimitsForRouting({bothWindows}, snapshotAtMs) ==
         (std::vector<contract::ProviderCapacityAdditionalRateLimit>{limit("Both", bothWindows.windows.primary, std::nullopt)}));
  // A window whose multiplied lifetime is not a safe integer is not unanchored.
  const auto overflowing = limit("Overflow", window(rt::Number::MAX_SAFE_INTEGER, 0, snapshotAtMs), std::nullopt);
  EXPECT(parse::additionalRateLimitsForRouting({overflowing}, snapshotAtMs) == (std::vector<contract::ProviderCapacityAdditionalRateLimit>{overflowing}));
}

TEST("capacity parse: stored codex sources round-trip only well-formed rows") {
  const double fallback = 1'700'000'000'000;
  const auto storedSource = parse::readStoredCodexSource(stored(codexSource()), fallback);
  ASSERT(storedSource.has_value());
  EXPECT_EQ(storedSource->state, std::string("available"));
  EXPECT(storedSource->failure_kind == std::nullopt);
  EXPECT(storedSource->account_cohort_id == std::optional<std::string>(SHA256));
  EXPECT_JSON(windowValue(storedSource->windows.primary), rt::jsonParse(R"({ "limit_window_seconds": 18000, "used_percent": 12.5, "reset_at_ms": 1700000600000 })"));

  // The optional snapshot timestamp defaults to the caller's fallback.
  const auto withoutSnapshot = parse::readStoredCodexSource(stored(codexSource({{"snapshot_at_ms", undefinedMember()}})), fallback);
  ASSERT(withoutSnapshot.has_value());
  EXPECT_EQ(withoutSnapshot->snapshot_at_ms, fallback);

  // An unavailable source without failure fields defaults to not_configured.
  const auto unavailable = parse::readStoredCodexSource(
    stored(codexSource({{"slot", 2}, {"state", "unavailable"}, {"failure_kind", undefinedMember()}, {"failure_status", undefinedMember()}})), fallback);
  ASSERT(unavailable.has_value());
  EXPECT(unavailable->failure_kind == std::optional<std::string>("not_configured"));
  EXPECT(unavailable->failure_status == std::nullopt);

  // An unavailable source keeps its recorded failure.
  const auto failing = parse::readStoredCodexSource(stored(codexSource({{"state", "unavailable"}, {"failure_kind", "http_error"}, {"failure_status", 503}})), fallback);
  ASSERT(failing.has_value());
  EXPECT(failing->failure_kind == std::optional<std::string>("http_error"));
  EXPECT(failing->failure_status == std::optional<double>(503));

  const std::vector<json::value> rejects = {
    nullptr,
    "codex",
    codexSource({{"source", "metered"}}),
    codexSource({{"slot", 3}}),
    codexSource({{"state", "unknown"}}),
    codexSource({{"source_observed_at_ms", "now"}}),
    codexSource({{"snapshot_at_ms", "now"}}),
    codexSource({{"failure_kind", "made_up"}}),
    codexSource({{"state", "available"}, {"failure_kind", "http_error"}}),
    codexSource({{"state", "available"}, {"failure_status", 500}}),
    codexSource({{"windows", nullptr}}),
  };
  for (const json::value& value : rejects) {
    if (parse::readStoredCodexSource(stored(value), fallback).has_value())
      rt::test::report(__FILE__, __LINE__, "expected rejection of " + rt::jsonStringify(value), false);
  }

  // An out-of-range status is dropped rather than failing an available source.
  const auto outOfRange = parse::readStoredCodexSource(stored(codexSource({{"failure_status", 99}})), fallback);
  ASSERT(outOfRange.has_value());
  EXPECT(outOfRange->failure_status == std::nullopt);
  EXPECT_EQ(outOfRange->state, std::string("available"));
}

TEST("capacity parse: a stored codex source normalizes labels, cohort ids and windows") {
  json::object windows;
  windows["primary"] = windowJson(nullptr, nullptr, nullptr);
  windows["secondary"] = windowJson(60, 1, 5);
  const json::value additional = rt::jsonParse(R"([
    {
      "limit_name": " spark ",
      "metered_feature": "  ",
      "windows": { "primary": { "limit_window_seconds": null, "used_percent": null, "reset_at_ms": null }, "secondary": null }
    },
    { "limit_name": 5, "windows": {} },
    { "limit_name": "   ", "windows": {} },
    { "limit_name": "no-windows", "windows": "later" },
    { "limit_name": "bad-feature", "windows": {}, "metered_feature": 5 },
    "skip",
    null
  ])");
  const auto storedSource = parse::readStoredCodexSource(
    stored(codexSource({{"label", "   "}, {"slot", 2}, {"account_cohort_id", "not-a-digest"}, {"windows", windows}, {"additional_rate_limits", additional}})),
    1'700'000'000'000);

  ASSERT(storedSource.has_value());
  EXPECT_EQ(storedSource->label, std::string("Codex account 2"));
  EXPECT(storedSource->account_cohort_id == std::nullopt);
  EXPECT_JSON(windowValue(storedSource->windows.primary), rt::jsonParse(R"({ "limit_window_seconds": null, "used_percent": null, "reset_at_ms": null })"));
  EXPECT_JSON(windowValue(storedSource->windows.secondary), rt::jsonParse(R"({ "limit_window_seconds": 60, "used_percent": 1, "reset_at_ms": 5 })"));
  EXPECT_JSON(limitsJson(storedSource->additional_rate_limits), rt::jsonParse(R"([
    {
      "limit_name": "spark",
      "metered_feature": null,
      "windows": { "primary": { "limit_window_seconds": null, "used_percent": null, "reset_at_ms": null }, "secondary": null }
    }
  ])"));

  // A stored list that is not an array reads as no additional limits at all.
  const auto noLimits = parse::readStoredCodexSource(stored(codexSource({{"additional_rate_limits", "none"}})), 1);
  ASSERT(noLimits.has_value());
  EXPECT(noLimits->additional_rate_limits.empty());
  EXPECT_JSON(windowValue(storedSource->windows.secondary), rt::jsonParse(R"({ "limit_window_seconds": 60, "used_percent": 1, "reset_at_ms": 5 })"));

  // A stored window with an out-of-range field is rejected.
  json::object badWindows;
  badWindows["primary"] = windowJson(-1, 5, 1);
  badWindows["secondary"] = nullptr;
  const auto rejectedWindow = parse::readStoredCodexSource(stored(codexSource({{"windows", badWindows}})), 1);
  ASSERT(rejectedWindow.has_value());
  EXPECT(rejectedWindow->windows.primary == std::nullopt);
  // A window that is not an object is rejected too.
  json::object shapeWindows;
  shapeWindows["primary"] = "later";
  shapeWindows["secondary"] = nullptr;
  const auto rejectedShape = parse::readStoredCodexSource(stored(codexSource({{"windows", shapeWindows}})), 1);
  ASSERT(rejectedShape.has_value());
  EXPECT(rejectedShape->windows.primary == std::nullopt);
}

TEST("capacity parse: a stored metered source validates its state, wallet and enumerations") {
  const auto storedValue = parse::readStoredSnapshot(stored(storedSnapshot()));
  ASSERT(storedValue.has_value());
  const std::vector<ProviderCapacityMeteredSource> metered = meteredSources(storedValue->sources);
  ASSERT(!metered.empty());
  EXPECT(metered[0].wallet.confidence == std::optional<std::string>("provisional"));
  EXPECT(metered[0].wallet.cache_state == std::optional<std::string>("fresh"));
  EXPECT(metered[0].wallet.unlimited_quota == std::optional<bool>(false));
  EXPECT(codexSources(storedValue->sources)[0].account_cohort_id == std::optional<std::string>(SHA256));
  EXPECT(codexSources(storedValue->sources)[1].account_cohort_id == std::nullopt);

  // Unknown enumeration values degrade to null instead of failing the record.
  json::object guessedWallet = meteredWallet();
  guessedWallet["confidence"] = "guessed";
  guessedWallet["cache_state"] = "warm";
  const auto degraded = parse::readStoredSnapshot(
    stored(storedSnapshot({{"sources", arrayOf({codexSource({{"slot", 1}}), codexSource({{"slot", 2}}), meteredSource({{"wallet", guessedWallet}})})}})));
  ASSERT(degraded.has_value());
  EXPECT(meteredSources(degraded->sources)[0].wallet.confidence == std::nullopt);
  EXPECT(meteredSources(degraded->sources)[0].wallet.cache_state == std::nullopt);

  // Non-numeric wallet fields fall back to null rather than leaking the value.
  json::object coercedWallet = meteredWallet();
  coercedWallet["balance_credits"] = "5";
  coercedWallet["unlimited_quota"] = "yes";
  coercedWallet["cycle_started_at_ms"] = -1;
  const auto coerced = parse::readStoredSnapshot(
    stored(storedSnapshot({{"sources", arrayOf({codexSource({{"slot", 1}}), codexSource({{"slot", 2}}), meteredSource({{"wallet", coercedWallet}})})}})));
  ASSERT(coerced.has_value());
  EXPECT(meteredSources(coerced->sources)[0].wallet.balance_credits == std::nullopt);
  EXPECT(meteredSources(coerced->sources)[0].wallet.unlimited_quota == std::nullopt);
  EXPECT(meteredSources(coerced->sources)[0].wallet.cycle_started_at_ms == std::nullopt);
}

TEST("capacity parse: a stored snapshot requires both codex slots and a metered source") {
  EXPECT(!parse::readStoredSnapshot(stored(nullptr)).has_value());
  EXPECT(!parse::readStoredSnapshot(stored(rt::jsonParse(R"({ "snapshot_at_ms": "now", "sources": [] })"))).has_value());
  EXPECT(!parse::readStoredSnapshot(stored(rt::jsonParse(R"({ "snapshot_at_ms": 1700000000000, "sources": "none" })"))).has_value());
  // A missing metered source invalidates the snapshot.
  EXPECT(!parse::readStoredSnapshot(stored(storedSnapshot({{"sources", arrayOf({codexSource({{"slot", 1}}), codexSource({{"slot", 2}})})}}))).has_value());
  // So does a malformed codex slot.
  EXPECT(!parse::readStoredSnapshot(
            stored(storedSnapshot({{"sources", arrayOf({codexSource({{"slot", 1}, {"state", "unknown"}}), codexSource({{"slot", 2}}), meteredSource()})}})))
            .has_value());
  EXPECT(!parse::readStoredSnapshot(stored(storedSnapshot({{"sources", arrayOf({codexSource({{"slot", 1}}), meteredSource()})}}))).has_value());
}

ASYNC_TEST("capacity parse: snapshot reads swallow a KV failure and decode a healthy row") {
  const json::value storedValue = storedSnapshot();
  auto healthy = std::make_shared<GetOnlyKv>([storedValue](rt::KvKey key) -> rt::Task<rt::KvEntryMaybe<>> {
    EXPECT(key == contract::PROVIDER_CAPACITY_SNAPSHOT_KEY);
    rt::KvEntryMaybe<> entry{.key = key, .value = rt::KvValue(storedValue), .versionstamp = std::string("00000000000000000001")};
    return rt::fulfilledTask(std::move(entry));
  });
  auto healthyRead = parse::readCapacitySnapshot(healthy);
  const auto healthySnapshot = co_await std::move(healthyRead);
  ASSERT(healthySnapshot.has_value());
  EXPECT_EQ(healthySnapshot->sources.size(), std::size_t{3});

  auto failing = std::make_shared<GetOnlyKv>([](rt::KvKey) -> rt::Task<rt::KvEntryMaybe<>> { throw rt::JsError("kv unavailable"); });
  auto failingRead = parse::readCapacitySnapshot(failing);
  const auto failingSnapshot = co_await std::move(failingRead);
  EXPECT(!failingSnapshot.has_value());

  auto empty = std::make_shared<GetOnlyKv>([](rt::KvKey key) -> rt::Task<rt::KvEntryMaybe<>> {
    rt::KvEntryMaybe<> entry{.key = key, .value = rt::KvValue(nullptr), .versionstamp = std::nullopt};
    return rt::fulfilledTask(std::move(entry));
  });
  auto emptyRead = parse::readCapacitySnapshot(empty);
  const auto emptySnapshot = co_await std::move(emptyRead);
  EXPECT(!emptySnapshot.has_value());
}

TEST("capacity parse: a stored history point substitutes the unavailable metered source") {
  json::object point;
  point["bucket_start_at_ms"] = number(1'700'000'000'000);
  point["sampled_at_ms"] = number(1'700'000'060'000);
  point["sources"] = arrayOf({codexSource({{"slot", 1}}), codexSource({{"slot", 2}}), meteredSource()});
  const auto parsed = parse::readStoredHistoryPoint(stored(point));
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->bucket_start_at_ms, 1'700'000'000'000.0);
  EXPECT_EQ(std::get<2>(parsed->sources).state, std::string("available"));

  // A point without a metered source falls back to the unavailable stub.
  const auto withoutMetered = parse::readStoredHistoryPoint(stored(withOverrides(point, {{"sources", arrayOf({codexSource({{"slot", 1}}), codexSource({{"slot", 2}})})}})));
  ASSERT(withoutMetered.has_value());
  EXPECT_EQ(std::get<2>(withoutMetered->sources).state, std::string("unavailable"));
  EXPECT(std::get<2>(withoutMetered->sources).wallet.balance_credits == std::nullopt);

  // A malformed metered row takes the same fallback.
  const auto malformedMetered = parse::readStoredHistoryPoint(
    stored(withOverrides(point, {{"sources", arrayOf({codexSource({{"slot", 1}}), codexSource({{"slot", 2}}), meteredSource({{"wallet", nullptr}})})}})));
  ASSERT(malformedMetered.has_value());
  EXPECT_EQ(std::get<2>(malformedMetered->sources).state, std::string("unavailable"));

  json::object badBucket;
  badBucket["bucket_start_at_ms"] = "now";
  badBucket["sampled_at_ms"] = 1;
  badBucket["sources"] = json::array();
  json::object badSample;
  badSample["bucket_start_at_ms"] = 1;
  badSample["sampled_at_ms"] = -1;
  badSample["sources"] = json::array();
  json::object badSources;
  badSources["bucket_start_at_ms"] = 1;
  badSources["sampled_at_ms"] = 1;
  badSources["sources"] = "none";
  json::object oneSlot;
  oneSlot["bucket_start_at_ms"] = 1;
  oneSlot["sampled_at_ms"] = 1;
  oneSlot["sources"] = arrayOf({codexSource({{"slot", 1}})});
  json::object badSlot;
  badSlot["bucket_start_at_ms"] = 1;
  badSlot["sampled_at_ms"] = 1;
  badSlot["sources"] = arrayOf({codexSource({{"slot", 1}, {"state", "unknown"}}), codexSource({{"slot", 2}})});
  const std::vector<json::value> rejects = {nullptr, badBucket, badSample, badSources, oneSlot, badSlot};
  for (const json::value& value : rejects) {
    if (parse::readStoredHistoryPoint(stored(value)).has_value())
      rt::test::report(__FILE__, __LINE__, "expected rejection of " + rt::jsonStringify(value), false);
  }
}

TEST("capacity parse: unavailable stubs describe an unobserved provider") {
  const ProviderCapacityCodexSource codex = parse::unavailableCodexSource(1, 1'700'000'000'000);
  EXPECT_EQ(codex.state, std::string("unavailable"));
  EXPECT(codex.failure_kind == std::optional<std::string>("not_configured"));
  EXPECT(codex.source_observed_at_ms == std::nullopt);

  const ProviderCapacityCodexSource observed =
    parse::unavailableCodexSource(2, 1'700'000'000'000, "unreachable", 504, true, std::string("offline@example.test"), SHA256);
  EXPECT(observed.failure_kind == std::optional<std::string>("unreachable"));
  EXPECT(observed.failure_status == std::optional<double>(504));
  EXPECT(observed.source_observed_at_ms == std::optional<double>(1'700'000'000'000));
  EXPECT_EQ(observed.label, std::string("offline@example.test"));
  EXPECT(observed.account_cohort_id == std::optional<std::string>(SHA256));

  const ProviderCapacityMeteredSource metered = parse::unavailableMeteredSource(1'700'000'000'000);
  EXPECT_EQ(std::string(ProviderCapacityMeteredSource::source), std::string("metered"));
  EXPECT(metered.wallet.total_available == std::nullopt);
}

TEST("capacity parse: the parsed snapshot type keeps its codex sources addressable") {
  const auto parsed = parse::readStoredSnapshot(stored(storedSnapshot()));
  ASSERT(parsed.has_value());
  std::vector<double> slots;
  for (const ProviderCapacityCodexSource& source : codexSources(parsed->sources)) slots.push_back(source.slot);
  EXPECT_EQ(slots, (std::vector<double>{1, 2}));
}
