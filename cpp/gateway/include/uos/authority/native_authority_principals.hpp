#pragma once
// Port of src/authority/native-authority-principals.ts.
#include "uos/authority/native_authority_paid.hpp"

namespace uos::authority::native_authority_principals {
  class NativeAuthorityPrincipals {
    struct State;
    std::shared_ptr<State> _state;
  public:
    NativeAuthorityPrincipals(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api, native_authority_paid::NativeAuthorityPaid& paid, const native_principal_contracts::NativePrincipalControlInitial& initial, std::function<double()> clock);
    void initialize();
    rt::Task<native_principal_contracts::NativeControlReply> control(native_principal_contracts::NativeControlBinding binding, native_principal_contracts::NativeControlCommand input);
  };
}
