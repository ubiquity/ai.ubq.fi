// Port of src/admin/api-keys.ts.
#include "uos/admin/api_keys.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/provider/metered.hpp"
#include "uos/request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"

#include <utility>
#include <variant>

namespace uos::admin::api_keys {
  namespace {
    namespace policy = authority::native_api_key_policy;
    namespace keys = ::uos::api_keys;
    namespace paid = paid_fallback::index;
    namespace ledger = paid_fallback::ledger_state;
    bool whitespace(const std::string& value) { static const rt::Regex pattern("\\s", ""); return pattern.test(value); }
    json::object paidFallbackHistoryRecord(const ::uos::types::PaidFallbackRequestV3& request) {
      auto record = json::value_from(request).as_object();
      const double startedAtMs = request.dispatched_at_ms.value_or(request.created_at_ms);
      record["id"] = request.request_id;
      record["method"] = "POST";
      record["status_code"] = request.terminal_state == "completed" ? json::value(200) : json::value(nullptr);
      record["provider"] = request.provider.value_or("metered");
      record["fallback_reason"] = "codex_429";
      record["started_at_ms"] = rt::makeNumber(startedAtMs);
      record["completed_at_ms"] = request.terminal_at_ms ? rt::makeNumber(*request.terminal_at_ms) : json::value(nullptr);
      record["latency_ms"] = request.terminal_at_ms ? rt::makeNumber(rt::Math::max(0.0, *request.terminal_at_ms - startedAtMs)) : json::value(nullptr);
      record["paid_fallback_window_reset_at_ms"] = rt::makeNumber(request.window_reset_at_ms);
      record["billing_status"] = request.billing_state == "settled" ? "reconciled" : request.billing_state;
      return record;
    }
    void copyField(json::object& output, const json::value& record, std::string_view field) {
      if (const auto* value = rt::get(record, field)) rt::objectDefine(output, field, *value);
    }
    std::string recordString(const json::value& record, std::string_view field) {
      const auto value = rt::getString(record, field);
      if (!value) throw rt::TypeError("API key " + std::string(field) + " is not a string");
      return std::string(*value);
    }
    rt::Task<json::object> paidPublicFields(json::value record, std::shared_ptr<rt::Kv> kv, double windowResetAtMs) {
      const auto id = recordString(record, "id");
      const double limit = rt::toNumber(rt::get(record, "paid_fallback_limit_microcredits"));
      auto projectionTask = ledger::getPaidFallbackWindowProjectionV3(id, windowResetAtMs, limit, kv);
      rt::Promise<std::optional<ledger::PaidFallbackWindowProjectionV3>> projectionPending(std::move(projectionTask));
      auto providerTask = ledger::getPaidFallbackProviderUsageV3(id, windowResetAtMs, kv);
      rt::Promise<ledger::PaidFallbackProviderUsageByProviderV3> providerPending(std::move(providerTask));
      auto combined = rt::all(projectionPending, providerPending);
      const auto result = co_await combined.wait();
      const auto& projection = std::get<0>(result);
      json::object body;
      copyField(body, record, "paid_fallback_enabled");
      body["paid_fallback_limit_credits"] = rt::makeNumber(keys::paidFallbackMicrocreditsToCredits(rt::get(record, "paid_fallback_limit_microcredits")));
      body["paid_fallback_spent_credits"] = rt::makeNumber(keys::paidFallbackMicrocreditsToCredits(projection ? projection->settled_microcredits : 0));
      body["paid_fallback_reserved_credits"] = rt::makeNumber(keys::paidFallbackMicrocreditsToCredits(projection ? projection->reserved_microcredits : 0));
      body["paid_fallback_pending_count"] = rt::makeNumber(projection ? projection->pending_count : 0);
      body["paid_fallback_provider_usage"] = json::value_from(std::get<1>(result));
      copyField(body, record, "paid_fallback_model_ids");
      copyField(body, record, "paid_fallback_pricing_checked_at_ms");
      co_return body;
    }
    struct CreatePolicyOk { paid::PaidFallbackPolicyFields policy; };
    struct CreatePolicyNotOk { rt::Response response; };
    using CreatePolicyResult = std::variant<CreatePolicyOk, CreatePolicyNotOk>;
    rt::Task<CreatePolicyResult> resolveApiKeyCreatePolicy(rt::AbortSignal signal, policy::ApiKeyCreateFields fields) {
      auto paidFallbackPolicy = paid::defaultPaidFallbackPolicy();
      if (!fields.paidFallbackEnabled) {
        paidFallbackPolicy.paid_fallback_limit_microcredits = fields.paidFallbackLimitMicrocredits;
        co_return CreatePolicyOk{paidFallbackPolicy};
      }
      try {
        auto task = paid::initializePaidFallbackPolicy(signal);
        rt::Promise<paid::InitializePaidFallbackPolicyResult> pending(std::move(task));
        const auto initialized = co_await pending.wait();
        paidFallbackPolicy.paid_fallback_model_ids = initialized.paid_fallback_model_ids;
        paidFallbackPolicy.paid_fallback_quota_per_credit = initialized.paid_fallback_quota_per_credit;
        paidFallbackPolicy.paid_fallback_max_exposure_microcredits = initialized.paid_fallback_max_exposure_microcredits;
        paidFallbackPolicy.paid_fallback_pricing_checked_at_ms = initialized.paid_fallback_pricing_checked_at_ms;
        paidFallbackPolicy.paid_fallback_enabled = true;
        paidFallbackPolicy.paid_fallback_limit_microcredits = fields.paidFallbackLimitMicrocredits;
        co_return CreatePolicyOk{paidFallbackPolicy};
      } catch (...) { co_return CreatePolicyNotOk{paidFallbackInitializationError(std::current_exception())}; }
    }
    rt::Task<rt::Response> create(rt::Request req, std::optional<api_key_audit::ApiKeyAuditContext> context) {
      auto kvPending = kv::getKv();
      auto kv = co_await kvPending.wait();
      if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
      rt::Promise<json::value> parsed(request::readJsonBody(req));
      const auto raw = co_await parsed.wait();
      if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto object = raw.is_object() ? raw.as_object() : json::object();
      if (const auto retired = rejectRetiredApiKeyResetSetting(object)) co_return *retired;
      const auto name = normalizeApiKeyName(rt::get(raw, "name"));
      if (!rt::truthy(name)) co_return http::openaiError(400, "name must be a non-empty string (<=80 chars)", "invalid_request_error");
      const auto providedToken = policy::normalizeOptionalApiKeyToken(rt::get(raw, "token"));
      if (!rt::isNullish(rt::get(raw, "token")) && !providedToken) co_return http::openaiError(400, "token must use the u_ prefix followed by 64 lowercase hexadecimal characters", "invalid_request_error");
      const auto token = providedToken ? *providedToken : keys::generateApiKeyToken();
      const double now = rt::dateNow();
      const policy::ResolveApiKeyCreateFieldsDefaults defaults{keys::getDefaultExpiryMs(now), keys::DEFAULT_USAGE_LIMIT_REQUESTS};
      const auto createFields = policy::resolveApiKeyCreateFields(object, now, defaults);
      if (const auto* failure = std::get_if<policy::ApiKeyFieldsNotOk>(&createFields)) co_return http::openaiError(400, failure->error, "invalid_request_error");
      const auto fields = std::get<policy::ResolveApiKeyCreateFieldsOk>(createFields).fields;
      rt::Promise<std::string> hashDigest(utils::sha256Base64Url(token));
      const auto hash = co_await hashDigest.wait();
      const auto hashKey = keys::apiKeyHashKey(hash);
      rt::Promise<rt::KvEntryMaybe<>> hashPending(kv->get(hashKey));
      const auto hashEntry = co_await hashPending.wait();
      if (hashEntry.value.truthy()) co_return http::openaiError(409, "API key already exists", "invalid_request_error");
      auto policyTask = resolveApiKeyCreatePolicy(req.signal(), fields);
      rt::Promise<CreatePolicyResult> policyPending(std::move(policyTask));
      const auto policyResult = co_await policyPending.wait();
      if (const auto* failure = std::get_if<CreatePolicyNotOk>(&policyResult)) co_return failure->response;
      const auto id = rt::crypto::randomUUID();
      const policy::BuildApiKeyCreateRecordsIdentity identity{id, *name, rt::slice(token, 0, 12), hash};
      const auto records = policy::buildApiKeyCreateRecords(identity, fields, std::get<CreatePolicyOk>(policyResult).policy, now);
      const auto& record = records.record;
      const auto quotaPolicy = api_key_policy::apiKeyPolicyFromHashRecord(hash, records.hashRecord, now);
      if (!quotaPolicy) co_return http::openaiError(500, "Failed to build API key quota policy", "server_error");
      const auto quotaWindow = api_key_policy::makeApiKeyUsageWindowV3(*quotaPolicy, now);
      auto operation = kv->atomic();
      const rt::KvValue recordValue(json::value_from(record)), hashValue(json::value_from(records.hashRecord)), windowValue(json::value_from(quotaWindow));
      operation->check(hashEntry).set(keys::apiKeyIdKey(id), recordValue).set(hashKey, hashValue).set(api_key_policy::apiKeyUsageV3WindowKey(*quotaPolicy), windowValue, rt::KvSetOptions{.expireIn = api_key_policy::apiKeyUsageV3RetentionMs(quotaWindow.window_reset_at_ms, now)});
      const json::object changed{{"name", *name}, {"expires_at_ms", rt::makeNumber(fields.expiresAtMs)}, {"usage_limit_requests", rt::makeNumber(fields.usageLimitRequests)}, {"window_ms", rt::makeNumber(fields.windowMs)}, {"paid_fallback_enabled", record.paid_fallback_enabled}, {"paid_fallback_limit_credits", rt::makeNumber(keys::paidFallbackMicrocreditsToCredits(record.paid_fallback_limit_microcredits))}};
      const api_key_audit::ApiKeyChangeEventInput input{.action = "create", .targetKeyId = id, .targetKeyName = *name, .changed = changed, .context = context, .nowMs = now};
      const auto event = api_key_audit::buildApiKeyChangeEvent(input);
      api_key_audit::appendApiKeyChangeEvent(*operation, event);
      rt::Promise<rt::KvCommitResult> committed(operation->commit());
      const auto commit = co_await committed.wait();
      if (!commit.ok) co_return http::openaiError(500, "Failed to persist API key", "server_error");
      json::object body{{"id", id}, {"name", *name}, {"token", token}, {"prefix", record.prefix}, {"created_at_ms", rt::makeNumber(record.created_at_ms)}, {"expires_at_ms", rt::makeNumber(record.expires_at_ms)}, {"usage_limit_requests", rt::makeNumber(record.usage_limit_requests)}, {"usage_requests", rt::makeNumber(record.usage_requests)}, {"usage_reset_at_ms", rt::makeNumber(record.usage_reset_at_ms)}, {"window_ms", rt::makeNumber(record.window_ms)}};
      auto publicTask = paidFallbackPublicFields(record, kv);
      rt::Promise<json::object> publicPending(std::move(publicTask));
      const auto publicFields = co_await publicPending.wait();
      body = rt::objectSpread(body, publicFields);
      co_return http::json(200, body, rt::Headers{{"x-uos-upstream", "chatgpt_codex"}});
    }
    rt::Task<rt::Response> history(rt::Request req, std::string keyId, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      std::shared_ptr<rt::Kv> kv;
      if (kvOverride) kv = *kvOverride;
      else { auto pending = kv::getKv(); kv = co_await pending.wait(); }
      if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot load paid fallbacks", "server_error");
      const auto normalizedKeyId = rt::trim(keyId);
      if (normalizedKeyId.empty() || rt::length(normalizedKeyId) > 200) co_return http::openaiError(400, "Invalid API key id", "invalid_request_error");
      const auto key = keys::apiKeyIdKey(normalizedKeyId);
      rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key));
      const auto keyEntry = co_await reading.wait();
      if (!keyEntry.value.truthy()) co_return http::openaiError(404, "Not found", "not_found");
      rt::URL url(req.url());
      const auto rawLimit = url.searchParams().get("limit");
      static const rt::Regex digits("^\\d+$", "");
      if (rawLimit && !digits.test(rt::trim(*rawLimit))) co_return http::openaiError(400, "limit must be a positive integer", "invalid_request_error");
      const double requestedLimit = rawLimit ? rt::stringToNumber(*rawLimit) : 20;
      if (!rt::Number::isSafeInteger(requestedLimit) || requestedLimit < 1) co_return http::openaiError(400, "limit must be a positive integer", "invalid_request_error");
      const double limit = rt::Math::min(requestedLimit, 100.0);
      try {
        auto reconcile = paid_fallback::ledger_backfill::reconcileDuePaidFallbacksV3(rt::dateNow(), kv);
        rt::spawn(std::move(reconcile), rt::ignoreRejection);
        auto listTask = ledger::listPaidFallbackRequestsV3(normalizedKeyId, limit, kv);
        rt::Promise<std::vector<::uos::types::PaidFallbackRequestV3>> pending(std::move(listTask));
        const auto records = co_await pending.wait();
        json::array data;
        for (const auto& record : records) data.push_back(paidFallbackHistoryRecord(record));
        const json::object body{{"object", "list"}, {"data", std::move(data)}};
        co_return http::json(200, body, rt::Headers{{"Cache-Control", "no-store"}});
      } catch (...) {
        rt::console::error("[ai.ubq.fi] Failed to load paid fallback ledger:", rt::console::Value(std::current_exception()));
        co_return http::openaiError(500, "Failed to load paid fallbacks", "server_error");
      }
    }
  }
  bool shouldIncludeUsage(const std::optional<std::string>& value) {
    if (!rt::truthy(value)) return false;
    const auto normalized = rt::toLowerCase(rt::trim(*value));
    return normalized == "1" || normalized == "true" || normalized == "yes";
  }
  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value* value, double nowMs) { return policy::normalizeApiKeyExpiresAtMs(value, nowMs, keys::getDefaultExpiryMs(nowMs)); }
  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value& value, double nowMs) { return normalizeApiKeyExpiresAtMs(&value, nowMs); }
  std::optional<double> normalizeApiKeyUsageLimit(const json::value* value) { return policy::normalizeApiKeyUsageLimit(value, keys::DEFAULT_USAGE_LIMIT_REQUESTS); }
  std::optional<double> normalizeApiKeyUsageLimit(const json::value& value) { return normalizeApiKeyUsageLimit(&value); }
  std::optional<double> normalizeKernelWindowMsInput(const json::value* value) { return policy::normalizeApiKeyWindowMsInput(value); }
  std::optional<double> normalizeKernelWindowMsInput(const json::value& value) { return normalizeKernelWindowMsInput(&value); }
  rt::Response paidFallbackInputError(const std::string& message) { return http::openaiError(400, message, "invalid_request_error"); }
  std::optional<rt::Response> rejectRetiredApiKeyResetSetting(const json::object& raw) {
    const auto error = policy::rejectRetiredApiKeyResetSetting(raw);
    return error ? std::optional(http::openaiError(400, *error, "invalid_request_error")) : std::nullopt;
  }
  rt::Response paidFallbackInitializationError(rt::Reason error) {
    if (error) {
      try { std::rethrow_exception(error); }
      catch (const provider::metered::MeteredError& failure) { return http::openaiError(failure.status, failure.message, failure.code, http::OpenaiErrorOptions{.type = "server_error"}); }
      catch (...) {}
    }
    rt::console::error("[ai.ubq.fi] Failed to initialize Metered paid fallback:", rt::console::Value(error));
    return http::openaiError(502, "Failed to initialize Metered paid fallback", "metered_pricing_unavailable", http::OpenaiErrorOptions{.type = "server_error"});
  }
  rt::Task<json::object> paidFallbackPublicFields(::uos::types::ApiKeyRecord record, std::shared_ptr<rt::Kv> kv) {
    const auto reset = record.usage_reset_at_ms;
    return paidFallbackPublicFields(std::move(record), std::move(kv), reset);
  }
  rt::Task<json::object> paidFallbackPublicFields(::uos::types::ApiKeyRecord record, std::shared_ptr<rt::Kv> kv, double windowResetAtMs) {
    return paidPublicFields(json::value_from(record), std::move(kv), windowResetAtMs);
  }
  std::optional<std::string> normalizeKernelRepoPart(const json::value* value) {
    if (!value || !value->is_string()) return std::nullopt;
    const auto trimmed = rt::trim(std::string(value->as_string()));
    if (trimmed.empty() || rt::length(trimmed) > 100 || whitespace(trimmed) || rt::includes(trimmed, "/")) return std::nullopt;
    return trimmed;
  }
  std::optional<std::string> normalizeKernelRepoPart(const json::value& value) { return normalizeKernelRepoPart(&value); }
  std::optional<double> normalizeKernelUsageLimitInput(const json::value* value) {
    if (rt::isNullish(value)) return std::nullopt;
    json::value parsed = *value;
    if (parsed.is_string()) {
      const auto trimmed = rt::toLowerCase(rt::trim(std::string(parsed.as_string())));
      if (trimmed.empty()) return std::nullopt;
      if (trimmed == "unlimited") return keys::API_KEY_NO_USAGE_LIMIT;
      parsed = rt::makeNumber(rt::stringToNumber(trimmed));
    }
    const auto number = rt::getNumber(parsed);
    if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
    const auto limit = rt::Math::trunc(*number);
    if (limit == keys::API_KEY_NO_USAGE_LIMIT) return limit;
    return limit < 0 ? std::nullopt : std::optional(limit);
  }
  std::optional<double> normalizeKernelUsageLimitInput(const json::value& value) { return normalizeKernelUsageLimitInput(&value); }
  std::optional<double> normalizeKernelExpiresAtMsInput(const json::value* value, double nowMs) {
    if (rt::isNullish(value)) return std::nullopt;
    json::value parsed = *value;
    if (parsed.is_string()) {
      const auto trimmed = rt::trim(std::string(parsed.as_string()));
      if (trimmed.empty()) return std::nullopt;
      parsed = rt::makeNumber(rt::stringToNumber(trimmed));
    }
    const auto number = rt::getNumber(parsed);
    if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
    const auto expires = rt::Math::trunc(*number);
    if (expires == keys::API_KEY_NO_EXPIRATION_MS) return expires;
    return expires <= nowMs ? std::nullopt : std::optional(expires);
  }
  std::optional<double> normalizeKernelExpiresAtMsInput(const json::value& value, double nowMs) { return normalizeKernelExpiresAtMsInput(&value, nowMs); }
  std::string normalizeKernelScope(const json::value* value) { return value && value->is_string() && rt::toLowerCase(rt::trim(std::string(value->as_string()))) == "org" ? "org" : "repo"; }
  std::string normalizeKernelScope(const json::value& value) { return normalizeKernelScope(&value); }
  std::vector<defaults::ReasoningEffort> extractModelReasoningLevels(const std::optional<json::object>& model) {
    if (!model) return {};
    const auto* raw = rt::getArray(*model, "supported_reasoning_levels");
    if (!raw) return {};
    rt::OrderedSet<std::string> levels;
    for (const auto& entry : *raw) {
      std::optional<std::string> level;
      if (entry.is_null()) level = "none";
      else if (entry.is_string()) level = defaults::normalizeReasoningEffort(entry);
      else if (utils::isRecord(entry)) {
        const auto* effort = rt::get(entry, "effort");
        level = effort && effort->is_null() ? std::optional<std::string>("none") : defaults::normalizeReasoningEffort(effort);
      }
      if (rt::truthy(level)) levels.add(*level);
    }
    return rt::arrayFrom(levels.values());
  }
  std::optional<double> estimateJsonSize(const json::value& value) { return estimateJsonSize(&value); }
  std::optional<double> estimateJsonSize(const json::value* value) {
    if (!value) return 0;
    try { return static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(*value))); } catch (...) { return std::nullopt; }
  }
  std::optional<double> estimateJsonSize(const json::object& value) { return estimateJsonSize(json::value(value)); }
  std::optional<double> estimateJsonSize(const json::array& value) { return estimateJsonSize(json::value(value)); }
  std::optional<double> estimateJsonSize(const rt::KvValue& value) {
    try { const auto encoded = rt::kvJsonStringify(value); return encoded ? static_cast<double>(rt::utf8ByteLength(*encoded)) : 0.0; } catch (...) { return std::nullopt; }
  }
  rt::Task<rt::Response> handleAdminApiKeysCreate(rt::Request req) { return create(std::move(req), std::nullopt); }
  rt::Task<rt::Response> handleAdminApiKeysCreate(rt::Request req, const api_key_audit::ApiKeyAuditContext& context) { return create(std::move(req), context); }
  rt::Task<rt::Response> handleAdminApiKeysList(rt::Request req) {
    auto pending = kv::getKv();
    const auto kv = co_await pending.wait();
    if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
    std::vector<json::value> records;
    rt::KvListSelector selector;
    selector.prefix = keys::API_KEY_ID_PREFIX;
    auto iterator = kv->list(selector);
    while (auto entry = co_await iterator->next()) if (entry->value.truthy()) records.push_back(rt::storedJsonView(entry->value));
    rt::sort(records, [](const auto& a, const auto& b) { return rt::toNumber(rt::get(b, "created_at_ms")) - rt::toNumber(rt::get(a, "created_at_ms")); });
    rt::URL url(req.url());
    const bool includeUsage = shouldIncludeUsage(url.searchParams().get("include_usage"));
    rt::OrderedMap<std::string, json::object> usageById;
    rt::OrderedMap<std::string, double> paidFallbackResetById;
    const std::vector<std::string> hashFields{"id", "expires_at_ms", "revoked_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms", "usage_quota_version", "paid_fallback_enabled", "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits", "paid_fallback_reserved_microcredits", "paid_fallback_reservation_request_id"};
    for (const auto& record : records) {
      json::object hashRecord;
      for (const auto& field : hashFields) copyField(hashRecord, record, field);
      const auto hash = recordString(record, "hash");
      const auto quotaPolicy = api_key_policy::apiKeyPolicyFromHashRecord(hash, json::value(hashRecord), rt::dateNow());
      if (!quotaPolicy) continue;
      const auto id = recordString(record, "id");
      const auto* revoked = rt::get(record, "revoked_at_ms");
      paidFallbackResetById.set(id, revoked && revoked->is_null() ? quotaPolicy->usage_reset_at_ms : rt::toNumber(rt::get(record, "usage_reset_at_ms")));
      if (includeUsage) {
        auto usageTask = api_key_policy::getApiKeyUsageV3(*quotaPolicy, kv);
        rt::Promise<double> usagePending(std::move(usageTask));
        const auto used = co_await usagePending.wait();
        const json::object usage{{"request_count", rt::makeNumber(used)}, {"limit", rt::makeNumber(quotaPolicy->usage_limit_requests)}, {"reset_at_ms", rt::makeNumber(quotaPolicy->usage_reset_at_ms)}};
        usageById.set(id, usage);
      }
    }
    std::vector<rt::Promise<json::object>> paidPending;
    for (const auto& record : records) {
      const auto* reset = paidFallbackResetById.get(recordString(record, "id"));
      auto task = paidPublicFields(record, kv, reset ? *reset : rt::toNumber(rt::get(record, "usage_reset_at_ms")));
      paidPending.emplace_back(std::move(task));
    }
    auto combined = rt::all(std::move(paidPending));
    const auto paidFields = co_await combined.wait();
    json::array data;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto& record = records[i];
      const auto* usage = usageById.get(recordString(record, "id"));
      json::object row;
      for (const std::string_view field : {"id", "name", "prefix", "created_at_ms"}) copyField(row, record, field);
      row["expires_at_ms"] = rt::makeNumber(keys::coerceApiKeyExpiresAtMs(record));
      copyField(row, record, "revoked_at_ms");
      copyField(row, record, "usage_limit_requests");
      if (includeUsage && usage) row["usage_reset_at_ms"] = *rt::get(*usage, "reset_at_ms");
      else copyField(row, record, "usage_reset_at_ms");
      row["window_ms"] = rt::makeNumber(keys::coerceApiKeyWindowMs(record));
      row = rt::objectSpread(row, paidFields[i]);
      if (includeUsage) { row["usage_requests"] = usage ? *rt::get(*usage, "request_count") : rt::makeNumber(0.0); row["usage"] = usage ? json::value(*usage) : json::value(nullptr); }
      data.push_back(std::move(row));
    }
    const json::object body{{"object", "list"}, {"data", std::move(data)}};
    co_return http::json(200, body, rt::Headers{{"x-uos-upstream", "chatgpt_codex"}});
  }
  rt::Task<rt::Response> handleAdminApiKeysPaidFallbacks(rt::Request req, std::string keyId) { return history(std::move(req), std::move(keyId), std::nullopt); }
  rt::Task<rt::Response> handleAdminApiKeysPaidFallbacks(rt::Request req, std::string keyId, std::shared_ptr<rt::Kv> kvOverride) { return history(std::move(req), std::move(keyId), std::move(kvOverride)); }
}
