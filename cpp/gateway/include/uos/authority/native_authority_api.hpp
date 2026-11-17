#pragma once
// Port of src/authority/native-authority-api.ts.
#include "uos/authority/native_authority_sql.hpp"
#include "uos/authority/native_api_key.hpp"

namespace uos::authority::native_authority_api {
  class NativeAuthorityApi {
    native_authority_sql::NativeAuthoritySql& _sql;
    native_contracts::ApiKeyUsageSnapshot _snapshot(const native_authority_contracts::NativeRequest& row, double now) const;
    void _write(const native_contracts::ApiKeyPolicy& policy, const native_contracts::ApiKeyUsageWritePlan& plan, double now);
    void _reclaim(const native_contracts::ApiKeyPolicy& policy, double now);
    bool _ownsReservation(const native_authority_contracts::NativeRequest& row, const native_contracts::ApiKeyPolicy& policy, double now) const;
    bool _compensationSafe(const native_authority_contracts::NativeRequest& row) const;
    bool _resetReservation(const std::string& keyId, const std::string& encoded, const json::value& value, double now);
  public:
    explicit NativeAuthorityApi(native_authority_sql::NativeAuthoritySql& sql) : _sql(sql) {}
    native_authority_contracts::NativeRequest request(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    void save(const native_authority_contracts::NativeRequest& row, const json::object& rawOverrides = {});
    native_runtime_contracts::NativeValidatedRequest validated(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    native_authority_contracts::NativeReply reserve(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::reserveApiUsage& args, double now);
    native_authority_contracts::NativeReply dispatch(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::markApiDispatch& args, double now);
    void assertDispatchReceipt(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    void assertTransportReceipt(const native_authority_contracts::NativeCapability& cap, const std::string& handle, const std::string& operationId, double now) const;
    native_authority_contracts::NativeReply start(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordTransportStarted& args, const std::string& operationId, double now);
    native_authority_contracts::NativeReply release(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::releaseApiUndispatched& args, double now);
    native_authority_contracts::NativeReply cancel(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply status(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    double usage(const native_contracts::ApiKeyPolicy& policy, double now) const;
    void createWindow(const native_contracts::ApiKeyPolicy& policy, double now);
    bool inspectKeyReset(const std::string& keyId, double now);
    void deleteForKey(const std::string& keyId, double now);
  };
}
