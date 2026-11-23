// Links: tests/claude_capacity_differential_test.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/utils.cpp
// Differential cases for src/provider/claude-capacity-rollups.ts and src/provider/claude-capacity-history.ts (their bun
// tests need modules of later waves or a durable KV). The vectors (tests/claude_capacity_differential_vectors.inc) were
// recorded from the TS under bun 1.4.0 with Date.now fixed and an in-memory Deno KV with Deno's key order: the guards and
// readers over stored shapes, rollup and usage-rollup merge sequences with their keys, a 17-event
// recordClaudeCapacityObservation sequence (per-step KV rows, retention flags, rollups, usage rollups, the binding
// mismatch), the three readers over the result, and corrupt-evidence cases. capacity_contract.cpp is not linked: the
// history point and view JSON below is written locally. The TS readers return the stored value itself; the C++ ones
// return the declared members, so the one case that stores an undeclared member (`extra`) drops it from the expected
// output.
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace rollups = provider::claude_capacity_rollups;
namespace history = provider::claude_capacity_history;
using provider::capacity_contract::ClaudeCapacityHistoryPoint;
using provider::capacity_contract::ClaudeCapacityHistoryView;
using provider::capacity_contract::ProviderCapacityClaudeWindow;

namespace {
  const char* const VECTORS =
#include "tests/claude_capacity_differential_vectors.inc"
    ;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTORS).as_object();
    return parsed;
  }
  double constant(std::string_view name) { return rt::toNumber(vectors().at("constants").at(name)); }

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, label + "\n      actual:   " + json::serialize(actual) + "\n      expected: " + json::serialize(expected), false);
  }

  json::value withoutExtra(json::value expected) {
    if (expected.is_object()) rt::objectDelete(expected.as_object(), "extra");
    return expected;
  }

  json::value number(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  std::optional<double> numberFrom(const json::value* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    return rt::toNumber(*value);
  }
  std::optional<std::string> stringFrom(const json::value* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    return std::string(value->as_string());
  }

  json::value windowJson(const std::optional<ProviderCapacityClaudeWindow>& window) {
    if (!window) return nullptr;
    json::object out;
    out["status"] = window->status ? json::value(json::string(*window->status)) : json::value(nullptr);
    out["used_percent"] = number(window->used_percent);
    out["reset_at_ms"] = number(window->reset_at_ms);
    out["observed_at_ms"] = number(window->observed_at_ms);
    return out;
  }
  std::optional<ProviderCapacityClaudeWindow> windowFrom(const json::value* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    return ProviderCapacityClaudeWindow{
      .status = stringFrom(rt::get(*value, "status")),
      .used_percent = numberFrom(rt::get(*value, "used_percent")),
      .reset_at_ms = numberFrom(rt::get(*value, "reset_at_ms")),
      .observed_at_ms = numberFrom(rt::get(*value, "observed_at_ms")),
    };
  }

  json::value pointJson(const ClaudeCapacityHistoryPoint& point) {
    json::object out;
    out["v"] = rt::makeNumber(point.v);
    out["point_id"] = json::string(point.point_id);
    out["bucket_start_at_ms"] = rt::makeNumber(point.bucket_start_at_ms);
    out["sampled_at_ms"] = rt::makeNumber(point.sampled_at_ms);
    out["source_observed_at_ms"] = number(point.source_observed_at_ms);
    out["account_id"] = json::string(point.account_id);
    out["account_cohort_id"] = json::string(point.account_cohort_id);
    out["credential_kind"] = json::string(point.credential_kind);
    out["workspace_id"] = point.workspace_id ? json::value(json::string(*point.workspace_id)) : json::value(nullptr);
    out["state"] = json::string(point.state);
    out["response_status"] = number(point.response_status);
    if (point.source) out["source"] = json::string(*point.source);
    json::object windows;
    windows["five_hour"] = windowJson(point.windows.five_hour);
    windows["seven_day"] = windowJson(point.windows.seven_day);
    out["windows"] = std::move(windows);
    if (point.usage_snapshot) out["usage_snapshot"] = *point.usage_snapshot;
    json::array standard;
    for (const ClaudeCapacityHistoryPoint::StandardLimit& item : point.standard) {
      json::object limit;
      limit["family"] = json::string(item.family);
      limit["limit"] = number(item.limit);
      limit["remaining"] = number(item.remaining);
      limit["reset_at_ms"] = number(item.reset_at_ms);
      limit["observed_at_ms"] = number(item.observed_at_ms);
      standard.emplace_back(std::move(limit));
    }
    out["standard"] = std::move(standard);
    return out;
  }
  // The generator's merge inputs (their bucket_start_at_ms is not a valid stored point's).
  ClaudeCapacityHistoryPoint pointFrom(const json::value& value) {
    ClaudeCapacityHistoryPoint point{
      .v = 1,
      .point_id = std::string(value.at("point_id").as_string()),
      .bucket_start_at_ms = rt::toNumber(value.at("bucket_start_at_ms")),
      .sampled_at_ms = rt::toNumber(value.at("sampled_at_ms")),
      .source_observed_at_ms = numberFrom(rt::get(value, "source_observed_at_ms")),
      .account_id = std::string(value.at("account_id").as_string()),
      .account_cohort_id = std::string(value.at("account_cohort_id").as_string()),
      .credential_kind = std::string(value.at("credential_kind").as_string()),
      .workspace_id = stringFrom(rt::get(value, "workspace_id")),
      .state = std::string(value.at("state").as_string()),
      .response_status = numberFrom(rt::get(value, "response_status")),
      .source = std::nullopt,
      .windows = {.five_hour = windowFrom(rt::get(value.at("windows"), "five_hour")), .seven_day = windowFrom(rt::get(value.at("windows"), "seven_day"))},
      .usage_snapshot = std::nullopt,
      .standard = {},
    };
    return point;
  }

  json::value viewJson(const ClaudeCapacityHistoryView& view) {
    json::object out;
    out["v"] = rt::makeNumber(view.v);
    out["bucket_ms"] = rt::makeNumber(view.bucket_ms);
    out["retention_ms"] = rt::makeNumber(view.retention_ms);
    out["stale_after_ms"] = rt::makeNumber(view.stale_after_ms);
    out["scan"] = json::string(view.scan);
    out["truncated"] = view.truncated;
    json::array points;
    for (const ClaudeCapacityHistoryPoint& point : view.points) points.push_back(pointJson(point));
    out["points"] = std::move(points);
    return out;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.emplace_back(json::string(part.string()));
      else parts.push_back(rt::makeNumber(part.number()));
    }
    return parts;
  }

  // Every row under ["uos_ai", "provider_capacity", "claude", "v1"], keyed by JSON.stringify(key).
  rt::Task<json::value> rowsOf(std::shared_ptr<rt::Kv> kv) {
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{"uos_ai", "provider_capacity", "claude", "v1"};
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, rt::KvListOptions());
    json::object rows;
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) rows[rt::jsonStringify(keyJson(entry->key))] = entry->value.toJson();
    co_return json::value(std::move(rows));
  }

  rt::Task<std::optional<std::string>> recordEvent(std::shared_ptr<rt::Kv> kv, provider::claude_code_quota::ClaudeCodeAcceptedObservation event) {
    std::optional<std::string> error;
    try {
      auto recording = history::recordClaudeCapacityObservation(std::move(kv), std::move(event));
      co_await std::move(recording);
    } catch (const rt::JsError& failure) {
      error = failure.toString();
    }
    co_return error;
  }

  template <class T> json::value plain(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
}

