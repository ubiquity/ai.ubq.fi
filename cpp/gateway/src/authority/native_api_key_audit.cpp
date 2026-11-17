// Port of src/authority/native-api-key-audit.ts.
#include "uos/authority/native_api_key_audit.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <array>

namespace uos::authority::native_api_key_audit {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    constexpr std::array<std::string_view, 8> SAFE_CHANGED_FIELDS = {"name", "expires_at_ms", "usage_limit_requests", "window_ms", "paid_fallback_enabled", "paid_fallback_limit_credits", "reset_usage", "revoked_at_ms"};
    std::optional<json::value> sanitizeChangedValue(const json::value& value) {
      if (value.is_null() || value.is_bool()) return value;
      if (rt::isNumber(value)) return rt::Number::isFinite(value) ? std::optional(value) : std::nullopt;
      if (value.is_string()) {
        const std::string text(value.as_string());
        return json::value(rt::length(text) > 200 ? rt::slice(text, 0, 200) : text);
      }
      return std::nullopt;
    }
  }
  json::object sanitizeApiKeyChangeFields(const json::object& value) {
    json::object changed;
    for (const auto& field : SAFE_CHANGED_FIELDS) {
      const auto* raw = rt::get(value, field);
      if (!raw) continue;
      const auto sanitized = sanitizeChangedValue(*raw);
      if (sanitized) rt::objectSet(changed, field, *sanitized);
    }
    return changed;
  }
  ApiKeyChangeActor apiKeyChangeActorFromAuth() { return apiKeyChangeActorFromAuth(std::nullopt); }
  ApiKeyChangeActor apiKeyChangeActorFromAuth(const std::optional<auth::index::AdminAuthResult>& auth) {
    const auto* authenticated = auth ? std::get_if<auth::index::AdminAuthResultOk>(&*auth) : nullptr;
    if (!authenticated) return {"unknown", std::nullopt, std::nullopt, false};
    const bool isSuperAdmin = authenticated->is_super_admin;
    if (const auto* passkey = std::get_if<auth::index::AdminAuthMethodPasskeySession>(&authenticated->method))
      return {"passkey_user", passkey->user_id, passkey->handle, isSuperAdmin};
    if (const auto* passport = std::get_if<auth::index::PassportAuthMethod>(&authenticated->method))
      return {"passkey_user", passport->user.sub, passport->user.handle, isSuperAdmin};
    if (std::holds_alternative<auth::index::AdminAuthMethodDisabled>(authenticated->method)) return {"local_admin", std::nullopt, std::nullopt, isSuperAdmin};
    return {"shared_admin_credential", std::nullopt, std::nullopt, isSuperAdmin};
  }
  ApiKeyChangeEvent buildApiKeyChangeEvent(const ApiKeyChangeEventInput& input, const ApiKeyAuditSources& sources) {
    const auto id = sources.randomUUID();
    const auto requestId = input.context && input.context->request_id ? rt::trim(*input.context->request_id) : std::string();
    return {.id = id, .at_ms = input.nowMs ? *input.nowMs : sources.now(), .action = input.action,
      .actor = input.context ? apiKeyChangeActorFromAuth(input.context->auth) : apiKeyChangeActorFromAuth(), .target_key_id = input.targetKeyId,
      .target_key_name = input.targetKeyName.hasValue() && !input.targetKeyName->empty() ? std::optional(rt::slice(*input.targetKeyName, 0, 200)) : std::nullopt,
      .changed = sanitizeApiKeyChangeFields(input.changed.value_or(json::object())), .request_id = requestId.empty() ? id : requestId, .release = sources.release()};
  }
  rt::KvKey apiKeyChangeEventKey(const ApiKeyChangeEventIdentity& event) { return rt::KvKey{API_KEY_CHANGE_PREFIX, rt::Number::MAX_SAFE_INTEGER - event.at_ms, event.id}; }
  rt::KvKey apiKeyChangeEventKey(const ApiKeyChangeEvent& event) { return apiKeyChangeEventKey(ApiKeyChangeEventIdentity{event.at_ms, event.id}); }
  bool isRetainedApiKeyChangeEvent(const json::value& value, double oldestRetainedAtMs) { return isRetainedApiKeyChangeEvent(&value, oldestRetainedAtMs); }
  bool isRetainedApiKeyChangeEvent(const json::value* value, double oldestRetainedAtMs) {
    if (!utils::isRecord(value)) return false;
    const auto at = rt::getNumber(*value, "at_ms");
    return at && *at >= oldestRetainedAtMs;
  }
  bool isRetainedApiKeyChangeEvent(const rt::KvValue& value, double oldestRetainedAtMs) {
    const auto view = rt::storedJsonView(value);
    return isRetainedApiKeyChangeEvent(view, oldestRetainedAtMs);
  }
  std::optional<double> parseApiKeyChangeLimit(const std::optional<std::string>& raw) {
    static const rt::Regex digits("^\\d+$", "");
    if (raw && !digits.test(rt::trim(*raw))) return std::nullopt;
    const double requested = raw ? rt::stringToNumber(*raw) : API_KEY_CHANGE_DEFAULT_LIMIT;
    if (!rt::Number::isSafeInteger(requested) || requested < 1) return std::nullopt;
    return rt::Math::min(requested, API_KEY_CHANGE_MAX_LIMIT);
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyChangeActor& value) {
    out = Writer().required("kind", value.kind).orNull("principal_id", value.principal_id).orNull("principal_label", value.principal_label).required("is_super_admin", value.is_super_admin).done();
  }
  ApiKeyChangeActor tag_invoke(const json::value_to_tag<ApiKeyChangeActor>&, const json::value& value) {
    const Reader in(value, "ApiKeyChangeActor");
    return {in.required<std::string>("kind"), in.orNull<std::string>("principal_id"), in.orNull<std::string>("principal_label"), in.required<bool>("is_super_admin")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyChangeEvent& value) {
    out = Writer().required("id", value.id).required("at_ms", value.at_ms).required("action", value.action).required("actor", value.actor).required("target_key_id", value.target_key_id).orNull("target_key_name", value.target_key_name).required("changed", value.changed).required("request_id", value.request_id).required("release", value.release).done();
  }
  ApiKeyChangeEvent tag_invoke(const json::value_to_tag<ApiKeyChangeEvent>&, const json::value& value) {
    const Reader in(value, "ApiKeyChangeEvent");
    return {in.required<std::string>("id"), in.required<double>("at_ms"), in.required<std::string>("action"), in.required<ApiKeyChangeActor>("actor"), in.required<std::string>("target_key_id"), in.orNull<std::string>("target_key_name"), in.required<json::object>("changed"), in.required<std::string>("request_id"), in.required<std::string>("release")};
  }
}
