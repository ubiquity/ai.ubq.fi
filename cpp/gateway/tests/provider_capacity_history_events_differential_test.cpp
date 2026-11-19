// Links: tests/provider_capacity_history_events_differential_test.cpp
//        src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp
//        src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
//        src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
//        src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
//        src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp
//        src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
//        src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http.cpp
//        src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//        src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp
//        src/utils.cpp
// Differential cases for src/provider/capacity-history.ts (no bun test imports it directly) and
// src/provider/capacity-events.ts (its bun tests need later waves). The vectors
// (tests/provider_capacity_modules_differential_vectors.inc, shared with provider_capacity_analytics_differential_test.cpp)
// were recorded from the TS modules under bun 1.4.0 with an in-memory Deno KV whose operations complete one macrotask
// later; the KV scenarios repeat the generator's steps one for one.
//
// The default trigger links the real capacity sampler. An isolated KV opener observes its one-microtask
// entry, and a refusing lease CAS records its pinned sample times without dispatching provider traffic.
#include "tests/provider_capacity_differential_support.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/capacity_history.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace uos;
using namespace uos::tests::capacity_differential;
namespace history = provider::capacity_history;
namespace events = provider::capacity_events;
using contract::ProviderCapacityHistoryPoint;
using contract::ProviderCapacitySnapshot;
using contract::StoredRateLimitObservation;

namespace {
  std::vector<double>& sampledNows() {
    static std::vector<double> calls;
    return calls;
  }
}


namespace {
  class ObservedSamplerKv final : private uos::tests::capacity_differential::OwnedInnerKv, public rt::ForwardingKv {
  public:
    ObservedSamplerKv() : OwnedInnerKv(), rt::ForwardingKv(*owned) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      if (key == contract::PROVIDER_CAPACITY_LEASE_KEY)
        return rt::fulfilledTask(rt::KvEntryMaybe<>{.key = std::move(key), .value = rt::KvValue(nullptr), .versionstamp = std::nullopt});
      return rt::ForwardingKv::get(std::move(key), options);
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      rt::GenericAtomicOperation::CommitFunction commit = [this](std::vector<rt::AtomicCheck>, std::vector<rt::KvMutation> mutations) {
        for (const auto& mutation : mutations) {
          if (mutation.type != rt::KvMutationType::set || mutation.key != contract::PROVIDER_CAPACITY_LEASE_KEY) continue;
          const json::value lease = mutation.value.toJson();
          sampledNows().push_back(*rt::getNumber(lease, "lease_until_ms") - contract::PROVIDER_CAPACITY_LEASE_MS);
          if (onProposedLease) onProposedLease();
        }
        // A concurrent lease owner wins: real sampler entry and plan, no provider dispatch.
        return rt::fulfilledTask(rt::KvCommitResult{.ok = false, .versionstamp = ""});
      };
      return std::make_unique<rt::GenericAtomicOperation>(std::move(commit));
    }
    std::function<void()> onProposedLease;
  };

  const json::object& historySection() { return vectors().at("history").as_object(); }
  const json::object& eventsSection() { return vectors().at("events").as_object(); }

  std::function<double()> clockAt(double value) {
    return [value] { return value; };
  }

  // A store whose `set` records `{ key, expireIn }` and throws for a key whose last part names the secondary window
  // (the generator's override in the mergeHistoricalRateLimitResetEvents scenario).
  class RecordingSetKv final : public rt::ForwardingKv {
  public:
    explicit RecordingSetKv(std::shared_ptr<rt::Kv> inner_) : rt::ForwardingKv(*inner_), owned(std::move(inner_)) {}
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      json::object call;
      call["key"] = keyJson(key);
      if (options.expireIn) call["expireIn"] = rt::makeNumber(*options.expireIn);
      sets.push_back(rt::jsonStringify(call));
      if (!key.empty() && key.back().isString() && key.back().string().find("secondary") != std::string::npos) throw rt::JsError("set refused");
      auto task = rt::ForwardingKv::set(std::move(key), std::move(value), options);
      rt::KvCommitResult result = co_await std::move(task);
      co_return result;
    }
    std::shared_ptr<rt::Kv> owned;
    std::vector<std::string> sets;
  };

  json::value eventsJson(const auto& list) { return json::value_from(list); }
}

