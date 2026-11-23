#pragma once
// Port of src/authority/native-authority.ts.
#include "uos/authority/native_authority_rpc.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace uos::authority::native_authority {
  /** All families share this single injected SQLite transaction domain. Provider and credential I/O stay outside it. */
  class NativeAuthority {
    struct State;
    std::shared_ptr<State> _state;
  public:
    NativeAuthority(native_authority_contracts::NativeSqlStorage storage, native_authority_contracts::NativeAuthorityInitial initial,
                    std::function<double()> clock = {}, std::optional<native_principal_contracts::NativePrincipalControlInitial> control = std::nullopt,
                    std::optional<native_runtime_contracts::NativeRuntimeInitial> runtime = std::nullopt);
    NativeAuthority(native_authority_contracts::NativeSqlStorage storage, native_authority_contracts::NativeAuthorityInitial initial,
                    std::function<double()> clock, std::shared_ptr<native_principal_contracts::NativePrincipalControlInitial> control,
                    std::shared_ptr<native_runtime_contracts::NativeRuntimeInitial> runtime);
    rt::Task<native_principal_contracts::NativeControlReply> control(native_principal_contracts::NativeControlBinding binding, native_principal_contracts::NativeControlCommand command);
    native_authority_rpc::NativeBoundRuntimePort bindAuthenticatedRuntime(native_runtime_contracts::NativeRuntimeBinding binding) const;
    native_authority_rpc::NativeBoundRuntimePort bindAuthenticatedRuntime(const json::value& binding) const;
    native_authority_rpc::NativeRetainedHistoryPort bindRetainedHistory(native_runtime_contracts::NativeRetainedHistoryBinding binding) const;
    native_authority_rpc::NativeRetainedHistoryPort bindRetainedHistory(const json::value& binding) const;
    operator native_authority_rpc::NativeOperationRunner() const;
    operator native_authority_rpc::NativeRetainedHistoryRunner() const;
    operator native_principal_contracts::NativePrincipalControlRunner() const;
  };
}
