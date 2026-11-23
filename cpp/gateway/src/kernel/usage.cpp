#include "uos/kernel/usage.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <unordered_map>
#include <utility>

namespace uos::kernel::usage {
  namespace {
    using rt::record::Reader;
    using ::uos::types::KernelAuthUsageDailyRecord;
    using ::uos::types::KernelAuthUsageDay;
    using ::uos::types::KernelAuthUsageRecord;
    using ::uos::types::KernelOrgUsageDailyRecord;
    using ::uos::types::KernelOrgUsageDay;
    using ::uos::types::KernelOrgUsageRecord;

    constexpr double MAX_LABEL_LENGTH = 120;
    constexpr double DAILY_SERIES_DAYS = 30;
    constexpr double DAY_MS = 24 * 60 * 60 * 1000;

    // `isRecord(value)` (src/utils.ts) on a stored value: `typeof value === "object" && value !==
    // null`, so arrays, Maps, Sets, Dates, typed arrays and KvU64 instances count.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    // `getString(value)` (src/utils.ts); std::nullopt is null.
    std::optional<std::string> getString(const rt::KvValue& value) {
      if (!value.isString()) return std::nullopt;
      return value.asString();
    }

    double coerceNumber(const rt::KvValue& value, double fallback = 0) {
      if (!value.isNumber() || !rt::Number::isFinite(value.asNumber())) return fallback;
      return rt::Math::trunc(value.asNumber());
    }

    // std::nullopt is null.
    std::optional<std::string> normalizeLabel(const rt::KvValue& value) {
      const std::optional<std::string> raw = getString(value);
      if (!rt::truthy(raw)) return std::nullopt;
      std::string trimmed = rt::trim(*raw);
      if (trimmed.empty()) return std::nullopt;
      if (static_cast<double>(rt::length(trimmed)) > MAX_LABEL_LENGTH) return rt::slice(trimmed, 0, MAX_LABEL_LENGTH);
      return trimmed;
    }

    std::string normalizeOwnerRepo(const rt::KvValue& value, const std::string& fallback) {
      const std::optional<std::string> raw = getString(value);
      std::string trimmed = raw ? rt::trim(*raw) : std::string();
      return !trimmed.empty() ? trimmed : fallback;
    }

    std::string pad2(double value) { return rt::padStart(rt::numberToString(value), 2, "0"); }

    double startOfDayUtcMs(double ms) {
      const rt::Date date(ms);
      return rt::Date::UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate());
    }

    std::string dayKeyFromMs(double ms) {
      const rt::Date date(ms);
      return rt::numberToString(date.getUTCFullYear()) + "-" + pad2(date.getUTCMonth() + 1) + "-" + pad2(date.getUTCDate());
    }

    // std::nullopt is null. (The TS also returns null for a value that is not a string; every
    // caller passes a string.)
    std::optional<double> dayKeyToMs(const std::string& value) {
      static const rt::Regex DAY_KEY(R"(^\d{4}-\d{2}-\d{2}$)", "");
      const std::string trimmed = rt::trim(value);
      if (!DAY_KEY.test(trimmed)) return std::nullopt;
      const std::vector<std::string> parts = rt::split(trimmed, "-");
      const double year = rt::stringToNumber(parts[0]);
      const double month = rt::stringToNumber(parts[1]);
      const double day = rt::stringToNumber(parts[2]);
      if (!rt::Number::isFinite(year) || !rt::Number::isFinite(month) || !rt::Number::isFinite(day)) return std::nullopt;
      return rt::Date::UTC(year, month - 1, day);
    }

    KernelAuthUsageRecord buildBaseUsageRecord(const std::string& owner, const std::string& repo, double nowMs) {
      return KernelAuthUsageRecord{
        .owner = owner,
        .repo = repo,
        .total_requests = 0,
        .stream_requests = 0,
        .non_stream_requests = 0,
        .completed_requests = 0,
        .error_requests = 0,
        .input_tokens = 0,
        .output_tokens = 0,
        .total_tokens = 0,
        .first_seen_at_ms = nowMs,
        .last_seen_at_ms = nowMs,
        .last_model = std::nullopt,
        .last_reasoning = std::nullopt,
        .last_route = std::nullopt,
      };
    }