TEST("capacity history: snapshot points, merges and transition detection") {
  const json::object& section = historySection();
  for (const json::value& row : section.at("pointForSnapshot").as_array()) {
    EXPECT_JSON(json::value_from(history::historyPointForSnapshot(recordOf<ProviderCapacitySnapshot>(row.at("snapshot")))), row.at("output"), "historyPointForSnapshot");
  }
  for (const json::value& row : section.at("merge").as_array()) {
    EXPECT_JSON(plain(history::mergeHistoryPoints(recordsOf<ProviderCapacityHistoryPoint>(row.at("points")), recordOf<ProviderCapacityHistoryPoint>(row.at("addition")))),
                row.at("output"), "mergeHistoryPoints");
  }
  for (const json::value& row : section.at("transitions").as_array()) {
    const std::optional<ProviderCapacityHistoryPoint> previous = optionalRecordOf<ProviderCapacityHistoryPoint>(row.at("previous"));
    const ProviderCapacityHistoryPoint current = recordOf<ProviderCapacityHistoryPoint>(row.at("current"));
    EXPECT_EQ(history::codexResetTransitionObserved(previous, current), row.at("reset").as_bool());
    EXPECT_EQ(history::codexAvailabilityTransitionObserved(previous, current), row.at("availability").as_bool());
  }
  for (const json::value& row : section.at("storedRateLimit").as_array()) {
    EXPECT_JSON(plain(history::readStoredRateLimitObservation(storedOf(row.at("input")))), row.at("output"), rt::jsonStringify(row.at("input")).substr(0, 160));
  }
  for (const json::value& row : section.at("observed").as_array()) {
    const std::optional<ProviderCapacitySnapshot> previous = optionalRecordOf<ProviderCapacitySnapshot>(row.at("previous"));
    const ProviderCapacitySnapshot current = recordOf<ProviderCapacitySnapshot>(row.at("current"));
    const json::value actual = row.at("last").is_null()
                                 ? plain(history::observedRateLimitResetEvents(previous, current))
                                 : plain(history::observedRateLimitResetEvents(previous, current, recordsOf<StoredRateLimitObservation>(row.at("last"))));
    EXPECT_JSON(actual, row.at("output"), "observedRateLimitResetEvents");
  }
  for (const json::value& row : section.at("recovery").as_array()) {
    const std::vector<double> slots = history::recoverySlotsForSnapshot(recordOf<ProviderCapacitySnapshot>(row.at("snapshot")),
                                                                        optionalRecordOf<ProviderCapacitySnapshot>(row.at("previous")),
                                                                        recordsOf<StoredRateLimitObservation>(row.at("last")));
    EXPECT_JSON(json::value_from(slots), rt::canonicalNumbers(row.at("output")), "recoverySlotsForSnapshot");
  }
  for (const json::value& row : section.at("lastAvailable").as_array()) {
    json::array entries;
    for (const history::LastAvailableCapacityEntry& entry : history::lastAvailableCapacityEntries(recordOf<ProviderCapacitySnapshot>(row.at("snapshot")))) {
      json::object item;
      item["slot"] = rt::makeNumber(entry.slot);
      item["value"] = json::value_from(entry.value);
      entries.push_back(std::move(item));
    }
    EXPECT_JSON(entries, row.at("output"), "lastAvailableCapacityEntries");
  }
  const json::object& keys = section.at("keys").as_object();
  EXPECT_JSON(keyJson(history::providerCapacityHistoryKey(1'800'000'000'000)), keys.at("history"), "providerCapacityHistoryKey");
  EXPECT_JSON(keyJson(history::providerCapacityHistoryTransitionKey(1'800'000'000'000, 1'800'000'000'005)), keys.at("transition"), "providerCapacityHistoryTransitionKey");
  // sameCodexAccountCohort: equal non-null cohorts only.
  const ProviderCapacitySnapshot previousSnapshot = recordOf<ProviderCapacitySnapshot>(section.at("observed").as_array()[0].at("previous"));
  contract::ProviderCapacityCodexSource left = std::get<contract::ProviderCapacityCodexSource>(previousSnapshot.sources[0]);
  contract::ProviderCapacityCodexSource right = left;
  EXPECT(history::sameCodexAccountCohort(left, right));
  right.account_cohort_id = std::string(64, 'b');
  EXPECT(!history::sameCodexAccountCohort(left, right));
  left.account_cohort_id = std::nullopt;
  right.account_cohort_id = std::nullopt;
  EXPECT(!history::sameCodexAccountCohort(left, right));
}

