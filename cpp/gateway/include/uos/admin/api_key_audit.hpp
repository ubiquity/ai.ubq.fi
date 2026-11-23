#pragma once
// Port of src/admin/api-key-audit.ts.
#include "uos/authority/native_api_key_audit.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::admin::api_key_audit {
  using authority::native_api_key_audit::API_KEY_CHANGE_PREFIX;
  using authority::native_api_key_audit::API_KEY_CHANGE_RETENTION_DAYS;
  using authority::native_api_key_audit::API_KEY_CHANGE_RETENTION_MS;
  using authority::native_api_key_audit::ApiKeyChangeEvent;
  using authority::native_api_key_audit::ApiKeyAuditContext;
  using authority::native_api_key_audit::ApiKeyChangeEventInput;
  ApiKeyChangeEvent buildApiKeyChangeEvent(const ApiKeyChangeEventInput& input);
  rt::KvKey apiKeyChangeEventKey(const ApiKeyChangeEvent& event);
  rt::AtomicOperation& appendApiKeyChangeEvent(rt::AtomicOperation& atomic, const ApiKeyChangeEvent& event);
  struct ApiKeyChangePage { std::vector<json::value> events; std::optional<std::string> next_cursor; };
  struct ListApiKeyChangeEventsInput { double limit; std::optional<std::string> cursor{}; };
  rt::Task<ApiKeyChangePage> listApiKeyChangeEvents(std::shared_ptr<rt::Kv> kv, const ListApiKeyChangeEventsInput& input);
  rt::Task<rt::Response> handleAdminApiKeyChanges(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeyChanges(rt::Request req, std::shared_ptr<rt::Kv> kvOverride);
}