    KernelAuthUsageRecord normalizeUsageRecord(const rt::KvValue& value, const std::string& owner, const std::string& repo, double nowMs) {
      if (!isRecord(value)) return buildBaseUsageRecord(owner, repo, nowMs);
      return KernelAuthUsageRecord{
        .owner = normalizeOwnerRepo(value["owner"], owner),
        .repo = normalizeOwnerRepo(value["repo"], repo),
        .total_requests = coerceNumber(value["total_requests"]),
        .stream_requests = coerceNumber(value["stream_requests"]),
        .non_stream_requests = coerceNumber(value["non_stream_requests"]),
        .completed_requests = coerceNumber(value["completed_requests"]),
        .error_requests = coerceNumber(value["error_requests"]),
        .input_tokens = coerceNumber(value["input_tokens"]),
        .output_tokens = coerceNumber(value["output_tokens"]),
        .total_tokens = coerceNumber(value["total_tokens"]),
        .first_seen_at_ms = coerceNumber(value["first_seen_at_ms"], nowMs),
        .last_seen_at_ms = coerceNumber(value["last_seen_at_ms"], nowMs),
        .last_model = normalizeLabel(value["last_model"]),
        .last_reasoning = normalizeLabel(value["last_reasoning"]),
        .last_route = normalizeLabel(value["last_route"]),
      };
    }

    // std::nullopt is null.
    std::optional<KernelAuthUsageDay> normalizeDailyUsageDay(const rt::KvValue& value) {
      if (!isRecord(value)) return std::nullopt;
      const rt::KvValue& dayValue = value["day"];
      const std::string day = dayValue.isString() ? rt::trim(dayValue.asString()) : std::string();
      if (day.empty()) return std::nullopt;
      if (!dayKeyToMs(day)) return std::nullopt;
      const double requestCount = rt::Math::max(0, coerceNumber(value["request_count"], 0));
      return KernelAuthUsageDay{.day = day, .request_count = requestCount};
    }

    std::vector<KernelAuthUsageDay> normalizeDays(const rt::KvValue& value) {
      const rt::KvValue& daysRaw = value["days"];
      std::vector<KernelAuthUsageDay> days;
      if (!daysRaw.isArray()) return days;
      for (const rt::KvValue& item : daysRaw.asArray()) {
        const std::optional<KernelAuthUsageDay> normalized = normalizeDailyUsageDay(item);
        if (normalized) days.push_back(*normalized);
      }
      return days;
    }

    KernelAuthUsageDailyRecord normalizeDailyUsageRecord(const rt::KvValue& value, const std::string& owner, const std::string& repo, double nowMs) {
      if (!isRecord(value)) return KernelAuthUsageDailyRecord{.owner = owner, .repo = repo, .days = {}, .updated_at_ms = nowMs};
      std::vector<KernelAuthUsageDay> days = normalizeDays(value);
      return KernelAuthUsageDailyRecord{
        .owner = normalizeOwnerRepo(value["owner"], owner),
        .repo = normalizeOwnerRepo(value["repo"], repo),
        .days = std::move(days),
        .updated_at_ms = coerceNumber(value["updated_at_ms"], nowMs),
      };
    }

    KernelOrgUsageDailyRecord normalizeDailyOrgUsageRecord(const rt::KvValue& value, const std::string& owner, double nowMs) {
      if (!isRecord(value)) return KernelOrgUsageDailyRecord{.owner = owner, .days = {}, .updated_at_ms = nowMs};
      std::vector<KernelOrgUsageDay> days;
      for (const KernelAuthUsageDay& normalized : normalizeDays(value)) days.push_back(KernelOrgUsageDay{.day = normalized.day, .request_count = normalized.request_count});
      return KernelOrgUsageDailyRecord{
        .owner = normalizeOwnerRepo(value["owner"], owner),
        .days = std::move(days),
        .updated_at_ms = coerceNumber(value["updated_at_ms"], nowMs),
      };
    }