ASYNC_TEST("capacity history: stored history reads, historical reset markers and recovered observations over the KV") {
  const json::object& scenario = historySection().at("kv").as_object();
  std::shared_ptr<rt::Kv> memory = memoryKv();
  for (const json::value& row : scenario.at("rows").as_array()) {
    const rt::KvValue value(row.at("value"));
    auto writing = memory->set(keyOf(row.at("key")), value);
    co_await std::move(writing);
  }
  const double nowMs = rt::toNumber(scenario.at("nowMs"));
  auto reading = history::readCapacityHistory(memory, nowMs);
  const std::vector<ProviderCapacityHistoryPoint> readHistory = co_await std::move(reading);
  EXPECT_JSON(plain(readHistory), scenario.at("readHistory"), "readCapacityHistory");
  auto failing = std::make_shared<FailingListKv>(false);
  auto failedReading = history::readCapacityHistory(failing, nowMs);
  const std::vector<ProviderCapacityHistoryPoint> readHistoryFailed = co_await std::move(failedReading);
  EXPECT(readHistoryFailed.empty());
  EXPECT(scenario.at("readHistoryFailed").as_array().empty());

  auto recording = std::make_shared<RecordingSetKv>(memory);
  const std::vector<events::ProviderCapacityRateLimitResetEvent> existing = recordsOf<events::ProviderCapacityRateLimitResetEvent>(scenario.at("existing"));
  auto merging = history::mergeHistoricalRateLimitResetEvents(recording, readHistory, existing);
  const std::vector<events::ProviderCapacityRateLimitResetEvent> merged = co_await std::move(merging);
  EXPECT_JSON(eventsJson(merged), scenario.at("merged"), "mergeHistoricalRateLimitResetEvents");
  json::array sets;
  for (const std::string& call : recording->sets) sets.emplace_back(json::string(call));
  EXPECT_JSON(sets, scenario.at("sets"), "sets");
  auto listing = rowsOf(memory, rt::KvKey{"uos_ai", "provider_capacity", "v1", "rate_limit_reset_event"}, {});
  const json::value eventRows = co_await std::move(listing);
  EXPECT_JSON(eventRows, scenario.at("eventRows"), "eventRows");

  const ProviderCapacitySnapshot recoverSnapshot = recordOf<ProviderCapacitySnapshot>(scenario.at("recoverSnapshot"));
  std::vector<double> bothSlots = {1, 2};
  auto recovering = history::recoverRateLimitObservations(memory, recoverSnapshot, bothSlots);
  const std::vector<StoredRateLimitObservation> recovered = co_await std::move(recovering);
  EXPECT_JSON(plain(recovered), scenario.at("recovered"), "recovered");
  std::vector<double> noSlots;
  auto recoveringNone = history::recoverRateLimitObservations(memory, recoverSnapshot, noSlots);
  const std::vector<StoredRateLimitObservation> recoveredNone = co_await std::move(recoveringNone);
  EXPECT_JSON(plain(recoveredNone), scenario.at("recoveredNone"), "recoveredNone");
  ProviderCapacitySnapshot noCohort = recoverSnapshot;
  std::get<contract::ProviderCapacityCodexSource>(noCohort.sources[0]).account_cohort_id = std::nullopt;
  std::vector<double> firstSlot = {1};
  auto recoveringNoCohort = history::recoverRateLimitObservations(memory, noCohort, firstSlot);
  const std::vector<StoredRateLimitObservation> recoveredNoCohort = co_await std::move(recoveringNoCohort);
  EXPECT(recoveredNoCohort.empty());
  EXPECT(scenario.at("recoveredNoCohort").as_array().empty());
}

