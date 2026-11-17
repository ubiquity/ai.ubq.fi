#pragma once
// Port of src/authority/native-api-key-audit.ts.
#include "uos/auth/index.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>

namespace uos::authority::native_api_key_audit {
  inline const rt::KvKey API_KEY_CHANGE_PREFIX{"uos_ai", "api_keys", "change"};
  inline constexpr double API_KEY_CHANGE_RETENTION_DAYS = 90;
  inline constexpr double API_KEY_CHANGE_RETENTION_MS = API_KEY_CHANGE_RETENTION_DAYS * 24 * 60 * 60 * 1000;
  inline constexpr double API_KEY_CHANGE_DEFAULT_LIMIT = 20;
  inline constexpr double API_KEY_CHANGE_MAX_LIMIT = 50;
  using ApiKeyChangeAction = std::string;
  struct ApiKeyChangeActor {
    std::string kind;
    std::optional<std::string> principal_id;
    std::optional<std::string> principal_label;
    bool is_super_admin;
    friend bool operator==(const ApiKeyChangeActor&, const ApiKeyChangeActor&) = default;
  };
  struct ApiKeyChangeEvent {
    std::string id;
    double at_ms;
    ApiKeyChangeAction action;
    ApiKeyChangeActor actor;
    std::string target_key_id;
    std::optional<std::string> target_key_name;
    json::object changed;
    std::string request_id;
    std::string release;
    friend bool operator==(const ApiKeyChangeEvent&, const ApiKeyChangeEvent&) = default;
  };
  struct ApiKeyAuditContext {
    std::optional<auth::index::AdminAuthResult> auth{};
    std::optional<std::string> request_id{};
  };
  json::object sanitizeApiKeyChangeFields(const json::object& value);
  ApiKeyChangeActor apiKeyChangeActorFromAuth();
  ApiKeyChangeActor apiKeyChangeActorFromAuth(const std::optional<auth::index::AdminAuthResult>& auth);
  struct ApiKeyChangeEventInput {
    ApiKeyChangeAction action;
    std::string targetKeyId;
    rt::Nullable<std::string> targetKeyName{};
    std::optional<json::object> changed{};
    std::optional<ApiKeyAuditContext> context{};
    std::optional<double> nowMs{};
  };
  struct ApiKeyAuditSources {
    std::function<std::string()> randomUUID;
    std::function<double()> now;
    std::function<std::string()> release;
  };
  ApiKeyChangeEvent buildApiKeyChangeEvent(const ApiKeyChangeEventInput& input, const ApiKeyAuditSources& sources);
  struct ApiKeyChangeEventIdentity { double at_ms; std::string id; };
  rt::KvKey apiKeyChangeEventKey(const ApiKeyChangeEventIdentity& event);
  rt::KvKey apiKeyChangeEventKey(const ApiKeyChangeEvent& event);
  bool isRetainedApiKeyChangeEvent(const json::value& value, double oldestRetainedAtMs);
  bool isRetainedApiKeyChangeEvent(const json::value* value, double oldestRetainedAtMs);
  bool isRetainedApiKeyChangeEvent(const rt::KvValue& value, double oldestRetainedAtMs);
  std::optional<double> parseApiKeyChangeLimit(const std::optional<std::string>& raw);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyChangeActor& value);
  ApiKeyChangeActor tag_invoke(const json::value_to_tag<ApiKeyChangeActor>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyChangeEvent& value);
  ApiKeyChangeEvent tag_invoke(const json::value_to_tag<ApiKeyChangeEvent>&, const json::value& value);
}
