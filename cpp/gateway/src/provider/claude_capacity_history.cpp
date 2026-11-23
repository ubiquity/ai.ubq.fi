#include "uos/provider/claude_capacity_history.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/provider/claude_code_usage_schema.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <array>
#include <unordered_set>
#include <utility>

namespace uos::provider::claude_capacity_history {
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
    using capacity_contract::ClaudeCapacityHistoryView;
    using capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
    using capacity_contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS;
    using capacity_contract::PROVIDER_CAPACITY_SOURCE_STALE_MS;
    using capacity_contract::ProviderCapacityClaudeWindow;
    using claude_capacity_rollups::claudeCapacityIdentity;
    using claude_capacity_rollups::claudeCapacityOptionalTimestamp;
    using claude_capacity_rollups::claudeCapacityPercent;
    using claude_capacity_rollups::claudeCapacityTimestamp;
    using claude_code_quota::ClaudeCodeAcceptedObservation;

    constexpr double CLAUDE_CAPACITY_MAX_BUCKET_POINTS = 6;
    constexpr std::array<std::string_view, 2> WINDOWS = {"five_hour", "seven_day"};

    // `type StoredBucket`: the value exactly as stored (`readBucket` returns the stored value
    // itself), its points both as stored and as read.
    struct StoredBucket {
      double bucket_start_at_ms;
      std::string account_id;
      std::string account_cohort_id;
      std::vector<rt::KvValue> rawPoints;
      std::vector<ClaudeCapacityHistoryPoint> points;
      rt::KvObject flags;
    };