TEST("capacity events: stored events validate as the TS guards do") {
  const json::object& parse = eventsSection().at("parse").as_object();
  for (const json::value& row : parse.at("reset").as_array())
    EXPECT_JSON(plain(events::parseProviderCapacityResetEvent(storedOf(row.at("input")))), row.at("output"), rt::jsonStringify(row.at("input")).substr(0, 120));
  for (const json::value& row : parse.at("rateLimit").as_array())
    EXPECT_JSON(plain(events::parseProviderCapacityRateLimitResetEvent(storedOf(row.at("input")))), row.at("output"), rt::jsonStringify(row.at("input")));
  for (const json::value& row : parse.at("downtime").as_array())
    EXPECT_JSON(plain(events::parseProviderCapacityDowntimeEvent(storedOf(row.at("input")))), row.at("output"), rt::jsonStringify(row.at("input")));
  const json::object& keys = eventsSection().at("keys").as_object();
  EXPECT_JSON(keyJson(events::providerCapacityResetEventKey("e")), keys.at("reset"), "providerCapacityResetEventKey");
  EXPECT_JSON(keyJson(events::providerCapacityRateLimitResetEventKey("e")), keys.at("rateLimit"), "providerCapacityRateLimitResetEventKey");
  EXPECT_JSON(keyJson(events::providerCapacityDowntimeEventKey("e")), keys.at("downtime"), "providerCapacityDowntimeEventKey");
}