TEST("guards and readers over stored shapes match the TS") {
  const json::object& pure = vectors().at("pure").as_object();
  for (const json::value& item : pure.at("timestamp").as_array())
    expectJson(json::value(rollups::claudeCapacityTimestamp(rt::KvValue(item.at("input")))), item.at("output"), "timestamp " + json::serialize(item.at("input")), __FILE__,
               __LINE__);
  for (const json::value& item : pure.at("optionalTimestamp").as_array())
    expectJson(json::value(rollups::claudeCapacityOptionalTimestamp(rt::KvValue(item.at("input")))), item.at("output"),
               "optionalTimestamp " + json::serialize(item.at("input")), __FILE__, __LINE__);
  for (const json::value& item : pure.at("percent").as_array())
    expectJson(json::value(rollups::claudeCapacityPercent(rt::KvValue(item.at("input")))), item.at("output"), "percent " + json::serialize(item.at("input")), __FILE__,
               __LINE__);
  for (const json::value& item : pure.at("identity").as_array())
    expectJson(json::value(rollups::claudeCapacityIdentity(rt::KvValue(item.at("input")))), item.at("output"), "identity " + json::serialize(item.at("input")), __FILE__,
               __LINE__);
  for (const json::value& item : pure.at("usedPercent").as_array())
    expectJson(number(history::claudeCapacityUsedPercent(numberFrom(&item.at("input")))), item.at("output"), "usedPercent " + json::serialize(item.at("input")), __FILE__,
               __LINE__);
  for (const json::value& item : pure.at("readRollup").as_array())
    expectJson(plain(rollups::readClaudeCapacityRollup(rt::KvValue(item.at("input")))), withoutExtra(item.at("output")), "readRollup " + json::serialize(item.at("input")),
               __FILE__, __LINE__);
  for (const json::value& item : pure.at("readUsageRollup").as_array())
    expectJson(plain(rollups::readClaudeUsageRollup(rt::KvValue(item.at("input")))), item.at("output"), "readUsageRollup " + json::serialize(item.at("input")), __FILE__,
               __LINE__);
  for (const json::value& item : pure.at("readPoint").as_array()) {
    const std::optional<ClaudeCapacityHistoryPoint> point = history::readClaudeCapacityHistoryPoint(rt::KvValue(item.at("input")));
    expectJson(point ? pointJson(*point) : json::value(nullptr), withoutExtra(item.at("output")), "readPoint " + json::serialize(item.at("input")), __FILE__, __LINE__);
  }
}

