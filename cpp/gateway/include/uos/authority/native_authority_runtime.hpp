#pragma once
// Port of src/authority/native-authority-runtime.ts.
#include "uos/authority/native_authority_api.hpp"

namespace uos::authority::native_authority_runtime {
  class NativeAuthorityRuntime {
    native_authority_sql::NativeAuthoritySql& _sql;
    native_authority_api::NativeAuthorityApi& _api;
    const native_runtime_contracts::NativeRuntimeInitial& _initial;
  public:
    NativeAuthorityRuntime(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api, const native_runtime_contracts::NativeRuntimeInitial& initial) : _sql(sql), _api(api), _initial(initial) {}
    void initialize();
    rt::Task<native_runtime_contracts::NativeRuntimeAuthorization> verify(native_runtime_contracts::NativeRuntimeBinding binding, std::string intent, double now);
    native_authority_contracts::NativeCapability authorize(const native_runtime_contracts::NativeRuntimeAuthorization& auth, double now);
    void assertCurrentApi(const native_authority_contracts::NativeCapability& cap, double now) const;
    native_authority_contracts::NativeReply open(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::openAuthenticatedRequest& args, double now);
    native_authority_contracts::NativeReply metadata(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::bindValidatedRequestMetadata& args, double now);
    rt::Task<native_runtime_contracts::NativeTrustedRoutingDecision> routing(native_runtime_contracts::NativeRuntimeBinding binding, native_runtime_contracts::NativeValidatedRequest request, std::string intent, double now);
    void assertRouting(const native_authority_contracts::NativeCapability& cap, const native_runtime_contracts::NativeValidatedRequest& request, const native_runtime_contracts::NativeTrustedRoutingDecision& decision, double now) const;
    native_authority_contracts::NativeReply bindRouting(const native_authority_contracts::NativeCapability& cap, const native_runtime_contracts::NativeValidatedRequest& request, const native_runtime_contracts::NativeTrustedRoutingDecision& decision, double now);
    rt::Task<native_runtime_contracts::VerifiedNativeRetainedHistory> verifyRetained(native_runtime_contracts::NativeRetainedHistoryBinding binding, std::string intent, double now);
    native_authority_contracts::NativeCapability authorizeRetained(const native_runtime_contracts::NativeRetainedHistoryBinding& binding, const native_runtime_contracts::VerifiedNativeRetainedHistory& verified, const native_authority_contracts::NativeCommand& command, double now) const;
  };
}
