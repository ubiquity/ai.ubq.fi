#include "uos/authority/credential_publication.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace uos::authority::credential_publication {
  using contracts::AuthorityError;
  using contracts::AuthoritySnapshot;
  using contracts::lookup;
  using contracts::sameEvidence;
  using contracts::sameScope;
  using contracts::toJson;

  namespace {
    constexpr std::array<std::string_view, 12> FIELDS = {
      "ownerId",
      "ownerEpoch",
      "accountId",
      "workspaceId",
      "generation",
      "previousGeneration",
      "nativeRefreshedAtNs",
      "fingerprint",
      "accessToken",
      "idToken",
      "issuedAtMs",
      "expiresAtMs",
    };

    // `Reflect.ownKeys(input)`: the declared members, then any extra own keys.
    std::vector<std::string> ownKeys(const AccessPublication& input) {
      std::vector<std::string> keys{"ownerId",     "ownerEpoch",  "accountId", "workspaceId", "generation", "nativeRefreshedAtNs",
                                    "fingerprint", "accessToken", "idToken",   "issuedAtMs",  "expiresAtMs", "previousGeneration"};
      keys.insert(keys.end(), input.extraOwnKeys.begin(), input.extraOwnKeys.end());
      return keys;
    }

    bool isField(const std::string& key) { return std::find(FIELDS.begin(), FIELDS.end(), key) != FIELDS.end(); }
  }

  CredentialPublisher::CredentialPublisher(std::shared_ptr<AuthorityStore> store_, CredentialOwnerBinding binding_, AccessVerifier verifyAccess_)
    : _store(std::move(store_)), _binding(std::move(binding_)), _verifyAccess(std::move(verifyAccess_)) {}

  void CredentialPublisher::_validateBinding(const AccessPublication& input) const {
    const std::vector<std::string> keys = ownKeys(input);
    if (std::any_of(keys.begin(), keys.end(), [](const std::string& key) { return !isField(key); })) throw AuthorityError("unexpected_credential_field", 400);
    if (input.ownerId != _binding.ownerId || input.ownerEpoch != _binding.ownerEpoch || input.accountId != _binding.accountId ||
        input.workspaceId != _binding.workspaceId)
      throw AuthorityError("credential_owner_binding_mismatch", 403);
  }

  void CredentialPublisher::_validateGeneration(const AccessPublication& input, double nowMs) const {
    static const rt::Regex DIGITS("^\\d+$");
    if (!rt::Number::isSafeInteger(input.generation) || input.generation < 1 || input.previousGeneration != input.generation - 1 ||
        !DIGITS.test(input.nativeRefreshedAtNs) || input.accessToken.empty() || input.idToken.empty() || input.fingerprint.empty() ||
        !rt::Number::isSafeInteger(input.expiresAtMs) || !rt::Number::isSafeInteger(input.issuedAtMs) || input.issuedAtMs > nowMs ||
        input.expiresAtMs <= nowMs + 120'000)
      throw AuthorityError("credential_generation_invalid", 403);
  }

  std::string CredentialPublisher::_applyPublication(AuthorityState& state, const AccessPublication& input, const AccessAuthorization& authorization) const {
    const AccessAuthorization* current = lookup(state.publications, input.accountId);
    std::ptrdiff_t accountIndex = -1;
    for (std::size_t position = 0; position < state.accounts.size(); ++position) {
      if (state.accounts[position].accountId == input.accountId) {
        accountIndex = static_cast<std::ptrdiff_t>(position);
        break;
      }
    }
    if (accountIndex < 0) throw AuthorityError("credential_account_missing", 403);
    const contracts::AccountBinding account = state.accounts[static_cast<std::size_t>(accountIndex)];
    if (current != nullptr && current->generation == input.generation) {
      if (!sameEvidence(toJson(*current), toJson(authorization))) throw AuthorityError("credential_generation_conflict");
      return "duplicate";
    }
    if (current != nullptr &&
        (current->ownerId != _binding.ownerId || current->ownerEpoch != _binding.ownerEpoch || current->workspaceId != _binding.workspaceId))
      throw AuthorityError("credential_owner_binding_mismatch", 403);
    if ((current != nullptr ? current->generation : account.credentialGeneration) != input.previousGeneration ||
        (current != nullptr && (rt::BigInt::fromString(input.nativeRefreshedAtNs) <= rt::BigInt::fromString(current->nativeRefreshedAtNs) ||
                                input.expiresAtMs < current->expiresAtMs)))
      throw AuthorityError("credential_generation_conflict");
    state.publications.set(input.accountId, authorization);
    contracts::AccountBinding published = account;
    published.fingerprint = input.fingerprint;
    published.credentialGeneration = input.generation;
    published.proof = std::nullopt;
    state.accounts[static_cast<std::size_t>(accountIndex)] = std::move(published);
    // A same-account publication keeps the durable global account generation.
    if (state.active && state.active->accountId == input.accountId) state.active->fingerprint = input.fingerprint;
    return "published";
  }

  rt::Task<std::string> CredentialPublisher::publish(AccessPublication input, double nowMs) {
    _validateBinding(input);
    _validateGeneration(input, nowMs);
    rt::Promise<VerifiedAccessIdentity> verifying = _verifyAccess(input);
    const VerifiedAccessIdentity identity = co_await verifying.wait();
    if (identity.accountId != input.accountId || identity.workspaceId != input.workspaceId || identity.fingerprint != input.fingerprint ||
        identity.issuedAtMs != input.issuedAtMs || identity.expiresAtMs != input.expiresAtMs ||
        rt::BigInt::fromString(input.nativeRefreshedAtNs) <= rt::BigInt::fromString(_binding.minimumNativeRefreshedAtNs))
      throw AuthorityError("credential_identity_mismatch", 403);
    const AccessAuthorization authorization{
      .ownerId = input.ownerId,
      .ownerEpoch = input.ownerEpoch,
      .accountId = input.accountId,
      .workspaceId = input.workspaceId,
      .generation = input.generation,
      .nativeRefreshedAtNs = input.nativeRefreshedAtNs,
      .fingerprint = input.fingerprint,
      .accessToken = input.accessToken,
      .idToken = input.idToken,
      .issuedAtMs = input.issuedAtMs,
      .expiresAtMs = input.expiresAtMs,
    };
    for (int attempt = 0; attempt < 8; attempt += 1) {
      rt::Promise<AuthoritySnapshot> reading = _store->read();
      const AuthoritySnapshot snapshot = co_await reading.wait();
      if (!sameScope(snapshot.state.scope, _binding.scope)) throw AuthorityError("namespace_or_epoch_mismatch", 403);
      AuthorityState state = snapshot.state;
      std::string outcome = _applyPublication(state, input, authorization);
      rt::Promise<bool> swapping = _store->compareAndSwap(snapshot.revision, state);
      const bool swapped = co_await swapping.wait();
      if (swapped) co_return outcome;
    }
    throw AuthorityError("concurrent_update", 503);
  }
}
