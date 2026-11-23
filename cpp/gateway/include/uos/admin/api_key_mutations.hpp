#pragma once
// Port of src/admin/api-key-mutations.ts.
#include "uos/admin/api_key_audit.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

namespace uos::admin::api_key_mutations {
  rt::Task<rt::Response> handleAdminApiKeysUpdate(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeysUpdate(rt::Request req, const api_key_audit::ApiKeyAuditContext& context);
  rt::Task<rt::Response> handleAdminApiKeysRevoke(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeysRevoke(rt::Request req, const api_key_audit::ApiKeyAuditContext& context);
  rt::Task<rt::Response> handleAdminApiKeysUnrevoke(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeysUnrevoke(rt::Request req, const api_key_audit::ApiKeyAuditContext& context);
  rt::Task<rt::Response> handleAdminApiKeysDelete(rt::Request req);
  rt::Task<rt::Response> handleAdminApiKeysDelete(rt::Request req, const api_key_audit::ApiKeyAuditContext& context);
}
