#include "uos/analytics.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace uos::analytics {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr double DAY_MS = 24.0 * 60 * 60 * 1000;
    constexpr double MAX_REQUEST_LOGS = 100;
    constexpr double MAX_PAID_FALLBACK_LEDGER_WRITE_RETRIES = 3;

    static_assert(API_KEY_REQUEST_LOG_RETENTION_MS == 365 * DAY_MS);

    // `typeof value === "object" && value !== null` over a stored value.

    // `integer(value: unknown, fallback = 0)`
    double integer(const rt::KvValue& value, double fallback = 0) {
      return value.isNumber() && rt::Number::isFinite(value.asNumber()) ? rt::Math::trunc(value.asNumber()) : fallback;
    }

    std::optional<double> nullableInteger(const rt::KvValue& value) {
      const double normalized = integer(value, -1);
      return normalized >= 0 ? std::optional<double>(normalized) : std::nullopt;
    }

    // `text(value: unknown, max = 120, fallback = "")`
    std::string text(const rt::KvValue& value, double max = 120, std::string_view fallback = "") {
      const std::string trimmed = value.isString() ? rt::trim(value.asString()) : std::string();
      // An empty trimmed string still counts as absent, so the fallback keeps winning.
      const std::string normalized = rt::length(trimmed) > 0 ? trimmed : std::string(fallback);
      return rt::slice(normalized, 0, max);
    }

    // `text(value) || null`
    std::optional<std::string> textOrNull(const rt::KvValue& value) {
      std::string normalized = text(value);
      if (normalized.empty()) return std::nullopt;
      return normalized;
    }

    /**
     * Provider names that survive a ledger read. Anything else (unknown, absent or
     * non-string) normalizes to the primary Codex provider, exactly as the
     * comparison chain it replaces did.
     */
    constexpr std::array<std::string_view, 7> CATALOG_PROVIDERS{"cerebras", "deepseek", "lithos", "openrouter", "voyage", "surplus", "metered"};

    // `ApiKeyRequestLogRecord["provider"]`
    std::string requestLogProvider(const rt::KvValue& value) {
      if (!value.isString()) return "chatgpt_codex";
      for (std::string_view provider : CATALOG_PROVIDERS) {
        if (provider == value.asString()) return std::string(provider);
      }
      return "chatgpt_codex";
    }

    std::optional<types::ApiKeyRequestLogRecord> normalize(const rt::KvValue& value, std::string_view keyId, std::string_view requestId, double createdAtMs) {
      if (!rt::kvIsObject(value)) return std::nullopt;
      const rt::KvValue& billing = value["billing_status"];
      const bool knownBilling = billing.isString() && (billing.asString() == "pending" || billing.asString() == "reconciled" || billing.asString() == "not_billed" ||
                                                       billing.asString() == "unresolved");
      return types::ApiKeyRequestLogRecord{
        .id = text(value["id"], 120, requestId),
        .key_id = text(value["key_id"], 200, keyId),
        .route = text(value["route"], 120, "unknown"),
        .path = text(value["path"], 1024),
        .method = rt::toUpperCase(text(value["method"], 16, "UNKNOWN")),
        .status_code = rt::Math::max(0.0, rt::Math::min(599.0, integer(value["status_code"]))),
        .stream = value["stream"].isBool() && value["stream"].asBool(),
        .model = textOrNull(value["model"]),
        .reasoning = textOrNull(value["reasoning"]),
        .created_at_ms = integer(value["created_at_ms"], createdAtMs),
        .provider = requestLogProvider(value["provider"]),
        .fallback_reason = textOrNull(value["fallback_reason"]),
        .provider_request_id = textOrNull(value["provider_request_id"]),
        .completed_at_ms = nullableInteger(value["completed_at_ms"]),
        .latency_ms = nullableInteger(value["latency_ms"]),
        .input_tokens = nullableInteger(value["input_tokens"]),
        .output_tokens = nullableInteger(value["output_tokens"]),
        .provider_quota = nullableInteger(value["provider_quota"]),
        .quota_per_credit = nullableInteger(value["quota_per_credit"]),
        .spend_microcredits = nullableInteger(value["spend_microcredits"]),
        .paid_fallback_window_reset_at_ms = nullableInteger(value["paid_fallback_window_reset_at_ms"]),
        .billing_status = knownBilling ? billing.asString() : std::string("not_applicable"),
      };
    }

    // `mutate: (existing: ApiKeyRequestLogRecord | null) => ApiKeyRequestLogInput | ApiKeyRequestLogRecord | null`:
    // the next record as the plain object the TS builds (std::nullopt is null).
    using Mutation = std::function<std::optional<json::object>(const std::optional<types::ApiKeyRequestLogRecord>& existing)>;

    json::object recordObject(const types::ApiKeyRequestLogRecord& record) {
      json::value value = json::value_from(record);
      return std::move(value.as_object());
    }

    rt::Task<void> mutateApiKeyRequestLog(std::string keyId, double createdAtMs, std::string requestId, Mutation mutate,
                                          std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      std::shared_ptr<rt::Kv> kv;
      if (kvOverride) {
        kv = *kvOverride;
      } else {
        const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
        kv = co_await opening.wait();
      }
      if (!kv || rt::trim(keyId).empty()) co_return;
      const rt::KvKey key = apiKeyRequestLogKey(keyId, createdAtMs, requestId);
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};

      for (double attempt = 0; attempt < MAX_PAID_FALLBACK_LEDGER_WRITE_RETRIES; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
        const std::optional<types::ApiKeyRequestLogRecord> existing = normalize(entry.value, keyId, requestId, createdAtMs);
        std::optional<json::object> next = mutate(existing);
        if (!next) co_return;
        // `{ ...next, id: requestId, key_id: keyId, created_at_ms: createdAtMs }`
        rt::objectDefine(*next, "id", json::value(requestId));
        rt::objectDefine(*next, "key_id", json::value(keyId));
        rt::objectDefine(*next, "created_at_ms", rt::makeNumber(createdAtMs));
        const std::optional<types::ApiKeyRequestLogRecord> record = normalize(rt::KvValue(*next), keyId, requestId, createdAtMs);
        if (!record) co_return;
        const double expireIn = rt::Math::max(1.0, createdAtMs + API_KEY_REQUEST_LOG_RETENTION_MS - rt::dateNow());
        const rt::KvSetOptions setOptions{.expireIn = expireIn};
        const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(key, rt::KvValue(json::value_from(*record)), setOptions);
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return;
      }

      throw rt::JsError("paid fallback ledger changed concurrently: " + requestId);
    }

    // `entry.key.at(index)` as the value the TS reads (undefined out of range).
    rt::KvValue keyPartAt(const rt::KvKey& key, std::int64_t index) {
      const std::int64_t size = static_cast<std::int64_t>(key.size());
      const std::int64_t position = index < 0 ? size + index : index;
      if (position < 0 || position >= size) return rt::KvValue();
      const rt::KvKeyPart& part = key[static_cast<std::size_t>(position)];
      if (part.isString()) return rt::KvValue(part.string());
      if (part.isNumber()) return rt::KvValue(part.number());
      if (part.isBigInt()) return rt::KvValue(part.bigint());
      if (part.isBool()) return rt::KvValue(part.boolean());
      return rt::KvValue(part.bytes());
    }
  }

  rt::KvKey apiKeyUsageKey(std::string_view id) { return rt::KvKey{API_KEY_USAGE_PREFIX, id}; }
  rt::KvKey apiKeyUsageDailyKey(std::string_view id) { return rt::KvKey{API_KEY_USAGE_DAILY_PREFIX, id}; }
  rt::KvKey legacyApiKeyRequestLogPrefix(std::string_view id) { return rt::KvKey{LEGACY_API_KEY_REQUEST_LOG_PREFIX, id}; }
  rt::KvKey apiKeyRequestLogPrefix(std::string_view id) { return rt::KvKey{API_KEY_REQUEST_LOG_PREFIX, id}; }
  rt::KvKey apiKeyRequestLogKey(std::string_view id, double createdAtMs, std::string_view requestId) {
    return rt::KvKey{apiKeyRequestLogPrefix(id), createdAtMs, requestId};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRequestLogInput& value) {
    out = Writer()
            .orUndefined("id", value.id)
            .required("route", value.route)
            .required("path", value.path)
            .required("method", value.method)
            .required("status_code", value.status_code)
            .required("stream", value.stream)
            .nullable("model", value.model)
            .nullable("reasoning", value.reasoning)
            .required("created_at_ms", value.created_at_ms)
            .orUndefined("provider", value.provider)
            .nullable("fallback_reason", value.fallback_reason)
            .nullable("provider_request_id", value.provider_request_id)
            .nullable("completed_at_ms", value.completed_at_ms)
            .nullable("latency_ms", value.latency_ms)
            .nullable("input_tokens", value.input_tokens)
            .nullable("output_tokens", value.output_tokens)
            .nullable("provider_quota", value.provider_quota)
            .nullable("quota_per_credit", value.quota_per_credit)
            .nullable("spend_microcredits", value.spend_microcredits)
            .nullable("paid_fallback_window_reset_at_ms", value.paid_fallback_window_reset_at_ms)
            .orUndefined("billing_status", value.billing_status)
            .done();
  }

  ApiKeyRequestLogInput tag_invoke(const json::value_to_tag<ApiKeyRequestLogInput>&, const json::value& value) {
    const Reader in(value, "ApiKeyRequestLogInput");
    return {
      .id = in.orUndefined<std::string>("id"),
      .route = in.required<std::string>("route"),
      .path = in.required<std::string>("path"),
      .method = in.required<std::string>("method"),
      .status_code = in.required<double>("status_code"),
      .stream = in.required<bool>("stream"),
      .model = in.nullable<std::string>("model"),
      .reasoning = in.nullable<std::string>("reasoning"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .provider = in.orUndefined<std::string>("provider"),
      .fallback_reason = in.nullable<std::string>("fallback_reason"),
      .provider_request_id = in.nullable<std::string>("provider_request_id"),
      .completed_at_ms = in.nullable<double>("completed_at_ms"),
      .latency_ms = in.nullable<double>("latency_ms"),
      .input_tokens = in.nullable<double>("input_tokens"),
      .output_tokens = in.nullable<double>("output_tokens"),
      .provider_quota = in.nullable<double>("provider_quota"),
      .quota_per_credit = in.nullable<double>("quota_per_credit"),
      .spend_microcredits = in.nullable<double>("spend_microcredits"),
      .paid_fallback_window_reset_at_ms = in.nullable<double>("paid_fallback_window_reset_at_ms"),
      .billing_status = in.orUndefined<std::string>("billing_status"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRequestLogPatch& value) {
    out = Writer()
            .orUndefined("route", value.route)
            .orUndefined("path", value.path)
            .orUndefined("method", value.method)
            .orUndefined("status_code", value.status_code)
            .orUndefined("stream", value.stream)
            .nullable("model", value.model)
            .nullable("reasoning", value.reasoning)
            .orUndefined("provider", value.provider)
            .nullable("fallback_reason", value.fallback_reason)
            .nullable("provider_request_id", value.provider_request_id)
            .nullable("completed_at_ms", value.completed_at_ms)
            .nullable("latency_ms", value.latency_ms)
            .nullable("input_tokens", value.input_tokens)
            .nullable("output_tokens", value.output_tokens)
            .nullable("provider_quota", value.provider_quota)
            .nullable("quota_per_credit", value.quota_per_credit)
            .nullable("spend_microcredits", value.spend_microcredits)
            .nullable("paid_fallback_window_reset_at_ms", value.paid_fallback_window_reset_at_ms)
            .orUndefined("billing_status", value.billing_status)
            .done();
  }

  ApiKeyRequestLogPatch tag_invoke(const json::value_to_tag<ApiKeyRequestLogPatch>&, const json::value& value) {
    const Reader in(value, "ApiKeyRequestLogPatch");
    return {
      .route = in.orUndefined<std::string>("route"),
      .path = in.orUndefined<std::string>("path"),
      .method = in.orUndefined<std::string>("method"),
      .status_code = in.orUndefined<double>("status_code"),
      .stream = in.orUndefined<bool>("stream"),
      .model = in.nullable<std::string>("model"),
      .reasoning = in.nullable<std::string>("reasoning"),
      .provider = in.orUndefined<std::string>("provider"),
      .fallback_reason = in.nullable<std::string>("fallback_reason"),
      .provider_request_id = in.nullable<std::string>("provider_request_id"),
      .completed_at_ms = in.nullable<double>("completed_at_ms"),
      .latency_ms = in.nullable<double>("latency_ms"),
      .input_tokens = in.nullable<double>("input_tokens"),
      .output_tokens = in.nullable<double>("output_tokens"),
      .provider_quota = in.nullable<double>("provider_quota"),
      .quota_per_credit = in.nullable<double>("quota_per_credit"),
      .spend_microcredits = in.nullable<double>("spend_microcredits"),
      .paid_fallback_window_reset_at_ms = in.nullable<double>("paid_fallback_window_reset_at_ms"),
      .billing_status = in.orUndefined<std::string>("billing_status"),
    };
  }

  rt::Task<void> recordApiKeyRequestLog(std::string keyId, ApiKeyRequestLogInput input) {
    rt::Task<void> recording = recordApiKeyRequestLog(std::move(keyId), std::move(input), std::nullopt);
    co_await std::move(recording);
  }

  rt::Task<void> recordApiKeyRequestLog(std::string keyId, ApiKeyRequestLogInput input, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    const double nowMs = rt::dateNow();
    const rt::KvValue inputId = input.id ? rt::KvValue(*input.id) : rt::KvValue();
    const std::string requestId = text(inputId, 120, rt::crypto::randomUUID());
    const double createdAtMs = integer(rt::KvValue(input.created_at_ms), nowMs);
    // `(existing) => ({ ...(existing ?? {}), ...input })`
    json::value inputObject = json::value_from(input);
    Mutation mutate = [inputObject = std::move(inputObject)](const std::optional<types::ApiKeyRequestLogRecord>& existing) -> std::optional<json::object> {
      const json::object base = existing ? recordObject(*existing) : json::object();
      return rt::objectSpread(base, inputObject.as_object());
    };
    rt::Task<void> mutating = mutateApiKeyRequestLog(std::move(keyId), createdAtMs, requestId, std::move(mutate), std::move(kvOverride));
    co_await std::move(mutating);
  }

  rt::Task<void> updateApiKeyRequestLog(std::string keyId, double createdAtMs, std::string requestId, ApiKeyRequestLogPatch patch) {
    rt::Task<void> updating = updateApiKeyRequestLog(std::move(keyId), createdAtMs, std::move(requestId), std::move(patch), std::nullopt);
    co_await std::move(updating);
  }

  rt::Task<void> updateApiKeyRequestLog(std::string keyId, double createdAtMs, std::string requestId, ApiKeyRequestLogPatch patch,
                                        std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    // `(existing) => (existing ? { ...existing, ...patch } : null)`
    json::value patchObject = json::value_from(patch);
    Mutation mutate = [patchObject = std::move(patchObject)](const std::optional<types::ApiKeyRequestLogRecord>& existing) -> std::optional<json::object> {
      if (!existing) return std::nullopt;
      const json::object base = recordObject(*existing);
      return rt::objectSpread(base, patchObject.as_object());
    };
    rt::Task<void> mutating = mutateApiKeyRequestLog(std::move(keyId), createdAtMs, std::move(requestId), std::move(mutate), std::move(kvOverride));
    co_await std::move(mutating);
  }

  rt::Task<std::vector<types::ApiKeyRequestLogRecord>> listApiKeyRequestLogs(std::string keyId) {
    rt::Task<std::vector<types::ApiKeyRequestLogRecord>> listing = listApiKeyRequestLogs(std::move(keyId), ListApiKeyRequestLogsOptions());
    std::vector<types::ApiKeyRequestLogRecord> records = co_await std::move(listing);
    co_return records;
  }

  rt::Task<std::vector<types::ApiKeyRequestLogRecord>> listApiKeyRequestLogs(std::string keyId, ListApiKeyRequestLogsOptions options) {
    std::shared_ptr<rt::Kv> kv;
    if (options.kv) {
      kv = *options.kv;
    } else {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
      kv = co_await opening.wait();
    }
    std::vector<types::ApiKeyRequestLogRecord> records;
    if (!kv || rt::trim(keyId).empty()) co_return records;
    const double limit = rt::Math::max(1.0, rt::Math::min(MAX_REQUEST_LOGS, rt::Math::trunc(options.limit.value_or(20))));
    rt::KvListSelector selector;
    selector.prefix = apiKeyRequestLogPrefix(keyId);
    rt::KvListOptions listOptions;
    listOptions.reverse = true;
    listOptions.limit = limit;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, listOptions);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const std::string requestId = text(keyPartAt(entry->key, -1), 120, "request");
      const double createdAtMs = integer(keyPartAt(entry->key, -2), rt::dateNow());
      std::optional<types::ApiKeyRequestLogRecord> record = normalize(entry->value, keyId, requestId, createdAtMs);
      if (record) records.push_back(std::move(*record));
    }
    co_return records;
  }
}
