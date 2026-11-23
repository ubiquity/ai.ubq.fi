#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <utility>

namespace uos::provider::claude_capacity_rollups {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using capacity_contract::ClaudeCapacityHistoryPoint;
    using capacity_contract::ProviderCapacityClaudeWindow;
    using claude_code_usage_schema::ClaudeCodeUsageMeter;
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr double CLAUDE_CAPACITY_ROLLUP_BUCKET_MS = 60 * 60'000;

    // `typeof value === "object" && value !== null` (utils.isRecord) on a stored value.
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }
    std::optional<double> numberOrNull(const rt::KvValue& value) { return value.isNull() ? std::nullopt : std::optional<double>(value.asNumber()); }
    std::optional<std::string> stringOrNull(const rt::KvValue& value) { return value.isNull() ? std::nullopt : std::optional<std::string>(value.asString()); }

    // A stored value as the JSON readers of claude-code-usage-schema see it: undefined members are
    // absent and every value JSON cannot hold (bigint, Map, Date, ...) reads as an empty object.

    // `point.windows[window]`
    const std::optional<ProviderCapacityClaudeWindow>* windowOf(const ClaudeCapacityHistoryPoint& point, std::string_view window) {
      if (window == "five_hour") return &point.windows.five_hour;
      if (window == "seven_day") return &point.windows.seven_day;
      return nullptr;
    }
    const ProviderCapacityClaudeWindow* observedWindow(const ClaudeCapacityHistoryPoint& point, std::string_view window) {
      const std::optional<ProviderCapacityClaudeWindow>* slot = windowOf(point, window);
      return slot != nullptr && *slot ? &**slot : nullptr;
    }

    double bucketOf(double at) { return rt::Math::floor(at / CLAUDE_CAPACITY_ROLLUP_BUCKET_MS) * CLAUDE_CAPACITY_ROLLUP_BUCKET_MS; }

    // `JSON.stringify(key) === JSON.stringify(expected)` for an expected key of strings and finite
    // numbers: a string part must be that string and a number part a number JSON prints the same
    // (-0 and 0 alike; NaN and the infinities print as null and never match). Any other part (a
    // bigint, which JSON.stringify refuses, bytes, a boolean) is a mismatch, which the callers treat
    // exactly as the TS treats both outcomes.
    bool sameKeyJson(const rt::KvKey& actual, const rt::KvKey& expected) {
      if (actual.size() != expected.size()) return false;
      for (std::size_t index = 0; index < actual.size(); ++index) {
        const rt::KvKeyPart& part = actual[index];
        const rt::KvKeyPart& wanted = expected[index];
        if (wanted.isString()) {
          if (!part.isString() || part.string() != wanted.string()) return false;
        } else if (wanted.isNumber()) {
          if (!part.isNumber() || !rt::Number::isFinite(part.number()) || part.number() != wanted.number()) return false;
        } else {
          return false;
        }
      }
      return true;
    }

    bool validRollupIdentity(const rt::KvValue& value) {
      return claudeCapacityIdentity(value["account_id"]) && claudeCapacityIdentity(value["account_cohort_id"]) &&
             (isText(value["credential_kind"], "oauth") || isText(value["credential_kind"], "api_key")) &&
             (value["workspace_id"].isNull() || claudeCapacityIdentity(value["workspace_id"])) &&
             (isText(value["window"], "five_hour") || isText(value["window"], "seven_day"));
    }

    bool validRollupOrder(const ClaudeCapacityRollupPoint& value) {
      const std::optional<double>& first = value.first_source_observed_at_ms;
      const std::optional<double>& last = value.last_source_observed_at_ms;
      if (!first != !last) return false;
      if (first && last && (*first > *last || *last > value.last_sampled_at_ms)) return false;
      if (first && *first < value.bucket_start_at_ms) return false;
      if (last && *last >= value.bucket_start_at_ms + CLAUDE_CAPACITY_ROLLUP_BUCKET_MS) return false;
      const std::optional<double>& min = value.min_used_percent;
      const std::optional<double>& max = value.max_used_percent;
      if (!min != !max) return false;
      if (min && max && *min > *max) return false;
      return !value.last_used_percent || (min && max && *value.last_used_percent >= *min && *value.last_used_percent <= *max);
    }

    std::optional<ClaudeCapacityRollupPoint> rollupForEntry(const rt::KvEntry<>& entry, double first, double last, double nowMs) {
      for (std::size_t index = 0; index < CLAUDE_CAPACITY_ROLLUP_PREFIX.size(); ++index)
        if (index >= entry.key.size() || !(entry.key[index] == CLAUDE_CAPACITY_ROLLUP_PREFIX[index])) return std::nullopt;
      if (entry.key.size() <= CLAUDE_CAPACITY_ROLLUP_PREFIX.size()) return std::nullopt;
      const rt::KvKeyPart& keyBucket = entry.key[CLAUDE_CAPACITY_ROLLUP_PREFIX.size()];
      if (!keyBucket.isNumber() || keyBucket.number() < first || keyBucket.number() > last) return std::nullopt;
      std::optional<ClaudeCapacityRollupPoint> point = readClaudeCapacityRollup(entry.value);
      if (!point || point->last_sampled_at_ms > nowMs) throw rt::JsError("Claude rollup evidence is corrupt");
      rt::KvKey expected = CLAUDE_CAPACITY_ROLLUP_PREFIX;
      expected.push_back(point->bucket_start_at_ms);
      expected.push_back(point->account_id);
      expected.push_back(point->account_cohort_id);
      expected.push_back(point->window);
      if (point->reset_at_ms) expected.push_back(*point->reset_at_ms);
      else expected.push_back("unknown");
      if (!sameKeyJson(entry.key, expected)) throw rt::JsError("Claude rollup key is corrupt");
      return point;
    }

    double minOf(const std::vector<double>& values) { return rt::Math::min(std::span<const double>(values)); }
    double maxOf(const std::vector<double>& values) { return rt::Math::max(std::span<const double>(values)); }

    // `left === right` on two JSON members (strings or null after validation).
    bool sameMember(const json::object& left, const json::object& right, std::string_view key) {
      const json::value* a = rt::get(left, key);
      const json::value* b = rt::get(right, key);
      if (a == nullptr || b == nullptr) return a == b;
      return rt::strictEquals(*a, *b);
    }
  }

  bool claudeCapacityTimestamp(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0; }
  bool claudeCapacityOptionalTimestamp(const rt::KvValue& value) { return value.isNull() || claudeCapacityTimestamp(value); }
  bool claudeCapacityPercent(const rt::KvValue& value) {
    return value.isNull() || (value.isNumber() && rt::Number::isFinite(value.asNumber()) && value.asNumber() >= 0 && value.asNumber() <= 100);
  }
  bool claudeCapacityIdentity(const rt::KvValue& value) {
    if (!value.isString()) return false;
    const std::int64_t length = rt::length(value.asString());
    return length > 0 && length <= 200;
  }

  std::optional<ClaudeCapacityRollupPoint> readClaudeCapacityRollup(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !(value["v"].isNumber() && value["v"].asNumber() == 1)) return std::nullopt;
    if (!claudeCapacityTimestamp(value["bucket_start_at_ms"]) || rt::mod(value["bucket_start_at_ms"].asNumber(), CLAUDE_CAPACITY_ROLLUP_BUCKET_MS) != 0)
      return std::nullopt;
    if (!validRollupIdentity(value)) return std::nullopt;
    if (!claudeCapacityOptionalTimestamp(value["reset_at_ms"])) return std::nullopt;
    if (!claudeCapacityTimestamp(value["samples"]) || !claudeCapacityTimestamp(value["available_sample_count"]) ||
        !claudeCapacityTimestamp(value["unavailable_sample_count"]) || value["samples"].asNumber() == 0)
      return std::nullopt;
    if (value["available_sample_count"].asNumber() + value["unavailable_sample_count"].asNumber() != value["samples"].asNumber()) return std::nullopt;
    if (!claudeCapacityTimestamp(value["first_sampled_at_ms"]) || !claudeCapacityTimestamp(value["last_sampled_at_ms"])) return std::nullopt;
    if (value["first_sampled_at_ms"].asNumber() > value["last_sampled_at_ms"].asNumber()) return std::nullopt;
    if (!claudeCapacityOptionalTimestamp(value["first_source_observed_at_ms"]) || !claudeCapacityOptionalTimestamp(value["last_source_observed_at_ms"]))
      return std::nullopt;
    if (!claudeCapacityPercent(value["min_used_percent"]) || !claudeCapacityPercent(value["max_used_percent"]) || !claudeCapacityPercent(value["last_used_percent"]))
      return std::nullopt;
    if (!value["last_status"].isNull() && !value["last_status"].isString()) return std::nullopt;
    ClaudeCapacityRollupPoint point{
      .v = 1,
      .bucket_start_at_ms = value["bucket_start_at_ms"].asNumber(),
      .account_id = value["account_id"].asString(),
      .account_cohort_id = value["account_cohort_id"].asString(),
      .credential_kind = value["credential_kind"].asString(),
      .workspace_id = stringOrNull(value["workspace_id"]),
      .window = value["window"].asString(),
      .reset_at_ms = numberOrNull(value["reset_at_ms"]),
      .samples = value["samples"].asNumber(),
      .available_sample_count = value["available_sample_count"].asNumber(),
      .unavailable_sample_count = value["unavailable_sample_count"].asNumber(),
      .first_source_observed_at_ms = numberOrNull(value["first_source_observed_at_ms"]),
      .last_source_observed_at_ms = numberOrNull(value["last_source_observed_at_ms"]),
      .first_sampled_at_ms = value["first_sampled_at_ms"].asNumber(),
      .last_sampled_at_ms = value["last_sampled_at_ms"].asNumber(),
      .min_used_percent = numberOrNull(value["min_used_percent"]),
      .max_used_percent = numberOrNull(value["max_used_percent"]),
      .last_used_percent = numberOrNull(value["last_used_percent"]),
      .last_status = stringOrNull(value["last_status"]),
    };
    if (!validRollupOrder(point)) return std::nullopt;
    return point;
  }

  rt::KvKey claudeCapacityRollupKey(const ClaudeCapacityHistoryPoint& point, std::string_view window) {
    const ProviderCapacityClaudeWindow* observed = observedWindow(point, window);
    double at = point.sampled_at_ms;
    if (observed != nullptr && observed->observed_at_ms) at = *observed->observed_at_ms;
    else if (point.source_observed_at_ms) at = *point.source_observed_at_ms;
    rt::KvKey key = CLAUDE_CAPACITY_ROLLUP_PREFIX;
    key.push_back(bucketOf(at));
    key.push_back(point.account_id);
    key.push_back(point.account_cohort_id);
    key.push_back(std::string(window));
    if (observed != nullptr && observed->reset_at_ms) key.push_back(*observed->reset_at_ms);
    else key.push_back("unknown");
    return key;
  }

  ClaudeCapacityRollupPoint mergeClaudeCapacityRollup(const std::optional<ClaudeCapacityRollupPoint>& previous, const ClaudeCapacityHistoryPoint& point,
                                                      std::string_view window) {
    const ProviderCapacityClaudeWindow* observed = observedWindow(point, window);
    const double bucket = claudeCapacityRollupKey(point, window)[CLAUDE_CAPACITY_ROLLUP_PREFIX.size()].number();
    const std::optional<double> resetAtMs = observed != nullptr ? observed->reset_at_ms : std::nullopt;
    if (previous && (previous->bucket_start_at_ms != bucket || previous->account_id != point.account_id || previous->account_cohort_id != point.account_cohort_id ||
                     previous->credential_kind != point.credential_kind || previous->workspace_id != point.workspace_id || previous->window != window ||
                     previous->reset_at_ms != resetAtMs))
      throw rt::JsError("Claude rollup binding mismatch");
    const std::optional<double> sourceAt = observed != nullptr && observed->observed_at_ms ? observed->observed_at_ms : point.source_observed_at_ms;
    const std::optional<double> used = observed != nullptr ? observed->used_percent : std::nullopt;
    const bool newer = !previous || sourceAt.value_or(point.sampled_at_ms) >= previous->last_source_observed_at_ms.value_or(previous->last_sampled_at_ms);
    std::vector<double> values;
    if (previous && previous->min_used_percent) values.push_back(*previous->min_used_percent);
    if (previous && previous->max_used_percent) values.push_back(*previous->max_used_percent);
    if (used) values.push_back(*used);
    std::vector<double> sourceTimes;
    if (previous && previous->first_source_observed_at_ms) sourceTimes.push_back(*previous->first_source_observed_at_ms);
    if (previous && previous->last_source_observed_at_ms) sourceTimes.push_back(*previous->last_source_observed_at_ms);
    if (sourceAt) sourceTimes.push_back(*sourceAt);
    return {
      .v = 1,
      .bucket_start_at_ms = bucket,
      .account_id = point.account_id,
      .account_cohort_id = point.account_cohort_id,
      .credential_kind = point.credential_kind,
      .workspace_id = point.workspace_id,
      .window = std::string(window),
      .reset_at_ms = resetAtMs,
      .samples = (previous ? previous->samples : 0) + 1,
      .available_sample_count = (previous ? previous->available_sample_count : 0) + (point.state == "available" ? 1 : 0),
      .unavailable_sample_count = (previous ? previous->unavailable_sample_count : 0) + (point.state == "unavailable" ? 1 : 0),
      .first_source_observed_at_ms = sourceTimes.empty() ? std::nullopt : std::optional<double>(minOf(sourceTimes)),
      .last_source_observed_at_ms = sourceTimes.empty() ? std::nullopt : std::optional<double>(maxOf(sourceTimes)),
      .first_sampled_at_ms = rt::Math::min(previous ? previous->first_sampled_at_ms : point.sampled_at_ms, point.sampled_at_ms),
      .last_sampled_at_ms = rt::Math::max(previous ? previous->last_sampled_at_ms : point.sampled_at_ms, point.sampled_at_ms),
      .min_used_percent = values.empty() ? std::nullopt : std::optional<double>(minOf(values)),
      .max_used_percent = values.empty() ? std::nullopt : std::optional<double>(maxOf(values)),
      .last_used_percent = newer ? used : previous->last_used_percent,
      .last_status = newer ? (observed != nullptr ? observed->status : std::nullopt) : previous->last_status,
    };
  }

  rt::Task<ListClaudeCapacityRollupsResult> listClaudeCapacityRollups(std::shared_ptr<rt::Kv> kv, double sinceMs, double nowMs) {
    if (!kv) co_return ListClaudeCapacityRollupsResult{.scan = "unavailable", .truncated = false, .points = {}};
    std::vector<ClaudeCapacityRollupPoint> points;
    bool failed = false;
    bool truncated = false;
    try {
      const double firstHour = bucketOf(sinceMs);
      const double lastHour = bucketOf(nowMs);
      double scanned = 0;
      rt::KvListSelector selector;
      selector.start = rt::KvKey{CLAUDE_CAPACITY_ROLLUP_PREFIX, firstHour};
      selector.end = rt::KvKey{CLAUDE_CAPACITY_ROLLUP_PREFIX, lastHour + CLAUDE_CAPACITY_ROLLUP_BUCKET_MS};
      rt::KvListOptions options;
      options.limit = CLAUDE_CAPACITY_READ_LIMIT + 1;
      options.reverse = true;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (++scanned > CLAUDE_CAPACITY_READ_LIMIT) {
          truncated = true;
          co_await iterator->return_();
          break;
        }
        std::optional<ClaudeCapacityRollupPoint> point = rollupForEntry(*entry, firstHour, lastHour, nowMs);
        if (point) points.push_back(std::move(*point));
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return ListClaudeCapacityRollupsResult{.scan = "unavailable", .truncated = false, .points = {}};
    rt::sort(points, [](const ClaudeCapacityRollupPoint& a, const ClaudeCapacityRollupPoint& b) { return a.bucket_start_at_ms - b.bucket_start_at_ms; });
    co_return ListClaudeCapacityRollupsResult{.scan = "ok", .truncated = truncated, .points = std::move(points)};
  }

  rt::Task<rt::KvKey> claudeUsageRollupKey(ClaudeUsageRollupKeyPoint point, ClaudeCodeUsageMeter meter) {
    const double observedAtMs = rt::toNumber(rt::get(meter, "observed_at_ms"));
    json::array parts;
    parts.push_back(rt::isNullish(rt::get(meter, "id")) ? json::value(nullptr) : *rt::get(meter, "id"));
    parts.push_back(rt::isNullish(rt::get(meter, "resets_at")) ? json::value(nullptr) : *rt::get(meter, "resets_at"));
    rt::Promise<std::string> hashing(utils::sha256Hex(rt::jsonStringify(json::value(std::move(parts)))));
    const std::string digest = co_await hashing.wait();
    rt::KvKey key = CLAUDE_USAGE_ROLLUP_PREFIX;
    key.push_back(bucketOf(observedAtMs));
    key.push_back(point.account_id);
    key.push_back(point.account_cohort_id);
    key.push_back(digest);
    co_return key;
  }

  std::optional<ClaudeUsageRollup> readClaudeUsageRollup(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !(value["v"].isNumber() && value["v"].asNumber() == 1) || !claudeCapacityIdentity(value["account_id"]) ||
        !claudeCapacityIdentity(value["account_cohort_id"]))
      return std::nullopt;
    if ((!isText(value["credential_kind"], "oauth") && !isText(value["credential_kind"], "api_key")) ||
        (!value["workspace_id"].isNull() && !claudeCapacityIdentity(value["workspace_id"])))
      return std::nullopt;
    if (!claudeCapacityTimestamp(value["bucket_start_at_ms"]) || !claudeCapacityTimestamp(value["first_observed_at_ms"]) ||
        !claudeCapacityTimestamp(value["last_observed_at_ms"]) || !claudeCapacityTimestamp(value["samples"]) || value["samples"].asNumber() < 1)
      return std::nullopt;
    const double bucket = value["bucket_start_at_ms"].asNumber();
    const double first = value["first_observed_at_ms"].asNumber();
    const double last = value["last_observed_at_ms"].asNumber();
    if (first > last || rt::mod(bucket, CLAUDE_CAPACITY_ROLLUP_BUCKET_MS) != 0) return std::nullopt;
    const json::value meterView = rt::storedJsonView(value["meter"], STORED_JSON_VIEW_OPTIONS);
    const std::optional<ClaudeCodeUsageMeter> meter = claude_code_usage_schema::readClaudeCodeUsageMeter(meterView);
    if (!meter || rt::getNumber(*meter, "observed_at_ms") != std::optional<double>(last)) return std::nullopt;
    if (first < bucket || last >= bucket + CLAUDE_CAPACITY_ROLLUP_BUCKET_MS) return std::nullopt;
    const rt::KvValue& min = value["min_used_percent"];
    const rt::KvValue& max = value["max_used_percent"];
    const auto finiteOrNull = [](const rt::KvValue& item) { return item.isNull() || (item.isNumber() && rt::Number::isFinite(item.asNumber())); };
    if (!finiteOrNull(min) || !finiteOrNull(max) || min.isNull() != max.isNull()) return std::nullopt;
    if (min.isNumber() && max.isNumber()) {
      const json::value* usedPercent = rt::get(*meter, "used_percent");
      const bool hasUsed = usedPercent != nullptr && !usedPercent->is_null();
      const double used = hasUsed ? rt::toNumber(*usedPercent) : 0;
      if (min.asNumber() > max.asNumber() || (hasUsed && (used < min.asNumber() || used > max.asNumber()))) return std::nullopt;
    }
    return ClaudeUsageRollup{
      .v = 1,
      .bucket_start_at_ms = bucket,
      .account_id = value["account_id"].asString(),
      .account_cohort_id = value["account_cohort_id"].asString(),
      .credential_kind = value["credential_kind"].asString(),
      .workspace_id = stringOrNull(value["workspace_id"]),
      .meter = meterView.as_object(),
      .samples = value["samples"].asNumber(),
      .first_observed_at_ms = first,
      .last_observed_at_ms = last,
      .min_used_percent = numberOrNull(min),
      .max_used_percent = numberOrNull(max),
    };
  }

  ClaudeUsageRollup mergeClaudeUsageRollup(const std::optional<ClaudeUsageRollup>& previous, const ClaudeCapacityHistoryPoint& point, const ClaudeCodeUsageMeter& meter) {
    const double observedAtMs = rt::toNumber(rt::get(meter, "observed_at_ms"));
    const double bucket = bucketOf(observedAtMs);
    if (previous && (previous->account_id != point.account_id || previous->account_cohort_id != point.account_cohort_id || previous->bucket_start_at_ms != bucket ||
                     previous->credential_kind != point.credential_kind || previous->workspace_id != point.workspace_id || !sameMember(previous->meter, meter, "id") ||
                     !sameMember(previous->meter, meter, "resets_at")))
      throw rt::JsError("Claude usage rollup binding mismatch");
    std::vector<double> values;
    if (previous && previous->min_used_percent) values.push_back(*previous->min_used_percent);
    if (previous && previous->max_used_percent) values.push_back(*previous->max_used_percent);
    if (const json::value* used = rt::get(meter, "used_percent"); used != nullptr && rt::isNumber(*used)) values.push_back(rt::toNumber(*used));
    return {
      .v = 1,
      .bucket_start_at_ms = bucket,
      .account_id = point.account_id,
      .account_cohort_id = point.account_cohort_id,
      .credential_kind = point.credential_kind,
      .workspace_id = point.workspace_id,
      .meter = !previous || observedAtMs >= previous->last_observed_at_ms ? meter : previous->meter,
      .samples = (previous ? previous->samples : 0) + 1,
      .first_observed_at_ms = rt::Math::min(previous ? previous->first_observed_at_ms : observedAtMs, observedAtMs),
      .last_observed_at_ms = rt::Math::max(previous ? previous->last_observed_at_ms : observedAtMs, observedAtMs),
      .min_used_percent = values.empty() ? std::nullopt : std::optional<double>(minOf(values)),
      .max_used_percent = values.empty() ? std::nullopt : std::optional<double>(maxOf(values)),
    };
  }

  rt::Task<ListClaudeUsageRollupsResult> listClaudeUsageRollups(std::shared_ptr<rt::Kv> kv, double sinceMs, double nowMs) {
    if (!kv) co_return ListClaudeUsageRollupsResult{.scan = "unavailable", .truncated = false, .points = {}};
    std::vector<ClaudeUsageRollup> points;
    bool failed = false;
    bool truncated = false;
    try {
      const double first = bucketOf(sinceMs);
      const double last = bucketOf(nowMs);
      double scanned = 0;
      rt::KvListSelector selector;
      selector.start = rt::KvKey{CLAUDE_USAGE_ROLLUP_PREFIX, first};
      selector.end = rt::KvKey{CLAUDE_USAGE_ROLLUP_PREFIX, last + CLAUDE_CAPACITY_ROLLUP_BUCKET_MS};
      rt::KvListOptions options;
      options.limit = CLAUDE_CAPACITY_READ_LIMIT + 1;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (++scanned > CLAUDE_CAPACITY_READ_LIMIT) {
          truncated = true;
          co_await iterator->return_();
          break;
        }
        std::optional<ClaudeUsageRollup> point = readClaudeUsageRollup(entry->value);
        if (!point || point->last_observed_at_ms > nowMs) throw rt::JsError("Claude usage rollup evidence is corrupt");
        ClaudeUsageRollupKeyPoint keyPoint{.account_id = point->account_id, .account_cohort_id = point->account_cohort_id};
        rt::Promise<rt::KvKey> keying(claudeUsageRollupKey(std::move(keyPoint), point->meter));
        const rt::KvKey expected = co_await keying.wait();
        if (!sameKeyJson(entry->key, expected)) throw rt::JsError("Claude usage rollup evidence is corrupt");
        points.push_back(std::move(*point));
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return ListClaudeUsageRollupsResult{.scan = "unavailable", .truncated = false, .points = {}};
    co_return ListClaudeUsageRollupsResult{.scan = "ok", .truncated = truncated, .points = std::move(points)};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityRollupPoint& value) {
    out = Writer()
            .required("v", value.v)
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("account_id", value.account_id)
            .required("account_cohort_id", value.account_cohort_id)
            .required("credential_kind", value.credential_kind)
            .orNull("workspace_id", value.workspace_id)
            .required("window", value.window)
            .orNull("reset_at_ms", value.reset_at_ms)
            .required("samples", value.samples)
            .required("available_sample_count", value.available_sample_count)
            .required("unavailable_sample_count", value.unavailable_sample_count)
            .orNull("first_source_observed_at_ms", value.first_source_observed_at_ms)
            .orNull("last_source_observed_at_ms", value.last_source_observed_at_ms)
            .required("first_sampled_at_ms", value.first_sampled_at_ms)
            .required("last_sampled_at_ms", value.last_sampled_at_ms)
            .orNull("min_used_percent", value.min_used_percent)
            .orNull("max_used_percent", value.max_used_percent)
            .orNull("last_used_percent", value.last_used_percent)
            .orNull("last_status", value.last_status)
            .done();
  }
  ClaudeCapacityRollupPoint tag_invoke(const json::value_to_tag<ClaudeCapacityRollupPoint>&, const json::value& value) {
    const Reader in(value, "ClaudeCapacityRollupPoint");
    const double version = in.required<double>("v");
    if (version != 1) in.fail("v", rt::get(value, "v"), "1");
    return {
      .v = version,
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .account_id = in.required<std::string>("account_id"),
      .account_cohort_id = in.required<std::string>("account_cohort_id"),
      .credential_kind = in.required<std::string>("credential_kind"),
      .workspace_id = in.orNull<std::string>("workspace_id"),
      .window = in.required<std::string>("window"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
      .samples = in.required<double>("samples"),
      .available_sample_count = in.required<double>("available_sample_count"),
      .unavailable_sample_count = in.required<double>("unavailable_sample_count"),
      .first_source_observed_at_ms = in.orNull<double>("first_source_observed_at_ms"),
      .last_source_observed_at_ms = in.orNull<double>("last_source_observed_at_ms"),
      .first_sampled_at_ms = in.required<double>("first_sampled_at_ms"),
      .last_sampled_at_ms = in.required<double>("last_sampled_at_ms"),
      .min_used_percent = in.orNull<double>("min_used_percent"),
      .max_used_percent = in.orNull<double>("max_used_percent"),
      .last_used_percent = in.orNull<double>("last_used_percent"),
      .last_status = in.orNull<std::string>("last_status"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ListClaudeCapacityRollupsResult& value) {
    out = Writer().required("scan", value.scan).required("truncated", value.truncated).required("points", value.points).done();
  }
  ListClaudeCapacityRollupsResult tag_invoke(const json::value_to_tag<ListClaudeCapacityRollupsResult>&, const json::value& value) {
    const Reader in(value, "ListClaudeCapacityRollupsResult");
    return {
      .scan = in.required<std::string>("scan"),
      .truncated = in.required<bool>("truncated"),
      .points = in.required<std::vector<ClaudeCapacityRollupPoint>>("points"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeUsageRollup& value) {
    out = Writer()
            .required("v", value.v)
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("account_id", value.account_id)
            .required("account_cohort_id", value.account_cohort_id)
            .required("credential_kind", value.credential_kind)
            .orNull("workspace_id", value.workspace_id)
            .required("meter", value.meter)
            .required("samples", value.samples)
            .required("first_observed_at_ms", value.first_observed_at_ms)
            .required("last_observed_at_ms", value.last_observed_at_ms)
            .orNull("min_used_percent", value.min_used_percent)
            .orNull("max_used_percent", value.max_used_percent)
            .done();
  }
  ClaudeUsageRollup tag_invoke(const json::value_to_tag<ClaudeUsageRollup>&, const json::value& value) {
    const Reader in(value, "ClaudeUsageRollup");
    const double version = in.required<double>("v");
    if (version != 1) in.fail("v", rt::get(value, "v"), "1");
    return {
      .v = version,
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .account_id = in.required<std::string>("account_id"),
      .account_cohort_id = in.required<std::string>("account_cohort_id"),
      .credential_kind = in.required<std::string>("credential_kind"),
      .workspace_id = in.orNull<std::string>("workspace_id"),
      .meter = in.required<json::object>("meter"),
      .samples = in.required<double>("samples"),
      .first_observed_at_ms = in.required<double>("first_observed_at_ms"),
      .last_observed_at_ms = in.required<double>("last_observed_at_ms"),
      .min_used_percent = in.orNull<double>("min_used_percent"),
      .max_used_percent = in.orNull<double>("max_used_percent"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ListClaudeUsageRollupsResult& value) {
    out = Writer().required("scan", value.scan).required("truncated", value.truncated).required("points", value.points).done();
  }
  ListClaudeUsageRollupsResult tag_invoke(const json::value_to_tag<ListClaudeUsageRollupsResult>&, const json::value& value) {
    const Reader in(value, "ListClaudeUsageRollupsResult");
    return {
      .scan = in.required<std::string>("scan"),
      .truncated = in.required<bool>("truncated"),
      .points = in.required<std::vector<ClaudeUsageRollup>>("points"),
    };
  }
}