    // `record: { days: { day: string; request_count: number }[] }`
    template <class DailyRecord> std::vector<double> buildDailySeries(const DailyRecord& record, double nowMs, double days) {
      const double seriesDays = rt::Math::max(1, rt::Math::trunc(days));
      const double startMs = startOfDayUtcMs(nowMs) - (seriesDays - 1) * DAY_MS;
      std::unordered_map<std::string, double> countsByDay;
      for (const auto& entry : record.days) countsByDay.insert_or_assign(entry.day, entry.request_count);
      std::vector<double> series;
      for (double i = 0; i < seriesDays; i += 1) {
        const double dayMs = startMs + i * DAY_MS;
        const std::string dayKey = dayKeyFromMs(dayMs);
        const auto found = countsByDay.find(dayKey);
        series.push_back(found != countsByDay.end() ? found->second : 0);
      }
      return series;
    }

    KernelOrgUsageRecord buildBaseOrgUsageRecord(const std::string& owner, double nowMs) {
      return KernelOrgUsageRecord{
        .owner = owner,
        .total_requests = 0,
        .stream_requests = 0,
        .non_stream_requests = 0,
        .completed_requests = 0,
        .error_requests = 0,
        .input_tokens = 0,
        .output_tokens = 0,
        .total_tokens = 0,
        .first_seen_at_ms = nowMs,
        .last_seen_at_ms = nowMs,
        .last_model = std::nullopt,
        .last_reasoning = std::nullopt,
        .last_route = std::nullopt,
      };
    }

    KernelOrgUsageRecord normalizeOrgUsageRecord(const rt::KvValue& value, const std::string& owner, double nowMs) {
      if (!isRecord(value)) return buildBaseOrgUsageRecord(owner, nowMs);
      return KernelOrgUsageRecord{
        .owner = normalizeOwnerRepo(value["owner"], owner),
        .total_requests = coerceNumber(value["total_requests"]),
        .stream_requests = coerceNumber(value["stream_requests"]),
        .non_stream_requests = coerceNumber(value["non_stream_requests"]),
        .completed_requests = coerceNumber(value["completed_requests"]),
        .error_requests = coerceNumber(value["error_requests"]),
        .input_tokens = coerceNumber(value["input_tokens"]),
        .output_tokens = coerceNumber(value["output_tokens"]),
        .total_tokens = coerceNumber(value["total_tokens"]),
        .first_seen_at_ms = coerceNumber(value["first_seen_at_ms"], nowMs),
        .last_seen_at_ms = coerceNumber(value["last_seen_at_ms"], nowMs),
        .last_model = normalizeLabel(value["last_model"]),
        .last_reasoning = normalizeLabel(value["last_reasoning"]),
        .last_route = normalizeLabel(value["last_route"]),
      };
    }

