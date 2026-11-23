#pragma once
// Port of src/authority/native-authority-rpc.ts.
#include "uos/authority/native_authority_contracts.hpp"
#include "uos/authority/native_principal_contracts.hpp"
#include "uos/authority/native_runtime_contracts.hpp"
#include "uos/rt/async.hpp"

#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace uos::authority::native_authority_rpc {
  inline const json::object NATIVE_METHOD_ROLES{
    {"openAuthenticatedRequest", "inference"}, {"bindValidatedRequestMetadata", "inference"}, {"reserveApiUsage", "inference"},
    {"recordTransportStarted", "transport"}, {"markApiDispatch", "transport"}, {"releaseApiUndispatched", "transport"},
    {"recordApiDispatchCancellation", "transport"}, {"reserveKernelUsage", "inference"}, {"recordKernelTerminalIntent", "terminal"},
    {"settleKernelUsage", "terminal"}, {"renewKernelReservation", "terminal"}, {"reclaimExpiredKernelReservations", "reconcile"},
    {"setKernelQuota", "quota_admin"}, {"deleteKernelQuota", "quota_admin"}, {"acquireKernelDefaultCutover", "quota_admin"},
    {"selectCodexAccount", "inference"}, {"recordCodexAttemptSignal", "transport"}, {"releaseCodexProbe", "transport"},
    {"electCodexResetProbe", "reset"}, {"refreshCodexAdmission", "transport"}, {"reservePaidAttempt", "inference"},
    {"recordPaidAttemptLifecycle", "transport"}, {"releasePaidBeforeFetch", "transport"}, {"recordVerifiedPaidBilling", "billing"},
    {"deferPaidReconciliation", "reconcile"}, {"claimPaidReconciliation", "reconcile"}, {"finishPaidReconciliation", "reconcile"},
    {"foldPaidBackfillPage", "reconcile"}, {"beginPaidKeyDeletion", "quota_admin"}, {"recordSemanticTerminal", "terminal"},
    {"recordDeliveryObservation", "transport"}, {"readRequestStatus", "observer"}
  };
  std::string nativeCommandBytes(const json::value& value);
  std::string nativeCommandBytes(const json::value* value);
  native_authority_contracts::NativeCommand parseNativeCommand(const json::value& value);
  native_authority_contracts::NativeCommand parseNativeCommand(const json::value* value);
  native_principal_contracts::NativeControlCommand parseNativeControlCommand(const json::value& value);
  native_principal_contracts::NativeControlCommand parseNativeControlCommand(const json::value* value);
  struct NativeBoundRuntimePort {
    std::function<rt::Task<native_authority_contracts::NativeReply>(native_authority_contracts::NativeCommand)> execute;
    std::function<rt::Task<native_authority_contracts::NativeReply>(std::string, std::string)> bindTrustedRoutingDecision;
  };
  struct NativeOperationRunner {
    std::function<NativeBoundRuntimePort(native_runtime_contracts::NativeRuntimeBinding)> bindAuthenticatedRuntime;
  };
  struct NativeRetainedHistoryPort { std::function<rt::Task<native_authority_contracts::NativeReply>(native_authority_contracts::NativeCommand)> execute; };
  struct NativeRetainedHistoryRunner { std::function<NativeRetainedHistoryPort(native_runtime_contracts::NativeRetainedHistoryBinding)> bindRetainedHistory; };
  struct NativeRpcResultResult { json::value result; };
  struct NativeRpcResultError { struct Error { std::string code; double status; }; Error error; };
  using NativeRpcResult = std::variant<NativeRpcResultResult, NativeRpcResultError>;
  struct NativeRpcTransport {
    std::function<rt::Task<NativeRpcResult>(json::value)> execute;
    std::function<rt::Task<NativeRpcResult>(std::string, std::string)> bindTrustedRoutingDecision{};
  };
  class NativeAuthorityRpcServer {
  public:
    NativeAuthorityRpcServer(const NativeOperationRunner& authority, const native_runtime_contracts::NativeRuntimeBinding& binding);
    rt::Task<NativeRpcResult> execute(json::value command);
    rt::Task<NativeRpcResult> bindTrustedRoutingDecision(std::string handle, std::string operationId);
    NativeRpcTransport transport() const;
  private:
    std::shared_ptr<NativeBoundRuntimePort> _port;
  };
  class NativeRetainedHistoryServer {
  public:
    NativeRetainedHistoryServer(const NativeRetainedHistoryRunner& authority, const native_runtime_contracts::NativeRetainedHistoryBinding& binding);
    rt::Task<NativeRpcResult> execute(json::value command);
    NativeRpcTransport transport() const;
  private:
    std::function<rt::Task<native_authority_contracts::NativeReply>(native_authority_contracts::NativeCommand)> _execute;
  };
  class NativeAuthorityClient {
  public:
    NativeAuthorityClient(NativeRpcTransport transport, std::string epoch);
    rt::Task<json::value> bindTrustedRoutingDecision(std::string handle);
    rt::Task<json::value> bindTrustedRoutingDecision(std::string handle, std::string operationId);
    rt::Task<native_authority_contracts::NativeReply> call(std::string method, const json::object& args);
    rt::Task<native_authority_contracts::NativeReply> call(std::string method, const json::object& args, std::string operationId);
    template<class Args> rt::Task<native_authority_contracts::NativeReply> call(std::string method, const Args& args) {
      const auto value = json::value_from(args); return call(std::move(method), value.as_object());
    }
    template<class Args> rt::Task<native_authority_contracts::NativeReply> call(std::string method, const Args& args, std::string operationId) {
      const auto value = json::value_from(args); return call(std::move(method), value.as_object(), std::move(operationId));
    }
  private:
    NativeRpcTransport _transport;
    std::string _epoch;
  };
  using NativeControlRpcResult = NativeRpcResult;
  struct NativeControlRpcTransport { std::function<rt::Task<NativeControlRpcResult>(json::value)> control; };
  class NativePrincipalControlServer {
  public:
    NativePrincipalControlServer(native_principal_contracts::NativePrincipalControlRunner authority, native_principal_contracts::NativeControlBinding binding);
    rt::Task<NativeControlRpcResult> control(json::value command);
    NativeControlRpcTransport transport() const;
  private:
    native_principal_contracts::NativePrincipalControlRunner _authority;
    native_principal_contracts::NativeControlBinding _binding;
  };
  class NativePrincipalControlClient {
  public:
    NativePrincipalControlClient(NativeControlRpcTransport transport, std::string epoch);
    rt::Task<json::value> call(std::string method, const json::object& args);
    rt::Task<json::value> call(std::string method, const json::object& args, std::string operationId);
    template<class Args> rt::Task<json::value> call(std::string method, const Args& args, std::string operationId) {
      const auto value = json::value_from(args); return call(std::move(method), value.as_object(), std::move(operationId));
    }
  private:
    NativeControlRpcTransport _transport;
    std::string _epoch;
  };
}