    // `typeof value === "object" && value !== null` (utils.isRecord) on a stored value.
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }
    std::optional<double> numberOrNull(const rt::KvValue& value) { return value.isNull() ? std::nullopt : std::optional<double>(value.asNumber()); }
    std::optional<std::string> stringOrNull(const rt::KvValue& value) { return value.isNull() ? std::nullopt : std::optional<std::string>(value.asString()); }

    // A stored value as the JSON readers of claude-code-usage-schema see it: undefined members are
    // absent and every value JSON cannot hold (bigint, Map, Date, ...) reads as an empty object.

    double historyBucket(double at) { return rt::Math::floor(at / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS; }

    // `JSON.stringify(actual) === JSON.stringify(expected)` for an expected key of strings and
    // finite numbers (see claude_capacity_rollups.cpp): any other part is a mismatch, which every
    // caller treats as the TS treats both a mismatch and JSON.stringify's BigInt TypeError.
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

    std::optional<ProviderCapacityClaudeWindow> readWindow(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || (!value["status"].isNull() && !value["status"].isString())) return std::nullopt;
      if (!claudeCapacityPercent(value["used_percent"]) || !claudeCapacityOptionalTimestamp(value["reset_at_ms"]) ||
          !claudeCapacityOptionalTimestamp(value["observed_at_ms"]))
        return std::nullopt;
      return ProviderCapacityClaudeWindow{
        .status = stringOrNull(value["status"]),
        .used_percent = numberOrNull(value["used_percent"]),
        .reset_at_ms = numberOrNull(value["reset_at_ms"]),
        .observed_at_ms = numberOrNull(value["observed_at_ms"]),
      };
    }

    bool validPointIdentity(const rt::KvValue& value) {
      return claudeCapacityIdentity(value["point_id"]) && claudeCapacityIdentity(value["account_id"]) && claudeCapacityIdentity(value["account_cohort_id"]) &&
             (isText(value["credential_kind"], "oauth") || isText(value["credential_kind"], "api_key")) &&
             (value["workspace_id"].isNull() || claudeCapacityIdentity(value["workspace_id"])) &&
             (isText(value["state"], "available") || isText(value["state"], "unavailable"));
    }

    bool validPointTimes(const rt::KvValue& value) {
      if (!claudeCapacityTimestamp(value["bucket_start_at_ms"]) || !claudeCapacityTimestamp(value["sampled_at_ms"]) ||
          !claudeCapacityOptionalTimestamp(value["source_observed_at_ms"]))
        return false;
      const double sampledAtMs = value["sampled_at_ms"].asNumber();
      if (!value["source_observed_at_ms"].isNull() && value["source_observed_at_ms"].asNumber() > sampledAtMs) return false;
      const double at = value["source_observed_at_ms"].isNull() ? sampledAtMs : value["source_observed_at_ms"].asNumber();
      return historyBucket(at) == value["bucket_start_at_ms"].asNumber();
    }

    bool validStandard(const rt::KvValue& value, double sampledAtMs) {
      if (!rt::kvIsObject(value) || !value["family"].isString()) return false;
      const std::string& family = value["family"].asString();
      if (family != "requests" && family != "input-tokens" && family != "output-tokens") return false;
      for (const char* key : {"limit", "remaining"}) {
        const rt::KvValue& item = value[key];
        if (!(item.isNull() || (item.isNumber() && rt::Number::isFinite(item.asNumber()) && item.asNumber() >= 0))) return false;
      }
      if (!claudeCapacityOptionalTimestamp(value["reset_at_ms"]) || !claudeCapacityOptionalTimestamp(value["observed_at_ms"])) return false;
      return value["observed_at_ms"].isNull() || value["observed_at_ms"].asNumber() <= sampledAtMs;
    }

    bool validPointWindows(const rt::KvValue& value) {
      const rt::KvValue& windows = value["windows"];
      const rt::KvValue& standard = value["standard"];
      if (!rt::kvIsObject(windows) || !standard.isArray() || standard.asArray().size() > 3 || !claudeCapacityTimestamp(value["sampled_at_ms"])) return false;
      const double sampledAtMs = value["sampled_at_ms"].asNumber();
      for (const std::string_view name : WINDOWS) {
        if (windows[name].isNull()) continue;
        const std::optional<ProviderCapacityClaudeWindow> window = readWindow(windows[name]);
        if (!window || (window->observed_at_ms && *window->observed_at_ms > sampledAtMs)) return false;
      }
      // `standard.every(...)` skips array holes.
      for (const rt::KvValue& item : standard.asArray())
        if (!item.is<rt::KvHole>() && !validStandard(item, sampledAtMs)) return false;
      return true;
    }

    rt::KvKey historyKey(double bucketStartAtMs, const std::string& accountId, const std::string& accountCohortId) {
      rt::KvKey key = CLAUDE_CAPACITY_HISTORY_PREFIX;
      key.emplace_back(bucketStartAtMs);
      key.emplace_back(accountId);
      key.emplace_back(accountCohortId);
      return key;
    }

    // The stored form of a point the module builds, in the order pointForObservation's object
    // literal writes its members.
    json::value pointValue(const ClaudeCapacityHistoryPoint& point) {
      const auto nullableNumber = [](const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); };
      const auto window = [&](const std::optional<ProviderCapacityClaudeWindow>& value) -> json::value {
        if (!value) return nullptr;
        json::object out;
        out["status"] = value->status ? json::value(json::string(*value->status)) : json::value(nullptr);
        out["used_percent"] = nullableNumber(value->used_percent);
        out["reset_at_ms"] = nullableNumber(value->reset_at_ms);
        out["observed_at_ms"] = nullableNumber(value->observed_at_ms);
        return out;
      };
      json::object out;
      out["v"] = rt::makeNumber(point.v);
      out["point_id"] = json::string(point.point_id);
      out["bucket_start_at_ms"] = rt::makeNumber(point.bucket_start_at_ms);
      out["sampled_at_ms"] = rt::makeNumber(point.sampled_at_ms);
      out["source_observed_at_ms"] = nullableNumber(point.source_observed_at_ms);
      out["account_id"] = json::string(point.account_id);
      out["account_cohort_id"] = json::string(point.account_cohort_id);
      out["credential_kind"] = json::string(point.credential_kind);
      out["workspace_id"] = point.workspace_id ? json::value(json::string(*point.workspace_id)) : json::value(nullptr);
      out["state"] = json::string(point.state);
      out["response_status"] = nullableNumber(point.response_status);
      if (point.source) out["source"] = json::string(*point.source);
      json::object windows;
      windows["five_hour"] = window(point.windows.five_hour);
      windows["seven_day"] = window(point.windows.seven_day);
      out["windows"] = std::move(windows);
      if (point.usage_snapshot) out["usage_snapshot"] = rt::canonicalNumbers(json::value(*point.usage_snapshot));
      json::array standard;
      for (const ClaudeCapacityHistoryPoint::StandardLimit& item : point.standard) {
        json::object limit;
        limit["family"] = json::string(item.family);
        limit["limit"] = nullableNumber(item.limit);
        limit["remaining"] = nullableNumber(item.remaining);
        limit["reset_at_ms"] = nullableNumber(item.reset_at_ms);
        limit["observed_at_ms"] = nullableNumber(item.observed_at_ms);
        standard.emplace_back(std::move(limit));
      }
      out["standard"] = std::move(standard);
      return out;
    }

    std::optional<StoredBucket> readBucket(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || !(value["v"].isNumber() && value["v"].asNumber() == 1) || !value["points"].isArray() ||
          static_cast<double>(value["points"].asArray().size()) > CLAUDE_CAPACITY_MAX_BUCKET_POINTS || !rt::kvIsObject(value["flags"]))
        return std::nullopt;
      StoredBucket bucket;
      for (const rt::KvValue& raw : value["points"].asArray()) {
        // `points.map(read).filter(...)` drops holes and invalid points; either makes the counts differ.
        if (raw.is<rt::KvHole>()) return std::nullopt;
        std::optional<ClaudeCapacityHistoryPoint> point = readClaudeCapacityHistoryPoint(raw);
        if (!point) return std::nullopt;
        bucket.rawPoints.push_back(raw);
        bucket.points.push_back(std::move(*point));
      }
      if (!claudeCapacityTimestamp(value["bucket_start_at_ms"]) || !claudeCapacityIdentity(value["account_id"]) || !claudeCapacityIdentity(value["account_cohort_id"]))
        return std::nullopt;
      bucket.bucket_start_at_ms = value["bucket_start_at_ms"].asNumber();
      bucket.account_id = value["account_id"].asString();
      bucket.account_cohort_id = value["account_cohort_id"].asString();
      std::unordered_set<std::string> ids;
      for (const ClaudeCapacityHistoryPoint& point : bucket.points) {
        if (point.bucket_start_at_ms != bucket.bucket_start_at_ms || point.account_id != bucket.account_id || point.account_cohort_id != bucket.account_cohort_id)
          return std::nullopt;
        ids.insert(point.point_id);
      }
      if (ids.size() != bucket.points.size()) return std::nullopt;
      const rt::KvValue& flags = value["flags"];
      const rt::KvValue& normal = flags["normal"];
      if (!normal.isNumber() || !(normal.asNumber() == 0 || normal.asNumber() == 1 || normal.asNumber() == 2)) return std::nullopt;
      for (const char* key : {"unavailable", "recovery", "five_hour", "seven_day"})
        if (!flags[key].isBool()) return std::nullopt;
      // Only a plain object can carry those members.
      if (!flags.isObject()) return std::nullopt;
      bucket.flags = flags.asObject();
      return bucket;
    }

    rt::Task<std::optional<ClaudeCapacityHistoryPoint>> pointForObservation(ClaudeCodeAcceptedObservation event) {
      if (!event.observation.usage_snapshot && event.observation.windows.size() == 0 && event.observation.standard.size() == 0) co_return std::nullopt;
      const auto window = [&event](std::string_view name) -> std::optional<ProviderCapacityClaudeWindow> {
        const claude_code_quota::ClaudeCodeQuotaWindow* input = event.observation.windows.get(name);
        if (input == nullptr) return std::nullopt;
        return ProviderCapacityClaudeWindow{
          .status = input->status,
          .used_percent = claudeCapacityUsedPercent(input->utilization),
          .reset_at_ms = input->reset_at_ms,
          .observed_at_ms = claudeCapacityTimestamp(rt::KvValue(input->observed_at_ms)) ? std::optional<double>(input->observed_at_ms) : std::nullopt,
        };
      };
      const std::optional<double> sourceTime =
        event.source_observed_at_ms && claudeCapacityOptionalTimestamp(rt::KvValue(*event.source_observed_at_ms)) ? event.source_observed_at_ms
                                                                                                                   : std::optional<double>();
      // Only the safe event enters this digest, never a credential or its internal fingerprint.
      json::object digestInput;
      digestInput["account"] = json::string(event.account_id);
      digestInput["cohort"] = json::string(event.account_cohort_id);
      digestInput["kind"] = json::string(event.credential_kind);
      digestInput["workspace"] = event.workspace_id ? json::value(json::string(*event.workspace_id)) : json::value(nullptr);
      digestInput["state"] = json::string(event.state);
      digestInput["response_status"] = event.response_status ? rt::makeNumber(*event.response_status) : json::value(nullptr);
      digestInput["observation"] = json::value_from(event.observation);
      if (event.source && !event.source->empty()) digestInput["source"] = json::string(*event.source);
      rt::Promise<std::string> hashing(utils::sha256Hex(rt::jsonStringify(json::value(std::move(digestInput)))));
      const std::string pointId = co_await hashing.wait();
      ClaudeCapacityHistoryPoint point{
        .v = 1,
        .point_id = pointId,
        .bucket_start_at_ms = historyBucket(sourceTime.value_or(event.sampled_at_ms)),
        .sampled_at_ms = event.sampled_at_ms,
        .source_observed_at_ms = sourceTime,
        .account_id = event.account_id,
        .account_cohort_id = event.account_cohort_id,
        .credential_kind = event.credential_kind,
        .workspace_id = event.workspace_id,
        .state = event.state,
        .response_status = event.response_status,
        .source = event.source && !event.source->empty() ? event.source : std::nullopt,
        .windows = {.five_hour = window("5h"), .seven_day = window("7d")},
        .usage_snapshot = event.observation.usage_snapshot,
        .standard = {},
      };
      for (const std::string& family : event.observation.standard.keys()) {
        const claude_code_quota::ClaudeCodeStandardWindow& item = *event.observation.standard.get(family);
        ClaudeCapacityHistoryPoint::StandardLimit limit;
        limit.family = family;
        limit.limit = item.limit;
        limit.remaining = item.remaining;
        limit.reset_at_ms = item.reset_at_ms;
        limit.observed_at_ms = item.observed_at_ms;
        point.standard.push_back(std::move(limit));
      }
      co_return readClaudeCapacityHistoryPoint(rt::KvValue(pointValue(point)));
    }

    // `snapshot?.meters.map((meter) => [meter.id, meter.resets_at])` serialized; std::nullopt is
    // JSON.stringify(undefined).
    std::optional<std::string> anchors(const json::value* snapshot) {
      if (snapshot == nullptr || rt::isNullish(snapshot)) return std::nullopt;
      const json::value* meters = rt::get(*snapshot, "meters");
      if (meters == nullptr || !meters->is_array()) throw rt::TypeError("snapshot?.meters.map is not a function");
      json::array out;
      for (const json::value& meter : meters->get_array()) {
        if (meter.is_null()) throw rt::TypeError("null is not an object (evaluating 'meter.id')");
        json::array pair;
        const json::value* id = rt::get(meter, "id");
        const json::value* resetsAt = rt::get(meter, "resets_at");
        pair.push_back(id != nullptr ? *id : json::value(nullptr));
        pair.push_back(resetsAt != nullptr ? *resetsAt : json::value(nullptr));
        out.emplace_back(std::move(pair));
      }
      return rt::jsonStringify(json::value(std::move(out)));
    }

    bool retainUsageTransition(const std::optional<StoredBucket>& previous, const ClaudeCapacityHistoryPoint& point, rt::KvObject& flags) {
      if (!point.usage_snapshot || flags.at("usage").truthy()) return false;
      std::optional<json::value> prior;
      if (previous)
        for (std::size_t index = previous->rawPoints.size(); index-- > 0;) {
          const rt::KvValue& stored = previous->rawPoints[index]["usage_snapshot"];
          if (stored.truthy()) {
            prior = rt::storedJsonView(stored, STORED_JSON_VIEW_OPTIONS);
            break;
          }
        }
      const json::value current(*point.usage_snapshot);
      if (anchors(prior ? &*prior : nullptr) == anchors(&current)) return false;
      flags.set("usage", true);
      return true;
    }

    double sortTime(const ClaudeCapacityHistoryPoint& point) { return point.source_observed_at_ms.value_or(point.sampled_at_ms); }

    std::optional<rt::KvValue> retainPoint(const std::optional<StoredBucket>& previous, const ClaudeCapacityHistoryPoint& point) {
      if (previous) {
        for (const ClaudeCapacityHistoryPoint& entry : previous->points)
          if (entry.point_id == point.point_id) return std::nullopt;
        if (static_cast<double>(previous->points.size()) >= CLAUDE_CAPACITY_MAX_BUCKET_POINTS) return std::nullopt;
      }
      rt::KvObject flags = previous ? previous->flags
                                    : rt::KvObject{{"normal", rt::KvValue(0)},
                                                   {"unavailable", rt::KvValue(false)},
                                                   {"recovery", rt::KvValue(false)},
                                                   {"five_hour", rt::KvValue(false)},
                                                   {"seven_day", rt::KvValue(false)}};
      const ClaudeCapacityHistoryPoint* latest = previous && !previous->points.empty() ? &previous->points.back() : nullptr;
      const double normal = flags.at("normal").isNumber() ? flags.at("normal").asNumber() : rt::Number::NaN;
      bool retain = normal < 2;
      if (retain) flags.set("normal", rt::KvValue(normal + 1));
      if (latest != nullptr && latest->state != point.state) {
        const std::string flag = point.state == "unavailable" ? "unavailable" : "recovery";
        if (!flags.at(flag).truthy()) {
          flags.set(flag, true);
          retain = true;
        }
      }
      for (const std::string_view name : WINDOWS) {
        const auto windowOf = [name](const ClaudeCapacityHistoryPoint& entry) -> const std::optional<ProviderCapacityClaudeWindow>& {
          return name == "five_hour" ? entry.windows.five_hour : entry.windows.seven_day;
        };
        const ProviderCapacityClaudeWindow* before = nullptr;
        if (previous)
          for (std::size_t index = previous->points.size(); index-- > 0;)
            if (windowOf(previous->points[index])) {
              before = &*windowOf(previous->points[index]);
              break;
            }
        const std::optional<ProviderCapacityClaudeWindow>& after = windowOf(point);
        // `before?.reset_at_ms !== after.reset_at_ms`: undefined differs from every reset.
        if (after && (before == nullptr || before->reset_at_ms != after->reset_at_ms) && !flags.at(name).truthy() && !retain) {
          flags.set(std::string(name), true);
          retain = true;
        }
      }
      if (!retain) retain = retainUsageTransition(previous, point, flags);
      if (!retain) return std::nullopt;
      std::vector<std::pair<rt::KvValue, ClaudeCapacityHistoryPoint>> entries;
      if (previous)
        for (std::size_t index = 0; index < previous->points.size(); ++index) entries.emplace_back(previous->rawPoints[index], previous->points[index]);
      entries.emplace_back(rt::KvValue(pointValue(point)), point);
      rt::sort(entries, [](const auto& a, const auto& b) {
        const double byTime = sortTime(a.second) - sortTime(b.second);
        return byTime != 0 && !rt::Number::isNaN(byTime) ? byTime : a.second.sampled_at_ms - b.second.sampled_at_ms;
      });
      rt::KvArray points;
      for (auto& [raw, typed] : entries) points.push_back(std::move(raw));
      rt::KvObject bucket;
      bucket.set("v", rt::KvValue(1));
      bucket.set("bucket_start_at_ms", rt::KvValue(point.bucket_start_at_ms));
      bucket.set("account_id", rt::KvValue(point.account_id));
      bucket.set("account_cohort_id", rt::KvValue(point.account_cohort_id));
      bucket.set("points", rt::KvValue(std::move(points)));
      bucket.set("flags", rt::KvValue(std::move(flags)));
      return rt::KvValue(std::move(bucket));
    }

    struct RollupRead {
      std::string window;
      rt::KvEntryMaybe<> entry;
    };
    struct UsageRollupRead {
      claude_code_usage_schema::ClaudeCodeUsageMeter meter;
      rt::KvEntryMaybe<> entry;
    };

    void attachRollups(rt::AtomicOperation& operation, const ClaudeCapacityHistoryPoint& point, const std::vector<RollupRead>& rollups,
                       const std::vector<UsageRollupRead>& usageRollups) {
      for (const RollupRead& rollup : rollups) {
        const std::optional<claude_capacity_rollups::ClaudeCapacityRollupPoint> stored = claude_capacity_rollups::readClaudeCapacityRollup(rollup.entry.value);
        if (!rollup.entry.value.isNull() && !stored) throw rt::JsError("Claude rollup is invalid");
        const claude_capacity_rollups::ClaudeCapacityRollupPoint merged = claude_capacity_rollups::mergeClaudeCapacityRollup(stored, point, rollup.window);
        operation.check(rollup.entry).set(rollup.entry.key, rt::KvValue(json::value_from(merged)));
      }
      for (const UsageRollupRead& rollup : usageRollups) {
        const std::optional<claude_capacity_rollups::ClaudeUsageRollup> stored = claude_capacity_rollups::readClaudeUsageRollup(rollup.entry.value);
        if (!rollup.entry.value.isNull() && !stored) throw rt::JsError("Claude usage rollup is invalid");
        const claude_capacity_rollups::ClaudeUsageRollup merged = claude_capacity_rollups::mergeClaudeUsageRollup(stored, point, rollup.meter);
        operation.check(rollup.entry).set(rollup.entry.key, rt::KvValue(json::value_from(merged)));
      }
    }

    // `kv.get(claudeCapacityRollupKey(point, window), ...)` of one Promise.all member.
    rt::Task<RollupRead> readRollup(std::shared_ptr<rt::Kv> kv, ClaudeCapacityHistoryPoint point, std::string window) {
      const rt::KvKey key = claude_capacity_rollups::claudeCapacityRollupKey(point, window);
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
      co_return RollupRead{.window = std::move(window), .entry = std::move(entry)};
    }

    rt::Task<UsageRollupRead> readUsageRollup(std::shared_ptr<rt::Kv> kv, ClaudeCapacityHistoryPoint point, claude_code_usage_schema::ClaudeCodeUsageMeter meter) {
      claude_capacity_rollups::ClaudeUsageRollupKeyPoint keyPoint{.account_id = point.account_id, .account_cohort_id = point.account_cohort_id};
      rt::Promise<rt::KvKey> keying(claude_capacity_rollups::claudeUsageRollupKey(std::move(keyPoint), meter));
      const rt::KvKey key = co_await keying.wait();
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
      co_return UsageRollupRead{.meter = std::move(meter), .entry = std::move(entry)};
    }

    std::optional<StoredBucket> bucketForEntry(const rt::KvEntry<>& entry, double first, double end) {
      for (std::size_t index = 0; index < CLAUDE_CAPACITY_HISTORY_PREFIX.size(); ++index)
        if (index >= entry.key.size() || !(entry.key[index] == CLAUDE_CAPACITY_HISTORY_PREFIX[index])) return std::nullopt;
      if (entry.key.size() <= CLAUDE_CAPACITY_HISTORY_PREFIX.size()) return std::nullopt;
      const rt::KvKeyPart& keyBucket = entry.key[CLAUDE_CAPACITY_HISTORY_PREFIX.size()];
      if (!keyBucket.isNumber() || keyBucket.number() < first || keyBucket.number() >= end) return std::nullopt;
      std::optional<StoredBucket> bucket = readBucket(entry.value);
      if (!bucket || !sameKeyJson(entry.key, historyKey(bucket->bucket_start_at_ms, bucket->account_id, bucket->account_cohort_id)))
        throw rt::JsError("Claude history evidence is corrupt");
      return bucket;
    }
  }

  std::optional<double> claudeCapacityUsedPercent(std::optional<double> value) {
    if (!value || !rt::Number::isFinite(*value) || *value < 0) return std::nullopt;
    return rt::Math::min(100.0, *value * 100);
  }

  std::optional<ClaudeCapacityHistoryPoint> readClaudeCapacityHistoryPoint(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !(value["v"].isNumber() && value["v"].asNumber() == 1) || !validPointIdentity(value) || !validPointTimes(value) ||
        !validPointWindows(value))
      return std::nullopt;
    if (!value["source"].isUndefined() && !isText(value["source"], "response_headers")) return std::nullopt;
    std::optional<claude_code_usage_schema::ClaudeCodeUsageSnapshot> snapshot;
    if (!value["usage_snapshot"].isUndefined()) {
      snapshot = claude_code_usage_schema::readClaudeCodeUsageSnapshot(rt::storedJsonView(value["usage_snapshot"], STORED_JSON_VIEW_OPTIONS));
      if (!snapshot || rt::toNumber(rt::get(*snapshot, "observed_at_ms")) > value["sampled_at_ms"].asNumber()) return std::nullopt;
    }
    const rt::KvValue& responseStatus = value["response_status"];
    if (!responseStatus.isNull() && (!responseStatus.isNumber() || !rt::Number::isInteger(responseStatus.asNumber()) || responseStatus.asNumber() < 100 ||
                                     responseStatus.asNumber() > 599))
      return std::nullopt;
    const rt::KvValue& windows = value["windows"];
    ClaudeCapacityHistoryPoint point{
      .v = 1,
      .point_id = value["point_id"].asString(),
      .bucket_start_at_ms = value["bucket_start_at_ms"].asNumber(),
      .sampled_at_ms = value["sampled_at_ms"].asNumber(),
      .source_observed_at_ms = numberOrNull(value["source_observed_at_ms"]),
      .account_id = value["account_id"].asString(),
      .account_cohort_id = value["account_cohort_id"].asString(),
      .credential_kind = value["credential_kind"].asString(),
      .workspace_id = stringOrNull(value["workspace_id"]),
      .state = value["state"].asString(),
      .response_status = numberOrNull(responseStatus),
      .source = value["source"].isUndefined() ? std::nullopt : std::optional<std::string>(value["source"].asString()),
      .windows = {.five_hour = windows["five_hour"].isNull() ? std::nullopt : readWindow(windows["five_hour"]),
                  .seven_day = windows["seven_day"].isNull() ? std::nullopt : readWindow(windows["seven_day"])},
      .usage_snapshot = snapshot,
      .standard = {},
    };
    for (const rt::KvValue& item : value["standard"].asArray()) {
      if (item.is<rt::KvHole>()) continue;
      ClaudeCapacityHistoryPoint::StandardLimit limit;
      limit.family = item["family"].asString();
      limit.limit = numberOrNull(item["limit"]);
      limit.remaining = numberOrNull(item["remaining"]);
      limit.reset_at_ms = numberOrNull(item["reset_at_ms"]);
      limit.observed_at_ms = numberOrNull(item["observed_at_ms"]);
      point.standard.push_back(std::move(limit));
    }
    return point;
  }

  rt::Task<void> recordClaudeCapacityObservation(std::shared_ptr<rt::Kv> kv, ClaudeCodeAcceptedObservation event) {
    rt::Promise<std::optional<ClaudeCapacityHistoryPoint>> building(pointForObservation(std::move(event)));
    const std::optional<ClaudeCapacityHistoryPoint> point = co_await building.wait();
    if (!point) co_return;
    const rt::KvKey key = historyKey(point->bucket_start_at_ms, point->account_id, point->account_cohort_id);
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    for (int attempt = 0; attempt < 4; attempt += 1) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
      const std::optional<StoredBucket> previous = readBucket(entry.value);
      // Corrupt evidence is not silently replaced with an apparently complete bucket.
      if (!entry.value.isNull() &&
          (!previous || !sameKeyJson(historyKey(previous->bucket_start_at_ms, previous->account_id, previous->account_cohort_id), key)))
        throw rt::JsError("Claude history bucket is invalid");
      std::optional<rt::KvValue> next = retainPoint(previous, *point);
      if (!next) co_return;
      std::vector<rt::Promise<RollupRead>> rollupReads;
      for (const std::string_view window : WINDOWS) {
        const bool present = window == "five_hour" ? point->windows.five_hour.has_value() : point->windows.seven_day.has_value();
        if (present) rollupReads.push_back(rt::Promise<RollupRead>(readRollup(kv, *point, std::string(window))));
      }
      const std::vector<RollupRead> rollups = co_await rt::all(std::move(rollupReads)).wait();
      std::vector<rt::Promise<UsageRollupRead>> usageReads;
      if (point->usage_snapshot)
        if (const json::array* meters = rt::getArray(*point->usage_snapshot, "meters"))
          for (const json::value& meter : *meters) usageReads.push_back(rt::Promise<UsageRollupRead>(readUsageRollup(kv, *point, meter.as_object())));
      const std::vector<UsageRollupRead> usageRollups = co_await rt::all(std::move(usageReads)).wait();
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      const rt::KvSetOptions expiry{
        .expireIn = rt::Math::max(1.0, point->bucket_start_at_ms + PROVIDER_CAPACITY_HISTORY_BUCKET_MS + PROVIDER_CAPACITY_HISTORY_RETENTION_MS - rt::dateNow())};
      operation->check(entry).set(key, std::move(*next), expiry);
      attachRollups(*operation, *point, rollups, usageRollups);
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) co_return;
    }
    throw rt::JsError("Claude history contention exceeded four attempts");
  }

  rt::Task<ClaudeCapacityHistoryView> readClaudeCapacityHistory(std::shared_ptr<rt::Kv> kv, double nowMs) {
    ClaudeCapacityHistoryView view{
      .v = 1,
      .bucket_ms = PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
      .retention_ms = PROVIDER_CAPACITY_HISTORY_RETENTION_MS,
      .stale_after_ms = PROVIDER_CAPACITY_SOURCE_STALE_MS,
      .scan = "unavailable",
      .truncated = false,
      .points = {},
    };
    if (!kv) co_return view;
    std::vector<ClaudeCapacityHistoryPoint> points;
    double scanned = 0;
    bool truncated = false;
    const double cutoff = rt::Math::max(0.0, nowMs - PROVIDER_CAPACITY_HISTORY_RETENTION_MS);
    const double first = historyBucket(cutoff);
    const double end = historyBucket(nowMs) + PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
    bool failed = false;
    try {
      rt::KvListSelector selector;
      selector.start = rt::KvKey{CLAUDE_CAPACITY_HISTORY_PREFIX, first};
      selector.end = rt::KvKey{CLAUDE_CAPACITY_HISTORY_PREFIX, end};
      rt::KvListOptions options;
      options.limit = claude_capacity_rollups::CLAUDE_CAPACITY_READ_LIMIT + 1;
      options.reverse = true;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (++scanned > claude_capacity_rollups::CLAUDE_CAPACITY_READ_LIMIT) {
          truncated = true;
          co_await iterator->return_();
          break;
        }
        const std::optional<StoredBucket> bucket = bucketForEntry(*entry, first, end);
        if (!bucket) continue;
        for (std::size_t index = bucket->points.size(); index-- > 0;) {
          const ClaudeCapacityHistoryPoint& point = bucket->points[index];
          if (!(point.sampled_at_ms <= nowMs && sortTime(point) >= cutoff)) continue;
          if (static_cast<double>(points.size()) >= claude_capacity_rollups::CLAUDE_CAPACITY_READ_LIMIT) {
            truncated = true;
            break;
          }
          points.push_back(point);
        }
        if (truncated) {
          co_await iterator->return_();
          break;
        }
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return view;
    rt::sort(points, [](const ClaudeCapacityHistoryPoint& a, const ClaudeCapacityHistoryPoint& b) { return sortTime(a) - sortTime(b); });
    view.scan = "ok";
    view.truncated = truncated;
    view.points = std::move(points);
    co_return view;
  }
}