    // `typeof entry.key[index] === "string" ? entry.key[index] : ""`
    std::string keyText(const rt::KvKey& key, std::size_t index) { return index < key.size() && key[index].isString() ? key[index].string() : std::string(); }
  }

  rt::KvKey kernelUsageKey(const std::string& owner, const std::string& repo) { return rt::KvKey{KERNEL_AUTH_USAGE_PREFIX, owner, repo}; }
  rt::KvKey kernelUsageDailyKey(const std::string& owner, const std::string& repo) { return rt::KvKey{KERNEL_AUTH_USAGE_DAILY_PREFIX, owner, repo}; }
  rt::KvKey kernelOrgUsageKey(const std::string& owner) { return rt::KvKey{KERNEL_AUTH_ORG_USAGE_PREFIX, owner}; }
  rt::KvKey kernelOrgUsageDailyKey(const std::string& owner) { return rt::KvKey{KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX, owner}; }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageWithDaily& value) {
    json::value record = json::value_from(static_cast<const KernelAuthUsageRecord&>(value));
    if (value.daily_requests) record.as_object()["daily_requests"] = rt::record::toJson(*value.daily_requests);
    out = std::move(record);
  }

  KernelAuthUsageWithDaily tag_invoke(const json::value_to_tag<KernelAuthUsageWithDaily>&, const json::value& value) {
    KernelAuthUsageRecord record = json::value_to<KernelAuthUsageRecord>(value);
    const Reader in(value, "KernelAuthUsageWithDaily");
    return KernelAuthUsageWithDaily{std::move(record), in.orUndefined<std::vector<double>>("daily_requests")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageWithDaily& value) {
    json::value record = json::value_from(static_cast<const KernelOrgUsageRecord&>(value));
    if (value.daily_requests) record.as_object()["daily_requests"] = rt::record::toJson(*value.daily_requests);
    out = std::move(record);
  }

  KernelOrgUsageWithDaily tag_invoke(const json::value_to_tag<KernelOrgUsageWithDaily>&, const json::value& value) {
    KernelOrgUsageRecord record = json::value_to<KernelOrgUsageRecord>(value);
    const Reader in(value, "KernelOrgUsageWithDaily");
    return KernelOrgUsageWithDaily{std::move(record), in.orUndefined<std::vector<double>>("daily_requests")};
  }

  rt::Task<std::optional<KernelAuthUsageWithDaily>> getKernelUsage(std::string owner, std::string repo) {
    return getKernelUsage(std::move(owner), std::move(repo), KernelUsageReadOptions{});
  }

  rt::Task<std::optional<KernelAuthUsageWithDaily>> getKernelUsage(std::string owner, std::string repo, KernelUsageReadOptions options) {
    rt::Reason failure;
    try {
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      const double nowMs = rt::dateNow();
      auto key = kernelUsageKey(owner, repo);
      auto reading = kv->get(std::move(key));
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (!entry.value.truthy()) co_return std::nullopt;
      KernelAuthUsageWithDaily usage{normalizeUsageRecord(entry.value, owner, repo, nowMs), std::nullopt};
      if (!rt::truthy(options.includeDaily)) co_return usage;
      auto dailyKey = kernelUsageDailyKey(owner, repo);
      auto readingDaily = kv->get(std::move(dailyKey));
      const rt::KvEntryMaybe<> dailyEntry = co_await std::move(readingDaily);
      const KernelAuthUsageDailyRecord dailyRecord = normalizeDailyUsageRecord(dailyEntry.value, owner, repo, nowMs);
      usage.daily_requests = buildDailySeries(dailyRecord, nowMs, options.dailyDays.value_or(DAILY_SERIES_DAYS));
      co_return usage;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to load kernel auth usage:", failure);
    co_return std::nullopt;
  }

  rt::Task<std::optional<KernelOrgUsageWithDaily>> getKernelOrgUsage(std::string owner) { return getKernelOrgUsage(std::move(owner), KernelUsageReadOptions{}); }

  rt::Task<std::optional<KernelOrgUsageWithDaily>> getKernelOrgUsage(std::string owner, KernelUsageReadOptions options) {
    rt::Reason failure;
    try {
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      const double nowMs = rt::dateNow();
      auto key = kernelOrgUsageKey(owner);
      auto reading = kv->get(std::move(key));
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (!entry.value.truthy()) co_return std::nullopt;
      KernelOrgUsageWithDaily usage{normalizeOrgUsageRecord(entry.value, owner, nowMs), std::nullopt};
      if (!rt::truthy(options.includeDaily)) co_return usage;
      auto dailyKey = kernelOrgUsageDailyKey(owner);
      auto readingDaily = kv->get(std::move(dailyKey));
      const rt::KvEntryMaybe<> dailyEntry = co_await std::move(readingDaily);
      const KernelOrgUsageDailyRecord dailyRecord = normalizeDailyOrgUsageRecord(dailyEntry.value, owner, nowMs);
      usage.daily_requests = buildDailySeries(dailyRecord, nowMs, options.dailyDays.value_or(DAILY_SERIES_DAYS));
      co_return usage;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to load kernel org usage:", failure);
    co_return std::nullopt;
  }

  rt::Task<std::optional<std::vector<KernelAuthUsageWithDaily>>> listKernelUsageRecords() { return listKernelUsageRecords(KernelUsageReadOptions{}); }

  rt::Task<std::optional<std::vector<KernelAuthUsageWithDaily>>> listKernelUsageRecords(KernelUsageReadOptions options) {
    rt::Reason failure;
    try {
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      const double nowMs = rt::dateNow();
      std::vector<KernelAuthUsageWithDaily> records;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = KERNEL_AUTH_USAGE_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const std::string keyOwner = keyText(entry->key, 3);
        const std::string keyRepo = keyText(entry->key, 4);
        KernelAuthUsageWithDaily usage{normalizeUsageRecord(entry->value, keyOwner, keyRepo, nowMs), std::nullopt};
        if (rt::truthy(options.includeDaily)) {
          auto dailyKey = kernelUsageDailyKey(usage.owner, usage.repo);
          auto readingDaily = kv->get(std::move(dailyKey));
          const rt::KvEntryMaybe<> dailyEntry = co_await std::move(readingDaily);
          const KernelAuthUsageDailyRecord dailyRecord = normalizeDailyUsageRecord(dailyEntry.value, usage.owner, usage.repo, nowMs);
          usage.daily_requests = buildDailySeries(dailyRecord, nowMs, options.dailyDays.value_or(DAILY_SERIES_DAYS));
        }
        records.push_back(std::move(usage));
      }
      rt::sort(records, [](const KernelAuthUsageWithDaily& a, const KernelAuthUsageWithDaily& b) {
        const int ownerCmp = rt::localeCompare(a.owner, b.owner);
        if (ownerCmp != 0) return ownerCmp;
        return rt::localeCompare(a.repo, b.repo);
      });
      co_return records;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to list kernel auth usage records:", failure);
    co_return std::nullopt;
  }

  rt::Task<std::optional<std::vector<KernelOrgUsageWithDaily>>> listKernelOrgUsageRecords() { return listKernelOrgUsageRecords(KernelUsageReadOptions{}); }

  rt::Task<std::optional<std::vector<KernelOrgUsageWithDaily>>> listKernelOrgUsageRecords(KernelUsageReadOptions options) {
    rt::Reason failure;
    try {
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      const double nowMs = rt::dateNow();
      std::vector<KernelOrgUsageWithDaily> records;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = KERNEL_AUTH_ORG_USAGE_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const std::string keyOwner = keyText(entry->key, 3);
        KernelOrgUsageWithDaily usage{normalizeOrgUsageRecord(entry->value, keyOwner, nowMs), std::nullopt};
        if (rt::truthy(options.includeDaily)) {
          auto dailyKey = kernelOrgUsageDailyKey(usage.owner);
          auto readingDaily = kv->get(std::move(dailyKey));
          const rt::KvEntryMaybe<> dailyEntry = co_await std::move(readingDaily);
          const KernelOrgUsageDailyRecord dailyRecord = normalizeDailyOrgUsageRecord(dailyEntry.value, usage.owner, nowMs);
          usage.daily_requests = buildDailySeries(dailyRecord, nowMs, options.dailyDays.value_or(DAILY_SERIES_DAYS));
        }
        records.push_back(std::move(usage));
      }
      rt::sort(records, [](const KernelOrgUsageWithDaily& a, const KernelOrgUsageWithDaily& b) { return rt::localeCompare(a.owner, b.owner); });
      co_return records;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to list kernel org usage records:", failure);
    co_return std::nullopt;
  }
}