ASYNC_TEST("rollup and usage-rollup merges follow the TS step by step") {
  std::optional<rollups::ClaudeCapacityRollupPoint> previous;
  for (const json::value& step : vectors().at("mergeSteps").as_array()) {
    if (const json::value* sevenKey = rt::get(step, "sevenKey")) {
      json::value point = rt::jsonParse(R"({"v":1,"point_id":"p","bucket_start_at_ms":0,"sampled_at_ms":0,"source_observed_at_ms":null,"account_id":"acct-1",)"
                                        R"("account_cohort_id":"cohort-1","credential_kind":"oauth","workspace_id":null,"state":"available","response_status":200,)"
                                        R"("windows":{"five_hour":null,"seven_day":null}})");
      point.as_object()["sampled_at_ms"] = rt::makeNumber(constant("B") + 100);
      expectJson(keyJson(rollups::claudeCapacityRollupKey(pointFrom(point), "seven_day")), *sevenKey, "sevenKey", __FILE__, __LINE__);
      continue;
    }
    const ClaudeCapacityHistoryPoint point = pointFrom(step.at("point"));
    const std::string window(step.at("window").as_string());
    expectJson(keyJson(rollups::claudeCapacityRollupKey(point, window)), step.at("key"), "rollup key", __FILE__, __LINE__);
    try {
      previous = rollups::mergeClaudeCapacityRollup(previous, point, window);
      expectJson(json::value_from(*previous), step.at("ok"), "rollup merge", __FILE__, __LINE__);
    } catch (const rt::JsError& error) {
      expectJson(json::value(error.toString()), step.at("error"), "rollup merge error", __FILE__, __LINE__);
    }
  }
  std::optional<rollups::ClaudeUsageRollup> usage;
  json::value base = rt::jsonParse(R"({"v":1,"point_id":"p","bucket_start_at_ms":0,"sampled_at_ms":0,"source_observed_at_ms":null,"account_id":"acct-1",)"
                                   R"("account_cohort_id":"cohort-1","credential_kind":"api_key","workspace_id":"wrk","state":"available","response_status":200,)"
                                   R"("windows":{"five_hour":null,"seven_day":null}})");
  const ClaudeCapacityHistoryPoint usagePoint = pointFrom(base);
  for (const json::value& step : vectors().at("usageSteps").as_array()) {
    const json::object meter = step.at("meter").as_object();
    try {
      usage = rollups::mergeClaudeUsageRollup(usage, usagePoint, meter);
      expectJson(json::value_from(*usage), step.at("ok"), "usage merge", __FILE__, __LINE__);
      auto keying = rollups::claudeUsageRollupKey(rollups::ClaudeUsageRollupKeyPoint{.account_id = "acct-1", .account_cohort_id = "cohort-1"}, meter);
      const rt::KvKey key = co_await std::move(keying);
      expectJson(keyJson(key), step.at("key"), "usage key", __FILE__, __LINE__);
    } catch (const rt::JsError& error) {
      expectJson(json::value(error.toString()), step.at("error"), "usage merge error", __FILE__, __LINE__);
    }
  }
}

