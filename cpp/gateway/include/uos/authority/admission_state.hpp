#pragma once
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::authority::admission_state {
  using contracts::AccountBinding;
  using contracts::AccountProof;
  using contracts::AdmissionHandle;
  using contracts::AdmissionRecord;
  using contracts::AdmitInput;
  using contracts::AttemptInput;
  using contracts::AuthorityScope;
  using contracts::AuthorityState;
  using contracts::AuthorityStore;
  using contracts::DeliveryOutcome;
  using contracts::PrincipalPolicy;
  using contracts::TerminalEvidence;

  AuthorityState createAuthorityState(const AuthorityScope& scope, const std::vector<AccountBinding>& accounts, const std::vector<PrincipalPolicy>& policies);

  // `selection()`'s result: `{ account, model, route }`.
  struct Selection {
    std::optional<AccountBinding> account;
    std::string model;
    contracts::GatewayRoute route;
    friend bool operator==(const Selection&, const Selection&) = default;
  };

  // `observeUsage()`'s evidence: `{ providerRequestId, spendMicrocredits }`.
  struct Usage {
    std::string providerRequestId;
    double spendMicrocredits;
  };

  // authority-rpc's `AuthorityOperations` (`Pick<PortableAuthority, keyof
  // PortableAuthority>`): the public operations of PortableAuthority as an
  // interface, declared here so PortableAuthority, AuthorityRpcClient and test
  // doubles can implement it.
  class AuthorityOperations {
  public:
    virtual ~AuthorityOperations() = default;
    virtual rt::Task<AdmissionHandle> admit(AdmitInput input) = 0;
    virtual rt::Task<void> reserveAttempt(AdmissionHandle handle, AttemptInput input, double nowMs) = 0;
    virtual rt::Task<bool> beginDispatch(AdmissionHandle handle, std::string attemptId, double nowMs) = 0;
    virtual rt::Task<Selection> selection(AdmissionHandle handle) = 0;
    virtual rt::Task<std::optional<AccountBinding>> reselectSubscription(AdmissionHandle handle, double nowMs) = 0;
    virtual rt::Task<void> confirmDispatched(AdmissionHandle handle, std::string attemptId) = 0;
    virtual rt::Task<void> releaseBeforeDispatch(AdmissionHandle handle, std::string attemptId) = 0;
    virtual rt::Task<void> observeProof(AdmissionHandle handle, std::string attemptId, AccountProof proof) = 0;
    virtual rt::Task<void> observeTerminal(AdmissionHandle handle, std::string attemptId, TerminalEvidence evidence, bool finalAttempt = true) = 0;
    virtual rt::Task<void> observeDelivery(AdmissionHandle handle, DeliveryOutcome outcome) = 0;
    virtual rt::Task<void> observeUsage(AdmissionHandle handle, std::string attemptId, Usage usage) = 0;
    virtual rt::Task<bool> resetEligible(AdmissionHandle handle, double nowMs) = 0;
    // "settled" | "pending"
    virtual rt::Task<std::string> settle(AdmissionHandle handle) = 0;
    virtual rt::Task<void> cancelUndispatched(AdmissionHandle handle) = 0;
  };

  /** A transaction protocol, not a live storage adapter or an independent Worker ledger. */
  class PortableAuthority : public AuthorityOperations {
  public:
    PortableAuthority(std::shared_ptr<AuthorityStore> store_, AuthorityScope scope_);

    rt::Task<AdmissionHandle> admit(AdmitInput input) override;
    rt::Task<void> reserveAttempt(AdmissionHandle handle, AttemptInput input, double nowMs) override;
    rt::Task<bool> beginDispatch(AdmissionHandle handle, std::string attemptId, double nowMs) override;
    rt::Task<Selection> selection(AdmissionHandle handle) override;
    /** Move a completed subscription hop without opening another principal admission. */
    rt::Task<std::optional<AccountBinding>> reselectSubscription(AdmissionHandle handle, double nowMs) override;
    rt::Task<void> confirmDispatched(AdmissionHandle handle, std::string attemptId) override;
    rt::Task<void> releaseBeforeDispatch(AdmissionHandle handle, std::string attemptId) override;
    rt::Task<void> observeProof(AdmissionHandle handle, std::string attemptId, AccountProof proof) override;
    rt::Task<void> observeTerminal(AdmissionHandle handle, std::string attemptId, TerminalEvidence evidence, bool finalAttempt = true) override;
    rt::Task<void> observeDelivery(AdmissionHandle handle, DeliveryOutcome outcome) override;
    rt::Task<void> observeUsage(AdmissionHandle handle, std::string attemptId, Usage usage) override;
    rt::Task<bool> resetEligible(AdmissionHandle handle, double nowMs) override;
    rt::Task<std::string> settle(AdmissionHandle handle) override;
    rt::Task<void> cancelUndispatched(AdmissionHandle handle) override;

  private:
    template <class T, class Action> rt::Task<T> _transact(AuthorityScope scope, Action action);
    AdmissionRecord& _request(AuthorityState& state, const AdmissionHandle& handle);

    const std::shared_ptr<AuthorityStore> _store;
    const AuthorityScope _scope;
  };
}
