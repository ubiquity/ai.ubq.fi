#pragma once
// Port of src/admin/api-keys.ts.
#include "uos/admin/api_key_audit.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/defaults.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::admin::api_keys {
  inline constexpr double MAX_KV_BYTES = 65536;
  inline constexpr double SAFE_KV_BYTES = 60000;
  using authority::native_api_key_policy::normalizeApiKeyName;
  bool shouldIncludeUsage(const std::optional<std::string>& value);
  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value* value, double nowMs);
  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value& value, double nowMs);
  std::optional<double> normalizeApiKeyUsageLimit(const json::value* value);
  std::optional<double> normalizeApiKeyUsageLimit(const json::value& value);
  std::optional<double> normalizeKernelWindowMsInput(const json::value* value);
  std::optional<double> normalizeKernelWindowMsInput(const json::value& value);
  rt::Response paidFallbackInputError(const std::string& message);
  std::optional<rt::Response> rejectRetiredApiKeyResetSetting(const json::object& raw);
  rt::Response paidFallbackInitializationError(rt::Reason error);
  rt::Task<json::object> paidFallbackPublicFields(::uos::types::ApiKeyRecord record, std::shared_ptr<rt::Kv> kv);
  rt::Task<json::object> paidFallbackPublicFields(::uos::types::ApiKeyRecord record, std::shared_ptr<rt::Kv> kv, double windowResetAtMs);
  std::optional<std::string> normalizeKernelRepoPart(const json::value* value);
  std::optional<std::string> normalizeKernelRepoPart(const json::value& value);
  std::optional<double> normalizeKernelUsageLimitInput(const json::value* value);
  std::optional<double> normalizeKernelUsageLimitInput(const json::value& value);
  std::optional<double> normalizeKernelExpiresAtMsInput(const json::value* value, double nowMs);
  std::optional<double> normalizeKernelExpiresAtMsInput(const json::value& value, double nowMs);
  std::string normalizeKernelScope(const json::value* value);
  std::string normalizeKernelScope(const json::value& value);
  std::vector<::uos::defaults::ReasoningEffort> extractModelReasoningLevels(const std::optional<json::object>& model);
  std::optional<double> estimateJsonSize(const json::value& value);
  std::optional<double> estimateJsonSize(const json::value* value);
  std::optional<double> estimateJsonSize(const json::object& value);
  std::optional<double> estimateJsonSize(const json::array& value);
  std::optional<double> estimateJsonSize(const rt::KvValue& value);
  rt::Task<rt::Response> handleAdminApiKeysCreate(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeysCreate(rt::Request req, const api_key_audit::ApiKeyAuditContext& context);
  rt::Task<rt::Response> handleAdminApiKeysList(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeysPaidFallbacks(rt::Request req, std::string keyId);
  rt::Task<rt::Response> handleAdminApiKeysPaidFallbacks(rt::Request req, std::string keyId, std::shared_ptr<rt::Kv> kvOverride);
}
