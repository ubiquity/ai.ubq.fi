#pragma once
// Port of src/authority/native-authority-kernel.ts.
#include "uos/authority/native_authority_api.hpp"
#include "uos/authority/native_kernel.hpp"

namespace uos::authority::native_authority_kernel {
  class NativeAuthorityKernel {
    native_authority_sql::NativeAuthoritySql& _sql;
    native_authority_api::NativeAuthorityApi& _api;
    const native_authority_contracts::NativeAuthorityInitial& _initial;
    struct Window { native_kernel_contracts::KernelQuotaRead<> row; std::optional<native_kernel_contracts::KernelQuotaWindowV2> value; };
    native_kernel_contracts::KernelQuotaDefaults _defaults(double now) const;
    Window _readWindow(const native_kernel_contracts::KernelQuotaIdentity& identity, double now) const;
    native_kernel_contracts::KernelQuotaRead<> _reservation(const native_kernel_contracts::KernelQuotaReservationIdentity& identity, double now) const;
    void _writeReservation(const native_kernel_contracts::KernelQuotaIdentity& identity, const native_kernel_contracts::KernelQuotaReservationRowV2& row, double expiry);
    double _reclaimWindow(const native_kernel_contracts::KernelQuotaIdentity& identity, double now);
    native_kernel_contracts::KernelQuotaReservationIdentity _identity(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    native_authority_contracts::NativeReply _settle(const native_kernel_contracts::KernelQuotaReservationIdentity& identity, const std::string& target, const std::string& reason, double now);
    native_authority_contracts::NativeReply _setPolicy(const native_kernel_contracts::KernelQuotaIdentity& identity, const native_authority_contracts::NativeArguments::setKernelQuota& args, const native_kernel_contracts::KernelQuotaEffectivePolicy& current, std::optional<native_kernel_contracts::KernelQuotaWindowV2> window, double now);
    native_authority_contracts::NativeReply _policy(const native_authority_contracts::NativeCapability& cap, const std::string& scope, const std::optional<native_authority_contracts::NativeArguments::setKernelQuota>& args, double now);
  public:
    NativeAuthorityKernel(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api, const native_authority_contracts::NativeAuthorityInitial& initial) : _sql(sql), _api(api), _initial(initial) {}
    native_authority_contracts::NativeReply reserve(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply intent(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordKernelTerminalIntent& args, double now);
    native_authority_contracts::NativeReply settle(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::settleKernelUsage& args, double now);
    native_authority_contracts::NativeReply renew(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply reclaim(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::reclaimExpiredKernelReservations& args, double now);
    native_authority_contracts::NativeReply cutover(double now);
    native_authority_contracts::NativeReply policy(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::setKernelQuota& args, double now);
    native_authority_contracts::NativeReply policy(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::deleteKernelQuota& args, double now);
  };
}
