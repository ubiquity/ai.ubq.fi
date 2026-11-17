#include "uos/provider/claude_code_usage_schema.hpp"

#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace uos::provider::claude_code_usage_schema {
  namespace {
    using utils::isRecord;

    // `value.key` on an unknown value: arrays and other non-objects have no such member.
    const json::value* member(const json::value* value, std::string_view key) { return value != nullptr ? rt::get(*value, key) : nullptr; }

    bool finite(const json::value* value) { return value != nullptr && rt::Number::isFinite(*value); }
    bool nullableNumber(const json::value* value) { return (value != nullptr && value->is_null()) || finite(value); }
    bool nullableString(const json::value* value) { return value != nullptr && (value->is_null() || value->is_string()); }
    bool optionalString(const json::value* value) { return value == nullptr || nullableString(value); }
    bool isString(const json::value* value) { return value != nullptr && value->is_string(); }

    bool validWindow(const json::value* value) {
      return isRecord(value) && nullableNumber(member(value, "utilization")) && nullableString(member(value, "resets_at"));
    }

    bool validScope(const json::value* value) {
      if (value == nullptr || value->is_null()) return true;
      if (!isRecord(value)) return false;
      for (const json::value* scope : {member(value, "model"), member(value, "surface")}) {
        if (!(scope == nullptr || scope->is_null() || (isRecord(scope) && isString(member(scope, "display_name"))))) return false;
      }
      return true;
    }

    bool validLimit(const json::value& value) {
      if (!isRecord(value)) return false;
      for (const char* key : {"kind", "group", "severity"})
        if (!isString(member(&value, key))) return false;
      const json::value* isActive = member(&value, "is_active");
      return finite(member(&value, "percent")) && nullableString(member(&value, "resets_at")) && isActive != nullptr && isActive->is_bool() &&
             validScope(member(&value, "scope"));
    }

    bool validExtraUsage(const json::value* value) {
      if (!isRecord(value)) return false;
      const json::value* isEnabled = member(value, "is_enabled");
      if (isEnabled == nullptr || !isEnabled->is_bool()) return false;
      for (const char* key : {"monthly_limit", "used_credits", "utilization"})
        if (!nullableNumber(member(value, key))) return false;
      for (const char* key : {"currency", "disabled_reason"})
        if (!optionalString(member(value, key))) return false;
      return true;
    }

    bool validCredit(const json::value* value) {
      if (!validWindow(value) || !isRecord(value)) return false;
      if (!optionalString(member(value, "label"))) return false;
      for (const char* key : {"limit_dollars", "used_dollars", "remaining_dollars"}) {
        const json::value* item = member(value, key);
        if (!(item == nullptr || nullableNumber(item))) return false;
      }
      const json::value* periods = member(value, "periods");
      if (periods == nullptr || periods->is_null()) return true;
      if (!periods->is_array()) return false;
      return std::all_of(periods->get_array().begin(), periods->get_array().end(), [](const json::value& period) {
        return isRecord(period) && isString(member(&period, "starts_at")) && isString(member(&period, "ends_at"));
      });
    }

    constexpr std::array<std::string_view, 9> recognized = {"five_hour", "seven_day", "seven_day_oauth_apps", "seven_day_opus", "seven_day_sonnet",
                                                           "limits",    "extra_usage", "cinder_cove",       "wattle_ember"};

    bool validPayload(const json::value* value) {
      if (!isRecord(value) || !value->is_object()) return false;
      const json::object& object = value->get_object();
      if (!std::any_of(recognized.begin(), recognized.end(), [&object](std::string_view key) { return rt::hasOwn(object, key); })) return false;
      for (const std::string_view key : CLAUDE_USAGE_LEGACY_WINDOWS) {
        const json::value* window = rt::get(object, key);
        if (window != nullptr && !window->is_null() && !validWindow(window)) return false;
      }
      const json::value* limits = rt::get(object, "limits");
      if (limits != nullptr && !limits->is_null() &&
          (!limits->is_array() || !std::all_of(limits->get_array().begin(), limits->get_array().end(), [](const json::value& limit) { return validLimit(limit); })))
        return false;
      const json::value* extraUsage = rt::get(object, "extra_usage");
      if (extraUsage != nullptr && !extraUsage->is_null() && !validExtraUsage(extraUsage)) return false;
      const json::value* cinderCove = rt::get(object, "cinder_cove");
      if (cinderCove != nullptr && !cinderCove->is_null() && !validWindow(cinderCove)) return false;
      const json::value* wattleEmber = rt::get(object, "wattle_ember");
      if (wattleEmber != nullptr && !wattleEmber->is_null() && !validCredit(wattleEmber)) return false;
      return true;
    }

    json::value resetTime(const json::value& value) {
      const double parsed = value.is_null() ? rt::Number::NaN : rt::Date::parse(std::string_view(value.get_string()));
      return rt::Number::isSafeInteger(parsed) && parsed > 0 ? rt::makeNumber(parsed) : json::value(nullptr);
    }

    std::string legacyLabel(std::string_view key) {
      if (key == "five_hour") return "5-hour window";
      if (key == "seven_day") return "Weekly window";
      if (key == "seven_day_oauth_apps") return "Weekly OAuth apps";
      if (key == "seven_day_opus") return "Weekly Opus";
      return "Weekly Sonnet";
    }

    json::array metersForPayload(const json::object& payload, double at) {
      json::array meters;
      if (const json::array* limits = rt::getArray(payload, "limits")) {
        rt::OrderedMap<std::string, double> occurrences;
        for (const json::value& rowValue : *limits) {
          const json::object& row = rowValue.get_object();
          const json::value* rawScope = rt::get(row, "scope");
          const json::value scope = rawScope != nullptr && !rawScope->is_null() ? *rawScope : json::value(nullptr);
          const std::string tuple = rt::jsonStringify(json::array{*rt::get(row, "kind"), *rt::get(row, "group"), scope});
          const double* seen = occurrences.get(tuple);
          const double occurrence = seen != nullptr ? *seen : 0;
          occurrences.set(tuple, occurrence + 1);
          std::string label = std::string(rt::get(row, "group")->get_string()) + " · " + std::string(rt::get(row, "kind")->get_string());
          for (const char* key : {"model", "surface"}) {
            const json::value* displayName = member(member(&scope, key), "display_name");
            if (isString(displayName)) label += " · " + std::string(displayName->get_string());
          }
          json::object meter;
          rt::objectDefine(meter, "id", json::value("limits:" + tuple + ":" + rt::numberToString(occurrence)));
          rt::objectDefine(meter, "label", json::value(label));
          rt::objectDefine(meter, "kind", *rt::get(row, "kind"));
          rt::objectDefine(meter, "group", *rt::get(row, "group"));
          rt::objectDefine(meter, "scope", scope);
          rt::objectDefine(meter, "severity", *rt::get(row, "severity"));
          rt::objectDefine(meter, "is_active", *rt::get(row, "is_active"));
          rt::objectDefine(meter, "used_percent", *rt::get(row, "percent"));
          rt::objectDefine(meter, "resets_at", *rt::get(row, "resets_at"));
          rt::objectDefine(meter, "reset_at_ms", resetTime(*rt::get(row, "resets_at")));
          rt::objectDefine(meter, "observed_at_ms", rt::makeNumber(at));
          meters.push_back(std::move(meter));
        }
        return meters;
      }
      for (const std::string_view key : CLAUDE_USAGE_LEGACY_WINDOWS) {
        const json::value* row = rt::get(payload, key);
        if (!rt::truthy(row)) continue;
        json::object meter;
        rt::objectDefine(meter, "id", json::value("legacy:" + std::string(key)));
        rt::objectDefine(meter, "label", json::value(legacyLabel(key)));
        rt::objectDefine(meter, "kind", json::value(json::string(key)));
        rt::objectDefine(meter, "group", json::value(key == "five_hour" ? "session" : "weekly"));
        rt::objectDefine(meter, "scope", nullptr);
        rt::objectDefine(meter, "severity", nullptr);
        rt::objectDefine(meter, "is_active", false);
        rt::objectDefine(meter, "used_percent", *rt::get(*row, "utilization"));
        rt::objectDefine(meter, "resets_at", *rt::get(*row, "resets_at"));
        rt::objectDefine(meter, "reset_at_ms", resetTime(*rt::get(*row, "resets_at")));
        rt::objectDefine(meter, "observed_at_ms", rt::makeNumber(at));
        meters.push_back(std::move(meter));
      }
      return meters;
    }

    bool safePositiveInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      return number && rt::Number::isSafeInteger(*number) && *number > 0;
    }
  }

  std::optional<ClaudeCodeUsageSnapshot> parseClaudeCodeUsageSnapshot(const json::value* payload, double observedAtMs) {
    if (!rt::Number::isSafeInteger(observedAtMs) || observedAtMs <= 0 || !validPayload(payload)) return std::nullopt;
    json::object snapshot;
    rt::objectDefine(snapshot, "v", 1);
    rt::objectDefine(snapshot, "observed_at_ms", rt::makeNumber(observedAtMs));
    rt::objectDefine(snapshot, "payload", *payload);
    rt::objectDefine(snapshot, "meters", metersForPayload(payload->get_object(), observedAtMs));
    return snapshot;
  }

  std::optional<ClaudeCodeUsageSnapshot> parseClaudeCodeUsageSnapshot(const json::value& payload, double observedAtMs) {
    return parseClaudeCodeUsageSnapshot(&payload, observedAtMs);
  }

  std::optional<ClaudeCodeUsageSnapshot> readClaudeCodeUsageSnapshot(const json::value* value) {
    const json::value* version = member(value, "v");
    const std::optional<double> observedAtMs = rt::getNumber(member(value, "observed_at_ms"));
    if (!isRecord(value) || version == nullptr || !rt::isNumber(*version) || *rt::getNumber(*version) != 1 || !observedAtMs) return std::nullopt;
    return parseClaudeCodeUsageSnapshot(member(value, "payload"), *observedAtMs);
  }

  std::optional<ClaudeCodeUsageSnapshot> readClaudeCodeUsageSnapshot(const json::value& value) { return readClaudeCodeUsageSnapshot(&value); }

  std::optional<ClaudeCodeUsageMeter> readClaudeCodeUsageMeter(const json::value* value) {
    if (!isRecord(value)) return std::nullopt;
    for (const char* key : {"id", "label", "kind", "group"})
      if (!isString(member(value, key))) return std::nullopt;
    const json::value* isActive = member(value, "is_active");
    if (!nullableNumber(member(value, "used_percent")) || !nullableString(member(value, "resets_at")) || !nullableString(member(value, "severity")) ||
        isActive == nullptr || !isActive->is_bool() || !validScope(member(value, "scope")))
      return std::nullopt;
    if (!safePositiveInteger(member(value, "observed_at_ms"))) return std::nullopt;
    const json::value* resetAtMs = member(value, "reset_at_ms");
    if (!(resetAtMs != nullptr && resetAtMs->is_null()) && !safePositiveInteger(resetAtMs)) return std::nullopt;
    return value->get_object();
  }

  std::optional<ClaudeCodeUsageMeter> readClaudeCodeUsageMeter(const json::value& value) { return readClaudeCodeUsageMeter(&value); }
}
