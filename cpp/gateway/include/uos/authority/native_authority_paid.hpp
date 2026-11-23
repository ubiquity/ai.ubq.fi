#pragma once
// Port of src/authority/native-authority-paid.ts.
#include "uos/authority/native_authority_api.hpp"
#include "uos/authority/native_paid.hpp"

namespace uos::authority::native_authority_paid {
  struct NativePaidResponse { native_authority_contracts::NativeAttempt attempt; std::string provider; };
  struct NativePaidDeletionResult { std::string kind; std::optional<native_paid_contracts::PaidFallbackOutstandingV3> outstanding{}; json::object deleted{}; };
  class NativeAuthorityPaid {
    native_authority_sql::NativeAuthoritySql& _sql;
    native_authority_api::NativeAuthorityApi& _api;
    native_authority_contracts::NativeProviderHop _hop(const std::string& id) const;
    void _saveAttempt(const native_authority_contracts::NativeAttempt& row);
    native_paid_contracts::NativePaidRow<native_paid_contracts::PaidFallbackRequestV3> _request(const native_authority_contracts::NativeAttempt& attempt, double now) const;
    native_paid_contracts::NativePaidRow<json::value> _gate(double now) const;
    void _writeRequest(const native_authority_contracts::NativeKey& key, const native_paid_contracts::PaidFallbackRequestV3& row, double expiry);
    void _arm(double now);
    std::optional<native_authority_contracts::NativeRead<native_authority_contracts::NativeKeyPolicy>> _policy(const native_authority_contracts::NativeCapability& cap, double now) const;
    bool _fallbackCurrent(const native_authority_contracts::NativeRequest& request, double now) const;
    bool _fallbackAllowed(const native_authority_contracts::NativeRequest& request, double now) const;
    struct Prior { std::optional<native_authority_contracts::NativeAttempt> prior; std::optional<native_authority_contracts::NativeReply> blocked; };
    Prior _prior(const native_authority_contracts::NativeRequest& request, const std::string& hopId, const std::string& billingId, double now) const;
    native_authority_contracts::NativeAttempt _requireAttempt(const std::optional<native_authority_contracts::NativeAttempt>& prior) const;
    void _expedite(const native_authority_contracts::NativeAttempt& attempt, double now);
    void _assertFetchStart(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeAttempt& attempt, std::optional<bool> fetchStarted, double now) const;
    void _applyRelease(const native_authority_contracts::NativeAttempt& attempt, const native_paid_contracts::NativePaidReleasePlan& plan, double now);
    void _cleanup(const native_authority_contracts::NativeAttempt& attempt, double now);
    std::string _resume(const native_authority_contracts::NativeCapability& cap, const std::optional<std::string>& cursor, const std::string& family, double now) const;
    bool _ownsCursor(const native_authority_contracts::NativeCapability& cap, const std::optional<native_authority_contracts::NativeCursor>& row) const;
    void _cursorScope(const native_authority_contracts::NativeCapability& cap, native_authority_contracts::NativeCursor& row) const;
    std::string _cursor(const native_authority_contracts::NativeCapability& cap, const std::string& family, const std::string& after, const std::optional<std::string>& lease);
    std::optional<json::object> _dueAttempt(const native_authority_contracts::NativeCapability& cap, const std::string& billingId, double now);
    double _backfillRow(const native_authority_contracts::NativeKey& key, const native_paid_contracts::PaidFallbackRequestV3& request, double now);
    native_paid_contracts::PaidFallbackOutstandingV3 _outstanding(const std::string& keyId, double now) const;
  public:
    NativeAuthorityPaid(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api) : _sql(sql), _api(api) {}
    native_authority_contracts::NativeAttempt attempt(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    native_authority_contracts::NativeReply reserve(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::reservePaidAttempt& args, double now);
    NativePaidResponse response(const native_authority_contracts::NativeCapability& cap, const std::string& attemptHandle, double now) const;
    std::optional<native_authority_contracts::NativeAttempt> assertStart(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordTransportStarted& args, double now) const;
    void start(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordTransportStarted& args, double now);
    native_authority_contracts::NativeReply lifecycle(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordPaidAttemptLifecycle& args, double now);
    native_authority_contracts::NativeReply release(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply defer(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply billing(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordVerifiedPaidBilling& args, double now);
    native_authority_contracts::NativeReply claim(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::claimPaidReconciliation& args, double now);
    native_authority_contracts::NativeReply finish(const native_authority_contracts::NativeCapability& cap, const std::string& token, double now);
    native_authority_contracts::NativeReply backfill(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::foldPaidBackfillPage& args, double now);
    NativePaidDeletionResult deleteForKey(const std::string& keyId, double now);
    native_authority_contracts::NativeReply deletion(const native_authority_contracts::NativeCapability& cap, double now);
  };
}