ASYNC_TEST("recorded observations retain, roll up and read back like the TS") {
  const double NOW = constant("NOW");
  rt::globalDateNow() = [NOW] { return NOW; };
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([NOW] { return NOW; });
  const json::object& sequence = vectors().at("scenarios").at("sequence").as_object();
  std::size_t index = 0;
  for (const json::value& step : sequence.at("steps").as_array()) {
    const auto event = json::value_to<provider::claude_code_quota::ClaudeCodeAcceptedObservation>(step.at("event"));
    auto recording = recordEvent(kv, event);
    const std::optional<std::string> error = co_await std::move(recording);
    const std::string label = "step " + std::to_string(index++) + " (" + std::string(step.at("event").at("observation_id").as_string()) + ")";
    if (const json::value* expectedError = rt::get(step, "error")) expectJson(error ? json::value(*error) : json::value(nullptr), *expectedError, label + " error", __FILE__, __LINE__);
    else expectJson(error ? json::value(*error) : json::value(nullptr), json::value(nullptr), label + " error", __FILE__, __LINE__);
    auto reading = rowsOf(kv);
    const json::value rows = co_await std::move(reading);
    expectJson(rows, step.at("rows"), label + " rows", __FILE__, __LINE__);
  }
  const json::object& reads = sequence.at("reads").as_object();
  const double S = constant("S");
  const double T0 = constant("T0");
  constexpr double H = 3'600'000;
  {
    auto reading = history::readClaudeCapacityHistory(kv, NOW + 900'000 * 4);
    const ClaudeCapacityHistoryView view = co_await std::move(reading);
    expectJson(viewJson(view), reads.at("history"), "history", __FILE__, __LINE__);
  }
  {
    auto reading = history::readClaudeCapacityHistory(kv, T0 + 25);
    const ClaudeCapacityHistoryView view = co_await std::move(reading);
    expectJson(viewJson(view), reads.at("historyEarly"), "historyEarly", __FILE__, __LINE__);
  }
  {
    auto reading = history::readClaudeCapacityHistory(kv, NOW + 8 * 24 * H);
    const ClaudeCapacityHistoryView view = co_await std::move(reading);
    expectJson(viewJson(view), reads.at("historyLate"), "historyLate", __FILE__, __LINE__);
  }
  {
    auto listing = rollups::listClaudeCapacityRollups(kv, S - 5 * H, NOW + 900'000 * 4);
    const rollups::ListClaudeCapacityRollupsResult result = co_await std::move(listing);
    expectJson(json::value_from(result), reads.at("rollups"), "rollups", __FILE__, __LINE__);
  }
  {
    auto listing = rollups::listClaudeCapacityRollups(kv, S, S + 10);
    const rollups::ListClaudeCapacityRollupsResult result = co_await std::move(listing);
    expectJson(json::value_from(result), reads.at("rollupsEarly"), "rollupsEarly", __FILE__, __LINE__);
  }
  {
    auto listing = rollups::listClaudeUsageRollups(kv, S - 5 * H, NOW + 900'000 * 4);
    const rollups::ListClaudeUsageRollupsResult result = co_await std::move(listing);
    expectJson(json::value_from(result), reads.at("usageRollups"), "usageRollups", __FILE__, __LINE__);
  }
  {
    auto listing = rollups::listClaudeUsageRollups(kv, S, T0 + 100);
    const rollups::ListClaudeUsageRollupsResult result = co_await std::move(listing);
    expectJson(json::value_from(result), reads.at("usageEarly"), "usageEarly", __FILE__, __LINE__);
  }
  {
    auto reading = history::readClaudeCapacityHistory(nullptr, NOW);
    const ClaudeCapacityHistoryView view = co_await std::move(reading);
    expectJson(viewJson(view), reads.at("nullHistory"), "nullHistory", __FILE__, __LINE__);
    auto listing = rollups::listClaudeCapacityRollups(nullptr, 0, NOW);
    const rollups::ListClaudeCapacityRollupsResult result = co_await std::move(listing);
    expectJson(json::value_from(result), reads.at("nullRollups"), "nullRollups", __FILE__, __LINE__);
    auto usageListing = rollups::listClaudeUsageRollups(nullptr, 0, NOW);
    const rollups::ListClaudeUsageRollupsResult usage = co_await std::move(usageListing);
    expectJson(json::value_from(usage), reads.at("nullUsage"), "nullUsage", __FILE__, __LINE__);
  }
}

ASYNC_TEST("corrupt history and rollup evidence fails closed like the TS") {
  const double NOW = constant("NOW");
  const double S = constant("S");
  const double B = constant("B");
  const double T0 = constant("T0");
  constexpr double H = 3'600'000;
  rt::globalDateNow() = [NOW] { return NOW; };
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([NOW] { return NOW; });
  const json::array& expected = vectors().at("scenarios").at("corrupt").as_array();
  const auto first = json::value_to<provider::claude_code_quota::ClaudeCodeAcceptedObservation>(
    vectors().at("scenarios").at("sequence").at("steps").as_array()[0].at("event"));
  const rt::KvKey bucketKey{history::CLAUDE_CAPACITY_HISTORY_PREFIX, S, "acct-1", "cohort-1"};
  const rt::KvValue corruptBucket(rt::jsonParse(R"({"v":1,"points":[],"flags":{}})"));
  auto seedingBucket = kv->set(bucketKey, corruptBucket);
  co_await std::move(seedingBucket);
  {
    auto recording = recordEvent(kv, first);
    const std::optional<std::string> error = co_await std::move(recording);
    json::object step;
    if (error) step["error"] = json::string(*error);
    else step["ok"] = true;
    expectJson(json::value(std::move(step)), expected[0], "corrupt bucket record", __FILE__, __LINE__);
  }
  {
    auto reading = history::readClaudeCapacityHistory(kv, NOW);
    const ClaudeCapacityHistoryView view = co_await std::move(reading);
    json::object step;
    step["history"] = viewJson(view);
    expectJson(json::value(std::move(step)), expected[1], "corrupt bucket read", __FILE__, __LINE__);
  }
  co_await kv->delete_(bucketKey);
  const rt::KvKey rollupKey{rollups::CLAUDE_CAPACITY_ROLLUP_PREFIX, rt::Math::floor((T0 + 9) / H) * H, "acct-1", "cohort-1", "five_hour", S + 5 * H};
  const rt::KvValue corruptRollup(rt::jsonParse(R"({"v":1})"));
  auto seedingRollup = kv->set(rollupKey, corruptRollup);
  co_await std::move(seedingRollup);
  {
    auto recording = recordEvent(kv, first);
    const std::optional<std::string> error = co_await std::move(recording);
    json::object step;
    if (error) step["error"] = json::string(*error);
    else step["ok"] = true;
    expectJson(json::value(std::move(step)), expected[2], "corrupt rollup record", __FILE__, __LINE__);
  }
  {
    auto listing = rollups::listClaudeCapacityRollups(kv, S - H, NOW);
    const rollups::ListClaudeCapacityRollupsResult result = co_await std::move(listing);
    auto reading = rowsOf(kv);
    const json::value rows = co_await std::move(reading);
    json::object step;
    step["rollups"] = json::value_from(result);
    step["rows"] = rows;
    expectJson(json::value(std::move(step)), expected[3], "corrupt rollup list", __FILE__, __LINE__);
  }
  co_await kv->delete_(rollupKey);
  {
    json::value stored = rt::jsonParse(R"({"v":1,"bucket_start_at_ms":0,"account_id":"acct-1","account_cohort_id":"cohort-1","credential_kind":"api_key","workspace_id":"wrk",)"
                                       R"("meter":{"id":"legacy:five_hour","label":"5-hour window","kind":"five_hour","group":"session","scope":null,"severity":null,"is_active":false,)"
                                       R"("used_percent":25,"resets_at":"2023-11-15T03:00:00Z","reset_at_ms":0,"observed_at_ms":0},"samples":2,"first_observed_at_ms":0,)"
                                       R"("last_observed_at_ms":0,"min_used_percent":20,"max_used_percent":25})");
    stored.as_object()["bucket_start_at_ms"] = rt::makeNumber(B);
    stored.as_object()["first_observed_at_ms"] = rt::makeNumber(B + 10);
    stored.as_object()["last_observed_at_ms"] = rt::makeNumber(B + 50);
    stored.at("meter").as_object()["observed_at_ms"] = rt::makeNumber(B + 50);
    stored.at("meter").as_object()["reset_at_ms"] = rt::makeNumber(rt::Date::parse("2023-11-15T03:00:00Z"));
    const rt::KvKey usageKey{rollups::CLAUDE_USAGE_ROLLUP_PREFIX, S - rt::mod(S, H), "acct-1", "cohort-1", "nothex"};
    const rt::KvValue usageValue(stored);
    auto seedingUsage = kv->set(usageKey, usageValue);
    co_await std::move(seedingUsage);
    auto listing = rollups::listClaudeUsageRollups(kv, S - H, NOW);
    const rollups::ListClaudeUsageRollupsResult result = co_await std::move(listing);
    json::object step;
    step["usage"] = json::value_from(result);
    expectJson(json::value(std::move(step)), expected[4], "corrupt usage list", __FILE__, __LINE__);
  }
  {
    json::value moved = rt::jsonParse(R"({"v":1,"bucket_start_at_ms":0,"account_id":"acct-1","account_cohort_id":"cohort-1","credential_kind":"oauth","workspace_id":null,)"
                                      R"("window":"five_hour","reset_at_ms":0,"samples":3,"available_sample_count":2,"unavailable_sample_count":1,)"
                                      R"("first_source_observed_at_ms":0,"last_source_observed_at_ms":0,"first_sampled_at_ms":0,"last_sampled_at_ms":0,)"
                                      R"("min_used_percent":10,"max_used_percent":30,"last_used_percent":20,"last_status":"allowed"})");
    json::object& fields = moved.as_object();
    fields["bucket_start_at_ms"] = rt::makeNumber(B);
    fields["reset_at_ms"] = rt::makeNumber(B + 5 * H);
    fields["first_source_observed_at_ms"] = rt::makeNumber(B + 10);
    fields["last_source_observed_at_ms"] = rt::makeNumber(B + 20);
    fields["first_sampled_at_ms"] = rt::makeNumber(B + 11);
    fields["last_sampled_at_ms"] = rt::makeNumber(B + 21);
    const rt::KvKey movedKey{rollups::CLAUDE_CAPACITY_ROLLUP_PREFIX, B, "acct-1", "cohort-1", "five_hour", "unknown"};
    const rt::KvValue movedValue(moved);
    auto seedingMoved = kv->set(movedKey, movedValue);
    co_await std::move(seedingMoved);
    auto listing = rollups::listClaudeCapacityRollups(kv, S - H, NOW);
    const rollups::ListClaudeCapacityRollupsResult result = co_await std::move(listing);
    json::object step;
    step["rollups"] = json::value_from(result);
    expectJson(json::value(std::move(step)), expected[5], "moved rollup list", __FILE__, __LINE__);
  }
}
