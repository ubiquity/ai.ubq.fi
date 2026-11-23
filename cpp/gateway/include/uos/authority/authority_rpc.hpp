#pragma once
#include "uos/authority/admission_state.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>

namespace uos::authority::authority_rpc {
  using contracts::AccountBinding;
  using contracts::AccountProof;
  using contracts::AdmissionHandle;
  using contracts::AdmitInput;
  using contracts::AttemptInput;
  using contracts::DeliveryOutcome;
  using contracts::TerminalEvidence;

  /** Public operations only. Storage, policy, publication and seed capabilities stay server-owned. */
  using AuthorityOperations = admission_state::AuthorityOperations;
  // `{ operation, arguments }`. The server receives it as an unknown value and
  // validates its complete shape, so it is carried as JSON.
  using AuthorityCommand = json::value;
  // `{ value: TValue } | { error: { code: string; status: number } }`; an
  // undefined value is a reply without a "value" member.
  using AuthorityReply = json::object;
  class AuthorityRpc {
  public:
    virtual ~AuthorityRpc() = default;
    virtual rt::Task<AuthorityReply> invoke(AuthorityCommand command) = 0;
  };

  /** This is a constructor capability, not a public HTTP endpoint or generic proxy. */
  class AuthorityRpcServer : public AuthorityRpc {
  public:
    AuthorityRpcServer(std::shared_ptr<AuthorityOperations> authority_, double maximumCommandBytes_);
    rt::Task<AuthorityReply> invoke(AuthorityCommand command) override;

  private:
    // The operation's result as JSON; std::nullopt is undefined.
    rt::Task<std::optional<json::value>> _execute(json::object command);

    const std::shared_ptr<AuthorityOperations> _authority;
    const double _maximumCommandBytes;
  };

  class AuthorityRpcClient : public AuthorityOperations {
  public:
    explicit AuthorityRpcClient(std::shared_ptr<AuthorityRpc> rpc_);
    rt::Task<AdmissionHandle> admit(AdmitInput input) override;
    rt::Task<void> reserveAttempt(AdmissionHandle handle, AttemptInput input, double nowMs) override;
    rt::Task<bool> beginDispatch(AdmissionHandle handle, std::string attemptId, double nowMs) override;
    rt::Task<admission_state::Selection> selection(AdmissionHandle handle) override;
    rt::Task<std::optional<AccountBinding>> reselectSubscription(AdmissionHandle handle, double nowMs) override;
    rt::Task<void> confirmDispatched(AdmissionHandle handle, std::string attemptId) override;
    rt::Task<void> releaseBeforeDispatch(AdmissionHandle handle, std::string attemptId) override;
    rt::Task<void> observeProof(AdmissionHandle handle, std::string attemptId, AccountProof proof) override;
    rt::Task<void> observeTerminal(AdmissionHandle handle, std::string attemptId, TerminalEvidence evidence, bool finalAttempt = true) override;
    rt::Task<void> observeDelivery(AdmissionHandle handle, DeliveryOutcome outcome) override;
    rt::Task<void> observeUsage(AdmissionHandle handle, std::string attemptId, admission_state::Usage usage) override;
    rt::Task<bool> resetEligible(AdmissionHandle handle, double nowMs) override;
    rt::Task<std::string> settle(AdmissionHandle handle) override;
    rt::Task<void> cancelUndispatched(AdmissionHandle handle) override;

  private:
    rt::Task<std::optional<json::value>> _call(std::string operation, json::array arguments);

    const std::shared_ptr<AuthorityRpc> _rpc;
  };
}