ASYNC_TEST("capacity events: record, list and backfill chart events over the KV") {
  constexpr double T = 1'800'000'000'000;
  constexpr double BUCKET = 15 * 60'000;
  constexpr double DAY = 24 * 3'600'000;
  const json::array& steps = eventsSection().at("steps").as_array();
  std::size_t index = 0;
  const auto expectStep = [&](std::string_view label, const json::value& result) {
    ASSERT(index < steps.size());
    const json::value& step = steps[index++];
    EXPECT_EQ(std::string(step.at("label").as_string()), std::string(label));
    EXPECT_JSON(result, step.at("result"), std::string(label));
  };
  std::shared_ptr<rt::Kv> kv = memoryKv();
  const std::optional<std::shared_ptr<rt::Kv>> override = kv;
  const std::optional<std::shared_ptr<rt::Kv>> nullKv = std::shared_ptr<rt::Kv>();
  const auto reset = [](std::string id, double slot, double observed) { return events::ProviderCapacityResetEvent{.v = 1, .event_id = std::move(id), .slot = slot, .observed_at_ms = observed}; };
  const auto setRaw = [kv](rt::KvKey key, json::value value) -> rt::Task<void> {
    const rt::KvValue stored(value);
    auto writing = kv->set(std::move(key), stored);
    co_await std::move(writing);
  };
#define RECORD_STEP(label, call) \
  { \
    auto recording_ = (call); \
    const bool result_ = co_await std::move(recording_); \
    expectStep(label, json::value(result_)); \
  }
  RECORD_STEP("recordReset", events::recordProviderCapacityResetEvent(reset("reset-1", 1, T), override));
  RECORD_STEP("recordResetSame", events::recordProviderCapacityResetEvent(reset("reset-1", 1, T), override));
  RECORD_STEP("recordResetConflict", events::recordProviderCapacityResetEvent(reset("reset-1", 2, T), override));
  RECORD_STEP("recordResetNullKv", events::recordProviderCapacityResetEvent(reset("reset-2", 1, T), nullKv));
  {
    json::object garbage;
    garbage["v"] = 9;
    auto writing = rt::async([setRaw, garbage] { return setRaw(events::providerCapacityResetEventKey("garbage"), garbage); });
    co_await std::move(writing);
  }
  RECORD_STEP("recordResetOverGarbage", events::recordProviderCapacityResetEvent(reset("garbage", 2, T + 1), override));
  const auto downtime = [](std::string kind, std::optional<double> status, double observed) {
    return events::ProviderCapacityDowntimeEventInput{.failure_kind = std::move(kind), .status = status, .observed_at_ms = observed};
  };
  RECORD_STEP("downtime", events::recordProviderCapacityDowntimeEvent(downtime("upstream_error", 503, T + 61'000), override));
  RECORD_STEP("downtimeSameBucket", events::recordProviderCapacityDowntimeEvent(downtime("unreachable", std::nullopt, T + 62'000), override));
  RECORD_STEP("downtimeBadStatus", events::recordProviderCapacityDowntimeEvent(downtime("upstream_error", 404, T), override));
  RECORD_STEP("downtimeBadTime", events::recordProviderCapacityDowntimeEvent(downtime("upstream_error", 503, -1), override));
  RECORD_STEP("downtimeNextBucket", events::recordProviderCapacityDowntimeEvent(downtime("unreachable", std::nullopt, T + BUCKET), override));
  {
    json::object broken;
    broken["broken"] = true;
    auto writing = rt::async([setRaw, broken] { return setRaw(events::providerCapacityDowntimeEventKey("openai-9"), broken); });
    co_await std::move(writing);
  }
  RECORD_STEP("downtimeOverGarbage", events::recordProviderCapacityDowntimeEvent(downtime("unreachable", std::nullopt, 9), override));
#undef RECORD_STEP

  const auto rateLimitEvent = [](std::string id, double observed) {
    json::object event;
    event["v"] = 1;
    event["event_id"] = json::string(id);
    event["provider"] = "openai";
    event["slot"] = 1;
    event["window"] = "primary";
    event["observed_at_ms"] = rt::makeNumber(observed);
    event["previous_sampled_at_ms"] = 1;
    event["previous_reset_at_ms"] = 6;
    event["reset_at_ms"] = 99;
    event["previous_used_percent"] = 90;
    event["current_used_percent"] = 20;
    event["capacity_gain_percentage_points"] = 70;
    return event;
  };
  const std::vector<std::pair<std::string, double>> rateLimitRows = {{"rl-b", T + 10}, {"rl-a", T + 10}, {"rl-c", T - 8 * DAY}, {"rl-d", T + 999'999'999}};
  for (const auto& [id, observed] : rateLimitRows) {
    const json::object event = rateLimitEvent(id, observed);
    auto writing = rt::async([setRaw, id, event] { return setRaw(events::providerCapacityRateLimitResetEventKey(id), event); });
    co_await std::move(writing);
  }
  {
    json::object bad;
    bad["v"] = 1;
    auto writing = rt::async([setRaw, bad] { return setRaw(events::providerCapacityRateLimitResetEventKey("rl-bad"), bad); });
    co_await std::move(writing);
  }
  const auto listOptions = [](std::optional<std::shared_ptr<rt::Kv>> store, std::function<double()> now) {
    events::ListProviderCapacityEventsOptions options;
    options.kv = std::move(store);
    options.now = std::move(now);
    return options;
  };
  {
    auto listing = events::listProviderCapacityRateLimitResetEvents(listOptions(override, clockAt(T + 100.7)));
    const auto listed = co_await std::move(listing);
    expectStep("listRateLimit", eventsJson(listed));
    auto listingNull = events::listProviderCapacityRateLimitResetEvents(listOptions(nullKv, {}));
    const auto listedNull = co_await std::move(listingNull);
    expectStep("listRateLimitNullKv", eventsJson(listedNull));
    auto listingDowntime = events::listProviderCapacityDowntimeEvents(listOptions(override, clockAt(T + BUCKET + 5)));
    const auto listedDowntime = co_await std::move(listingDowntime);
    expectStep("listDowntime", eventsJson(listedDowntime));
    auto listingBadNow = events::listProviderCapacityDowntimeEvents(listOptions(override, clockAt(-5)));
    const auto listedBadNow = co_await std::move(listingBadNow);
    expectStep("listDowntimeBadNow", json::value(!listedBadNow.empty()));
  }

  // Shadow decisions and verified redemptions the reset listing backfills from.
  const auto fence = [](double slot, std::string hash, std::string generation) {
    json::object item;
    item["slot"] = rt::makeNumber(slot);
    item["account_id_hash"] = json::string(hash);
    item["quota_generation"] = json::string(generation);
    item["routing_generation"] = 1;
    item["quota_reset_at_ms"] = rt::makeNumber(T);
    return item;
  };
  const auto decision = [](std::string episode, json::array fences) {
    json::object record;
    record["v"] = 1;
    record["episode_hash"] = json::string(episode);
    record["created_at_ms"] = rt::makeNumber(T - 10);
    record["expires_at_ms"] = rt::makeNumber(T + 10);
    record["decision_reason"] = "all_exhausted";
    record["selected_account_id_hash"] = nullptr;
    record["selected_credit_id_hash"] = nullptr;
    record["selected_credit_expires_at_ms"] = nullptr;
    record["fences"] = std::move(fences);
    return record;
  };
  const auto redemption = [](std::string key, std::string hash, std::string generation, std::string state, std::optional<double> verified) {
    json::object record;
    record["v"] = 1;
    record["account_id_hash"] = json::string(hash);
    record["credential_version"] = "cv";
    record["quota_generation"] = json::string(generation);
    record["routing_generation"] = 1;
    record["idempotency_key_hash"] = json::string(key);
    record["state"] = json::string(state);
    record["owner_token"] = "owner";
    record["fence"] = 1;
    record["lease_expires_at_ms"] = rt::makeNumber(T);
    record["provider_receipt_id"] = state == "verified" ? json::value(json::string("receipt")) : json::value(nullptr);
    record["created_at_ms"] = rt::makeNumber(T - 100);
    record["updated_at_ms"] = rt::makeNumber(verified.value_or(T - 50));
    record["submitted_at_ms"] = state == "claimed" ? json::value(nullptr) : rt::makeNumber(T - 90);
    record["verified_at_ms"] = verified ? rt::makeNumber(*verified) : json::value(nullptr);
    record["last_error_code"] = nullptr;
    return record;
  };
  {
    json::array first;
    first.push_back(fence(1, "h1", "g1"));
    first.push_back(fence(2, "h2", "g2"));
    first.push_back(fence(3, "h3", "g3"));
    json::array third;
    third.push_back(fence(2, "h1", "g1"));
    json::object broken;
    broken["v"] = 1;
    broken["broken"] = true;
    std::vector<std::pair<rt::KvKey, json::value>> rows;
    const rt::KvKey decisions = codex::banked_reset::CODEX_RESET_SHADOW_DECISION_KV_PREFIX;
    const rt::KvKey redemptions = codex::banked_reset::CODEX_RESET_REDEMPTION_KV_PREFIX;
    rows.emplace_back(rt::KvKey{decisions, std::string("ep1")}, decision("ep1", first));
    rows.emplace_back(rt::KvKey{decisions, std::string("ep2")}, broken);
    rows.emplace_back(rt::KvKey{decisions, std::string("ep3")}, decision("ep3", third));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k1")}, redemption("idem-1", "h1", "g1", "verified", T + 5));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k2")}, redemption("idem-2", "h2", "g2", "verified", T + 6));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k3")}, redemption("idem-3", "h3", "g3", "verified", T + 7));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k4")}, redemption("idem-4", "h1", "g1", "submitted", std::nullopt));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k5")}, redemption("idem-5", "h9", "g9", "verified", T + 8));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k6")}, redemption("idem-6", "h2", "g2", "verified", T + 99'999'999));
    rows.emplace_back(rt::KvKey{redemptions, std::string("k7")}, redemption("reset-1", "h2", "g2", "verified", T + 9));
    json::object old;
    old["v"] = 1;
    old["event_id"] = "old";
    old["slot"] = 1;
    old["observed_at_ms"] = rt::makeNumber(T - 8 * DAY);
    rows.emplace_back(events::providerCapacityResetEventKey("old"), old);
    for (auto& [key, value] : rows) {
      const rt::KvValue stored(value);
      auto writing = kv->set(key, stored);
      co_await std::move(writing);
    }
  }
  {
    auto listing = events::listProviderCapacityResetEvents(listOptions(override, clockAt(T + 1'000)));
    const auto listed = co_await std::move(listing);
    expectStep("listReset", eventsJson(listed));
    auto again = events::listProviderCapacityResetEvents(listOptions(override, clockAt(T + 1'000)));
    const auto listedAgain = co_await std::move(again);
    expectStep("listResetAgain", eventsJson(listedAgain));
    auto listingNull = events::listProviderCapacityResetEvents(listOptions(nullKv, {}));
    const auto listedNull = co_await std::move(listingNull);
    expectStep("listResetNullKv", eventsJson(listedNull));
  }
  EXPECT_EQ(index, steps.size());
  const auto onlyCapacity = [](const rt::KvKey& key) { return key.size() > 1 && key[1].isString() && key[1].string() == "provider_capacity"; };
  auto listing = rowsOf(kv, rt::KvKey{"uos_ai"}, onlyCapacity);
  const json::value rows = co_await std::move(listing);
  EXPECT_JSON(rows, eventsSection().at("rows"), "rows");
}

ASYNC_TEST("capacity events: the sample trigger seam, its default debounce and its one-microtask sampler call") {
  json::array seen;
  events::setProviderCapacitySampleTriggerForTest([&seen](const events::ProviderCapacitySampleTriggerOptions& options) {
    if (options.now) seen.push_back(rt::makeNumber(options.now()));
    else seen.emplace_back("no-now");
  });
  events::triggerProviderCapacitySample();
  events::ProviderCapacitySampleTriggerOptions withNow;
  withNow.now = clockAt(42);
  events::triggerProviderCapacitySample(withNow);
  events::setProviderCapacitySampleTriggerForTest({});
  EXPECT_JSON(seen, eventsSection().at("triggerSeen"), "triggerSeen");

  // The default trigger: the first observation in a 15-minute bucket samples (one microtask later), later ones in the
  // same bucket do not, and the sampler receives `now: () => nowMs`.
  rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
  const std::string savedSeed = config::config.codexAuthJsonB64;
  config::config.codexAuthJsonB64.clear();
  const auto restore = rt::scopeExit([&savedSeed] { kv::setKvForTest(nullptr); config::config.codexAuthJsonB64 = savedSeed; });
  int networkCalls = 0;
  rt::GlobalFetchOverride noNetwork(rt::Fetch([&networkCalls](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    ++networkCalls;
    throw rt::JsError("the refused sampling lease must not dispatch a provider request");
  }));
  auto store = std::make_shared<ObservedSamplerKv>();
  std::shared_ptr<rt::Kv> handle = store;
  int samplerEntries = 0;
  kv::KvOpener opener = [&samplerEntries, handle] {
    ++samplerEntries;
    return rt::fulfilledTask(handle);
  };
  kv::setKvForTest(nullptr, opener);
  auto firstLease = rt::withResolvers<void>();
  store->onProposedLease = [resolve = firstLease.resolve] { resolve(); };
  sampledNows().clear();
  constexpr double T = 1'800'000'000'000;
  events::ProviderCapacitySampleTriggerOptions first;
  first.now = clockAt(T + 5);
  events::triggerProviderCapacitySample(first);
  EXPECT_EQ(samplerEntries, 0);
  EXPECT(sampledNows().empty());
  co_await rt::microtask();
  EXPECT_EQ(samplerEntries, 1);
  co_await firstLease.promise.wait();
  EXPECT_EQ(sampledNows(), (std::vector<double>{T + 5}));
  auto nextLease = rt::withResolvers<void>();
  store->onProposedLease = [resolve = nextLease.resolve] { resolve(); };
  kv::setKvForTest(nullptr, opener);
  events::ProviderCapacitySampleTriggerOptions sameBucket;
  sameBucket.now = clockAt(T + 15 * 60'000 - 1);
  events::triggerProviderCapacitySample(sameBucket);
  events::ProviderCapacitySampleTriggerOptions earlier;
  earlier.now = clockAt(T - 1);
  events::triggerProviderCapacitySample(earlier);
  events::ProviderCapacitySampleTriggerOptions nextBucket;
  nextBucket.now = clockAt(T + 15 * 60'000);
  events::triggerProviderCapacitySample(nextBucket);
  EXPECT_EQ(samplerEntries, 1);
  co_await rt::microtask();
  EXPECT_EQ(samplerEntries, 2);
  co_await nextLease.promise.wait();
  EXPECT_EQ(sampledNows(), (std::vector<double>{T + 5, T + 15 * 60'000}));
  EXPECT_EQ(networkCalls, 0);
}
