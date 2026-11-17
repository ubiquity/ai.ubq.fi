#pragma once
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uos::authority::credential_publication {
  using contracts::AccessAuthorization;
  using contracts::AuthorityScope;
  using contracts::AuthorityState;
  using contracts::AuthorityStore;

  struct CredentialOwnerBinding {
    AuthorityScope scope;
    std::string ownerId;
    double ownerEpoch;
    std::string accountId;
    std::string workspaceId;
    std::string minimumNativeRefreshedAtNs;
  };
  // `AccessAuthorization & Readonly<{ previousGeneration: number }>`.
  struct AccessPublication {
    std::string ownerId;
    double ownerEpoch;
    std::string accountId;
    std::string workspaceId;
    double generation;
    std::string nativeRefreshedAtNs;
    std::string fingerprint;
    std::string accessToken;
    std::string idToken;
    double issuedAtMs;
    double expiresAtMs;
    double previousGeneration;
    // Own property keys the JS object carries beyond the declared members
    // (part of `Reflect.ownKeys(input)`), e.g. a smuggled "refreshToken".
    std::vector<std::string> extraOwnKeys{};
  };
  struct VerifiedAccessIdentity {
    std::string accountId;
    std::string workspaceId;
    std::string fingerprint;
    double issuedAtMs;
    double expiresAtMs;
  };
  using AccessVerifier = std::function<rt::Promise<VerifiedAccessIdentity>(AccessPublication publication)>;

  /** Fixed owner capability; access-only publication has no renewal or native transport API. */
  class CredentialPublisher {
  public:
    CredentialPublisher(std::shared_ptr<AuthorityStore> store_, CredentialOwnerBinding binding_, AccessVerifier verifyAccess_);

    // "published" | "duplicate"
    rt::Task<std::string> publish(AccessPublication input, double nowMs);

  private:
    void _validateBinding(const AccessPublication& input) const;
    void _validateGeneration(const AccessPublication& input, double nowMs) const;
    std::string _applyPublication(AuthorityState& state, const AccessPublication& input, const AccessAuthorization& authorization) const;

    const std::shared_ptr<AuthorityStore> _store;
    const CredentialOwnerBinding _binding;
    const AccessVerifier _verifyAccess;
  };
}
