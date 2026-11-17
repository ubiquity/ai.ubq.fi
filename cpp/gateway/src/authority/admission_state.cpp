#include "uos/authority/admission_state.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace uos::authority::admission_state {
  using contracts::ActiveAccount;
  using contracts::AttemptRecord;
  using contracts::AuthorityError;
  using contracts::AuthoritySnapshot;
  using contracts::PrincipalWindow;
  using contracts::assertAmount;
  using contracts::lookup;
  using contracts::sameEvidence;
  using contracts::sameScope;
  using contracts::toJson;

  namespace {
    PrincipalWindow emptyWindow() {
      return PrincipalWindow{
        .reserved = 0,
        .dispatched = 0,
        .kernelReserved = 0,
        .kernelCompleted = 0,
        .active = 0,
        .paidReservedMicrocredits = 0,
        .paidSettledMicrocredits = 0,
      };
    }

    // A member access on `state.windows[request.windowKey]` when the state holds
    // no such window: JavaScriptCore's TypeError, naming the expression it
    // evaluated.
    PrincipalWindow& existingWindow(PrincipalWindow* window, const char* evaluating) {
      if (window == nullptr) throw rt::TypeError(std::string("undefined is not an object (evaluating '") + evaluating + "')");
      return *window;
    }

    // `JSON.stringify([principalId, windowResetAtMs])`
    std::string windowKeyFor(const std::string& principalId, double windowResetAtMs) {
      json::array key;
      key.emplace_back(principalId);
      key.push_back(rt::makeNumber(windowResetAtMs));
      return rt::jsonStringify(key);
    }

    bool includes(const std::vector<std::string>& values, const std::string& value) { return std::find(values.begin(), values.end(), value) != values.end(); }

    std::optional<AccountProof> applicableProof(const AccountBinding& account, const std::string& model, double now) {
      const std::optional<AccountProof>& proof = account.proof;
      return proof && proof->fingerprint == account.fingerprint && proof->model == model && proof->observedAtMs <= now && proof->validUntilMs > now ? proof : std::nullopt;
    }

    std::vector<AccountBinding> cohort(const AuthorityState& state, const std::string& model) {
      std::vector<AccountBinding> selected;
      for (const AccountBinding& account : state.accounts) {
        if (account.enabled && includes(account.models, model)) selected.push_back(account);
      }
      return selected;
    }

    bool unavailable(const AccountBinding& account, const std::string& model, double now) {
      const std::optional<AccountProof> proof = applicableProof(account, model, now);
      return proof && (proof->kind == "quota_exhausted" || proof->kind == "credential_invalid");
    }

    bool allUnavailable(const std::vector<AccountBinding>& selected, const std::string& model, double now) {
      return std::all_of(selected.begin(), selected.end(), [&](const AccountBinding& account) { return unavailable(account, model, now); });
    }

    std::optional<AccountBinding> selectAccount(AuthorityState& state, const std::string& model, double now) {
      const std::vector<AccountBinding> selected = cohort(state, model);
      std::optional<AccountBinding> current;
      for (const AccountBinding& account : state.accounts) {
        if (account.enabled && state.active && account.accountId == state.active->accountId) {
          current = account;
          break;
        }
      }
      if (current) {
        // Missing model support or a transient circuit never retires the global account.
        if (!includes(current->models, model)) throw AuthorityError("active_model_unavailable", 503);
        const std::optional<AccountProof> proof = applicableProof(*current, model, now);
        if (proof && proof->kind == "transient") throw AuthorityError("routing_unavailable", 503);
        if (!unavailable(*current, model, now)) {
          state.active = ActiveAccount{.accountId = current->accountId, .fingerprint = current->fingerprint, .generation = state.active ? state.active->generation : 1};
          return current;
        }
      }
      for (const AccountBinding& next : selected) {
        if (!applicableProof(next, model, now)) {
          state.active = ActiveAccount{.accountId = next.accountId, .fingerprint = next.fingerprint, .generation = (state.active ? state.active->generation : 0) + 1};
          return next;
        }
      }
      if (selected.empty() || !allUnavailable(selected, model, now)) throw AuthorityError("routing_unavailable", 503);
      return std::nullopt;
    }

    const AccountBinding* findAccount(const AuthorityState& state, const std::optional<std::string>& accountId) {
      if (!accountId) return nullptr;
      for (const AccountBinding& account : state.accounts) {
        if (account.accountId == *accountId) return &account;
      }
      return nullptr;
    }

    std::optional<std::string> accountIdOf(const std::optional<AccountBinding>& account) {
      return account ? std::optional<std::string>(account->accountId) : std::nullopt;
    }

    void reserveSubscription(AuthorityState& state, AdmissionRecord& request, const AttemptInput& input, double nowMs) {
      if (!input.account || !sameEvidence(toJson(*input.account), toJson(request.account)) || input.maximumExposureMicrocredits != 0)
        throw AuthorityError("account_fence_mismatch");
      const AccountBinding* current = findAccount(state, accountIdOf(input.account));
      if (current == nullptr || !state.active || !sameEvidence(toJson(*current), toJson(*input.account)) || !request.activeGeneration ||
          state.active->generation != *request.activeGeneration || state.active->accountId != current->accountId)
        throw AuthorityError("account_fence_mismatch");
      if (applicableProof(*current, request.model, nowMs)) throw AuthorityError("routing_unavailable", 503);
    }

    void reservePaid(AuthorityState& state, AdmissionRecord& request, const AttemptInput& input, const PrincipalPolicy& policy, double nowMs) {
      if (input.account) throw AuthorityError("paid_account_binding_forbidden");
      const std::vector<AccountBinding> selected = cohort(state, request.model);
      if (selected.empty() || !allUnavailable(selected, request.model, nowMs)) throw AuthorityError("paid_pool_proof_required", 503);
      std::vector<const AttemptRecord*> paid;
      for (const AttemptRecord* attempt : std::as_const(request.attempts).values()) {
        if (attempt->input.provider != "codex") paid.push_back(attempt);
      }
      if (input.provider == "surplus" && !paid.empty()) throw AuthorityError("provider_order_conflict");
      if (input.provider == "openlux" &&
          (paid.size() != 1 || paid[0]->input.provider != "surplus" || !(paid[0]->terminal && paid[0]->terminal->fallback && !paid[0]->terminal->fallback->empty())))
        throw AuthorityError("paid_capacity_proof_required", 503);
      PrincipalWindow* window = state.windows.get(request.windowKey);
      if (existingWindow(window, "window.paidReservedMicrocredits").paidReservedMicrocredits + window->paidSettledMicrocredits +
            input.maximumExposureMicrocredits >
          policy.paidLimitMicrocredits)
        throw AuthorityError("paid_limit_exceeded", 429);
      window->paidReservedMicrocredits += input.maximumExposureMicrocredits;
    }

    bool truthy(const std::optional<std::string>& value) { return value && !value->empty(); }
  }

  AuthorityState createAuthorityState(const AuthorityScope& scope, const std::vector<AccountBinding>& accounts, const std::vector<PrincipalPolicy>& policies) {
    AuthorityState state{
      .scope = scope,
      .sequence = 0,
      .accounts = accounts,
      .active = std::nullopt,
      .policies = {},
      .windows = {},
      .admissions = {},
      .publications = {},
    };
    for (const PrincipalPolicy& policy : policies) state.policies.define(policy.principalId, policy);
    return state;
  }

  PortableAuthority::PortableAuthority(std::shared_ptr<AuthorityStore> store_, AuthorityScope scope_) : _store(std::move(store_)), _scope(std::move(scope_)) {}

  template <class T, class Action> rt::Task<T> PortableAuthority::_transact(AuthorityScope scope, Action action) {
    if (!sameScope(scope, _scope)) throw AuthorityError("namespace_or_epoch_mismatch", 403);
    for (int attempt = 0; attempt < 8; attempt += 1) {
      rt::Promise<AuthoritySnapshot> reading = _store->read();
      const AuthoritySnapshot snapshot = co_await reading.wait();
      if (!sameScope(snapshot.state.scope, scope)) throw AuthorityError("namespace_or_epoch_mismatch", 403);
      AuthorityState state = snapshot.state;
      if constexpr (std::is_void_v<T>) {
        action(state);
        // Even unchanged admissions linearize against the entire strong snapshot.
        rt::Promise<bool> swapping = _store->compareAndSwap(snapshot.revision, state);
        const bool swapped = co_await swapping.wait();
        if (swapped) co_return;
      } else {
        T result = action(state);
        // Even unchanged admissions linearize against the entire strong snapshot.
        rt::Promise<bool> swapping = _store->compareAndSwap(snapshot.revision, state);
        const bool swapped = co_await swapping.wait();
        if (swapped) co_return result;
      }
    }
    throw AuthorityError("concurrent_update", 503);
  }

  AdmissionRecord& PortableAuthority::_request(AuthorityState& state, const AdmissionHandle& handle) {
    AdmissionRecord* request = lookup(state.admissions, handle.requestId);
    if (request == nullptr || request->handle.fence != handle.fence) throw AuthorityError("admission_fence_mismatch");
    return *request;
  }

  rt::Task<AdmissionHandle> PortableAuthority::admit(AdmitInput input) {
    auto action = [&input](AuthorityState& state) -> AdmissionHandle {
      if (const AdmissionRecord* previous = lookup(state.admissions, input.requestId)) {
        if (previous->digest != input.digest || previous->principalId != input.principalId || previous->model != input.model || previous->route != input.route ||
            previous->policyVersion != input.policyVersion)
          throw AuthorityError("request_identity_conflict");
        return previous->handle;
      }
      const PrincipalPolicy* policy = lookup(state.policies, input.principalId);
      if (policy == nullptr || policy->deleted || policy->version != input.policyVersion) throw AuthorityError("principal_policy_changed", 403);
      if (policy->windowResetAtMs <= input.nowMs) throw AuthorityError("quota_window_requires_refresh", 503);
      for (const double amount : {policy->requestLimit, policy->kernelLimit, policy->concurrencyLimit, policy->paidLimitMicrocredits}) assertAmount(amount);
      const std::string windowKey = windowKeyFor(input.principalId, policy->windowResetAtMs);
      // `state.windows[windowKey] ??= emptyWindow()` evaluates to the window even
      // where the assignment stores nothing.
      PrincipalWindow assigned = emptyWindow();
      PrincipalWindow* window = state.windows.get(windowKey);
      if (window == nullptr) {
        state.windows.set(windowKey, emptyWindow());
        window = state.windows.get(windowKey);
        if (window == nullptr) window = &assigned;
      }
      double active = 0;
      for (const AdmissionRecord* request : std::as_const(state.admissions).values()) {
        if (request->principalId == input.principalId && !request->settled) active += 1;
      }
      if (window->reserved + window->dispatched >= policy->requestLimit || window->kernelReserved + window->kernelCompleted >= policy->kernelLimit ||
          active >= policy->concurrencyLimit)
        throw AuthorityError("principal_quota_exceeded", 429);
      const std::optional<AccountBinding> account = selectAccount(state, input.model, input.nowMs);
      const AdmissionHandle handle{.scope = input.scope, .requestId = input.requestId, .fence = ++state.sequence};
      state.admissions.set(input.requestId, AdmissionRecord{
                                              .handle = handle,
                                              .principalId = input.principalId,
                                              .policyVersion = input.policyVersion,
                                              .windowKey = windowKey,
                                              .digest = input.digest,
                                              .route = input.route,
                                              .model = input.model,
                                              .account = account,
                                              .activeGeneration = state.active ? std::optional<double>(state.active->generation) : std::nullopt,
                                              .attempts = {},
                                              .dispatched = false,
                                              .semantic = std::nullopt,
                                              .kernelSettled = false,
                                              .delivery = std::nullopt,
                                              .settled = false,
                                            });
      window->reserved += 1;
      window->kernelReserved += 1;
      window->active += 1;
      return handle;
    };
    rt::Task<AdmissionHandle> transaction = _transact<AdmissionHandle>(input.scope, action);
    AdmissionHandle handle = co_await std::move(transaction);
    co_return handle;
  }

  rt::Task<void> PortableAuthority::reserveAttempt(AdmissionHandle handle, AttemptInput input, double nowMs) {
    assertAmount(input.maximumExposureMicrocredits);
    auto action = [this, &handle, &input, nowMs](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      if (request.settled || request.semantic) throw AuthorityError("admission_closed");
      if (const AttemptRecord* existing = lookup(request.attempts, input.attemptId)) {
        if (!sameEvidence(toJson(existing->input), toJson(input))) throw AuthorityError("attempt_identity_conflict");
        return;
      }
      for (const AttemptRecord* attempt : std::as_const(request.attempts).values()) {
        if (attempt->state != "not_dispatched" && !attempt->terminal) throw AuthorityError("attempt_outcome_pending");
      }
      const PrincipalPolicy* policy = lookup(state.policies, request.principalId);
      if (policy == nullptr || policy->deleted || policy->version != request.policyVersion) throw AuthorityError("principal_policy_changed", 403);
      if (policy->windowResetAtMs <= nowMs || request.windowKey != windowKeyFor(request.principalId, policy->windowResetAtMs))
        throw AuthorityError("quota_window_changed", 409);
      if (input.provider == "codex") reserveSubscription(state, request, input, nowMs);
      else reservePaid(state, request, input, *policy, nowMs);
      request.attempts.set(input.attemptId, AttemptRecord{.input = input, .state = "reserved", .terminal = std::nullopt, .billingSettled = false, .settledSpendMicrocredits = std::nullopt});
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<bool> PortableAuthority::beginDispatch(AdmissionHandle handle, std::string attemptId, double nowMs) {
    auto action = [this, &handle, &attemptId, nowMs](AuthorityState& state) -> bool {
      AdmissionRecord& request = _request(state, handle);
      AttemptRecord* attempt = lookup(request.attempts, attemptId);
      if (attempt == nullptr) throw AuthorityError("attempt_missing");
      if (attempt->state != "reserved") return false;
      const PrincipalPolicy* policy = lookup(state.policies, request.principalId);
      if (policy == nullptr || policy->deleted || policy->version != request.policyVersion || request.settled) throw AuthorityError("principal_policy_changed", 403);
      if (policy->windowResetAtMs <= nowMs || request.windowKey != windowKeyFor(request.principalId, policy->windowResetAtMs))
        throw AuthorityError("quota_window_changed", 409);
      if (attempt->input.account) {
        const AccountBinding* current = findAccount(state, accountIdOf(attempt->input.account));
        const json::value currentJson = current != nullptr ? toJson(*current) : json::value();
        const json::value boundJson = toJson(*attempt->input.account);
        const std::optional<double> activeGeneration = state.active ? std::optional<double>(state.active->generation) : std::nullopt;
        if (!sameEvidence(current != nullptr ? &currentJson : nullptr, &boundJson) || activeGeneration != request.activeGeneration)
          throw AuthorityError("account_fence_mismatch");
      } else {
        const std::vector<AccountBinding> selected = cohort(state, request.model);
        if (selected.empty() || !allUnavailable(selected, request.model, nowMs)) throw AuthorityError("paid_pool_proof_required", 503);
      }
      // Persist intent first. A crash after this point remains indeterminate, never replayable/refundable.
      attempt->state = "dispatch_intent";
      return true;
    };
    rt::Task<bool> transaction = _transact<bool>(handle.scope, action);
    const bool begun = co_await std::move(transaction);
    co_return begun;
  }

  rt::Task<Selection> PortableAuthority::selection(AdmissionHandle handle) {
    auto action = [this, &handle](AuthorityState& state) -> Selection {
      const AdmissionRecord& request = _request(state, handle);
      return Selection{.account = request.account, .model = request.model, .route = request.route};
    };
    rt::Task<Selection> transaction = _transact<Selection>(handle.scope, action);
    Selection selected = co_await std::move(transaction);
    co_return selected;
  }

  rt::Task<std::optional<AccountBinding>> PortableAuthority::reselectSubscription(AdmissionHandle handle, double nowMs) {
    auto action = [this, &handle, nowMs](AuthorityState& state) -> std::optional<AccountBinding> {
      AdmissionRecord& request = _request(state, handle);
      if (request.settled || request.semantic) throw AuthorityError("admission_closed");
      const std::vector<const AttemptRecord*> attempts = std::as_const(request.attempts).values();
      if (std::any_of(attempts.begin(), attempts.end(), [](const AttemptRecord* attempt) { return attempt->input.provider != "codex"; }))
        throw AuthorityError("provider_order_conflict");
      const bool dispatchedOnBinding = std::any_of(attempts.begin(), attempts.end(), [&](const AttemptRecord* attempt) {
        return attempt->state == "dispatched" && sameEvidence(toJson(attempt->input.account), toJson(request.account));
      });
      const bool outcomePending = std::any_of(attempts.begin(), attempts.end(), [](const AttemptRecord* attempt) {
        return attempt->state != "not_dispatched" && (attempt->state != "dispatched" || !attempt->terminal);
      });
      if (!dispatchedOnBinding || outcomePending) throw AuthorityError("attempt_outcome_pending");
      const PrincipalPolicy* policy = lookup(state.policies, request.principalId);
      if (policy == nullptr || policy->deleted || policy->version != request.policyVersion) throw AuthorityError("principal_policy_changed", 403);
      if (policy->windowResetAtMs <= nowMs || request.windowKey != windowKeyFor(request.principalId, policy->windowResetAtMs))
        throw AuthorityError("quota_window_changed");
      const std::optional<AccountBinding> binding = request.account;
      const AccountBinding* current = findAccount(state, accountIdOf(binding));
      // Proof is the only captured field that an observation may change before reselection.
      if (!binding || current == nullptr) throw AuthorityError("account_fence_mismatch");
      AccountBinding withBindingProof = *current;
      withBindingProof.proof = binding->proof;
      if (!sameEvidence(toJson(withBindingProof), toJson(*binding))) throw AuthorityError("account_fence_mismatch");
      if (!unavailable(*current, request.model, nowMs)) throw AuthorityError("subscription_proof_required", 503);
      request.account = selectAccount(state, request.model, nowMs);
      request.activeGeneration = state.active ? std::optional<double>(state.active->generation) : std::nullopt;
      return request.account;
    };
    rt::Task<std::optional<AccountBinding>> transaction = _transact<std::optional<AccountBinding>>(handle.scope, action);
    std::optional<AccountBinding> account = co_await std::move(transaction);
    co_return account;
  }

  rt::Task<void> PortableAuthority::confirmDispatched(AdmissionHandle handle, std::string attemptId) {
    auto action = [this, &handle, &attemptId](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      AttemptRecord* attempt = lookup(request.attempts, attemptId);
      if (attempt == nullptr || (attempt->state != "dispatch_intent" && attempt->state != "dispatched")) throw AuthorityError("dispatch_state_conflict");
      attempt->state = "dispatched";
      if (!request.dispatched) {
        request.dispatched = true;
        existingWindow(state.windows.get(request.windowKey), "state.windows[request.windowKey].reserved").reserved -= 1;
        existingWindow(state.windows.get(request.windowKey), "state.windows[request.windowKey].dispatched").dispatched += 1;
      }
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<void> PortableAuthority::releaseBeforeDispatch(AdmissionHandle handle, std::string attemptId) {
    auto action = [this, &handle, &attemptId](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      AttemptRecord* attempt = lookup(request.attempts, attemptId);
      if (attempt == nullptr || attempt->state == "not_dispatched") return;
      if (attempt->state != "reserved") throw AuthorityError("dispatch_indeterminate");
      attempt->state = "not_dispatched";
      existingWindow(state.windows.get(request.windowKey), "state.windows[request.windowKey].paidReservedMicrocredits").paidReservedMicrocredits -=
        attempt->input.maximumExposureMicrocredits;
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<void> PortableAuthority::observeProof(AdmissionHandle handle, std::string attemptId, AccountProof proof) {
    if (!rt::Number::isSafeInteger(proof.observedAtMs) || !rt::Number::isSafeInteger(proof.validUntilMs) || proof.validUntilMs <= proof.observedAtMs)
      throw AuthorityError("invalid_proof", 400);
    auto action = [this, &handle, &attemptId, &proof](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      const AttemptRecord* observed = lookup(request.attempts, attemptId);
      if (observed == nullptr || observed->state != "dispatched") throw AuthorityError("proof_without_dispatch");
      const std::optional<AccountBinding> binding = observed->input.account;
      std::ptrdiff_t index = -1;
      for (std::size_t position = 0; position < state.accounts.size(); ++position) {
        if (binding && state.accounts[position].accountId == binding->accountId) {
          index = static_cast<std::ptrdiff_t>(position);
          break;
        }
      }
      const AccountBinding* current = findAccount(state, accountIdOf(binding));
      if (!binding || current == nullptr || current->fingerprint != binding->fingerprint || current->credentialGeneration != binding->credentialGeneration ||
          current->routingGeneration != binding->routingGeneration || current->resetGeneration != binding->resetGeneration ||
          current->overageVersion != binding->overageVersion || proof.fingerprint != current->fingerprint || proof.model != request.model)
        throw AuthorityError("proof_fence_mismatch");
      if (current->proof && proof.observedAtMs < current->proof->observedAtMs) throw AuthorityError("stale_proof");
      AccountBinding updated = *current;
      updated.proof = proof;
      state.accounts[static_cast<std::size_t>(index)] = std::move(updated);
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<void> PortableAuthority::observeTerminal(AdmissionHandle handle, std::string attemptId, TerminalEvidence evidence, bool finalAttempt) {
    if (evidence.spendMicrocredits) assertAmount(*evidence.spendMicrocredits);
    auto action = [this, &handle, &attemptId, &evidence, finalAttempt](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      AttemptRecord* attempt = lookup(request.attempts, attemptId);
      if (attempt == nullptr || attempt->state != "dispatched") throw AuthorityError("terminal_without_dispatch");
      if (attempt->terminal && !sameEvidence(toJson(*attempt->terminal), toJson(evidence))) throw AuthorityError("terminal_evidence_conflict");
      if (finalAttempt && truthy(request.semantic) && *request.semantic != evidence.outcome) throw AuthorityError("terminal_evidence_conflict");
      attempt->terminal = evidence;
      if (finalAttempt) {
        request.semantic = evidence.outcome;
        if (!request.kernelSettled) {
          existingWindow(state.windows.get(request.windowKey), "state.windows[request.windowKey].kernelReserved").kernelReserved -= 1;
          if (evidence.outcome == "completed") existingWindow(state.windows.get(request.windowKey), "state.windows[request.windowKey].kernelCompleted").kernelCompleted += 1;
          request.kernelSettled = true;
        }
      }
      if (!attempt->billingSettled && evidence.spendMicrocredits) {
        PrincipalWindow* window = state.windows.get(request.windowKey);
        existingWindow(window, "window.paidReservedMicrocredits").paidReservedMicrocredits -= attempt->input.maximumExposureMicrocredits;
        // Preserve authoritative actual spend even when it exceeds admitted exposure.
        if (attempt->input.provider != "codex") window->paidSettledMicrocredits += *evidence.spendMicrocredits;
        attempt->billingSettled = true;
        attempt->settledSpendMicrocredits = evidence.spendMicrocredits;
      }
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<void> PortableAuthority::observeDelivery(AdmissionHandle handle, DeliveryOutcome outcome) {
    auto action = [this, &handle, &outcome](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      if (truthy(request.delivery) && *request.delivery != "unobserved" && *request.delivery != outcome) throw AuthorityError("delivery_evidence_conflict");
      if (outcome != "unobserved" || !request.delivery) request.delivery = outcome;
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<void> PortableAuthority::observeUsage(AdmissionHandle handle, std::string attemptId, Usage usage) {
    assertAmount(usage.spendMicrocredits);
    auto action = [this, &handle, &attemptId, &usage](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      AttemptRecord* attempt = lookup(request.attempts, attemptId);
      if (attempt == nullptr || attempt->state != "dispatched" || !attempt->terminal || attempt->input.provider == "codex" ||
          attempt->terminal->providerRequestId != usage.providerRequestId)
        throw AuthorityError("usage_correlation_mismatch");
      if (attempt->billingSettled) {
        if (attempt->settledSpendMicrocredits != usage.spendMicrocredits) throw AuthorityError("usage_evidence_conflict");
        return;
      }
      attempt->billingSettled = true;
      attempt->settledSpendMicrocredits = usage.spendMicrocredits;
      PrincipalWindow* window = state.windows.get(request.windowKey);
      existingWindow(window, "window.paidReservedMicrocredits").paidReservedMicrocredits -= attempt->input.maximumExposureMicrocredits;
      window->paidSettledMicrocredits += usage.spendMicrocredits;
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
  }

  rt::Task<bool> PortableAuthority::resetEligible(AdmissionHandle handle, double nowMs) {
    auto action = [this, &handle, nowMs](AuthorityState& state) -> bool {
      const AdmissionRecord& request = _request(state, handle);
      const std::vector<AccountBinding> selected = cohort(state, request.model);
      return !selected.empty() && std::all_of(selected.begin(), selected.end(), [&](const AccountBinding& account) {
               const std::optional<AccountProof> proof = applicableProof(account, request.model, nowMs);
               return proof && proof->kind == "quota_exhausted";
             });
    };
    rt::Task<bool> transaction = _transact<bool>(handle.scope, action);
    const bool eligible = co_await std::move(transaction);
    co_return eligible;
  }

  rt::Task<std::string> PortableAuthority::settle(AdmissionHandle handle) {
    auto action = [this, &handle](AuthorityState& state) -> std::string {
      AdmissionRecord& request = _request(state, handle);
      if (request.settled) return "settled";
      // Worker EOF records unobserved delivery. It never fabricates a native finish receipt.
      if (!request.semantic || !request.delivery) return "pending";
      const std::vector<const AttemptRecord*> attempts = std::as_const(request.attempts).values();
      if (std::any_of(attempts.begin(), attempts.end(), [](const AttemptRecord* attempt) {
            return attempt->state == "reserved" || attempt->state == "dispatch_intent" || (attempt->state == "dispatched" && !attempt->terminal);
          }))
        return "pending";
      PrincipalWindow* window = state.windows.get(request.windowKey);
      if (!request.dispatched) existingWindow(window, "window.reserved").reserved -= 1;
      if (!request.kernelSettled) {
        existingWindow(window, "window.kernelReserved").kernelReserved -= 1;
        if (*request.semantic == "completed") window->kernelCompleted += 1;
        request.kernelSettled = true;
      }
      existingWindow(window, "window.active").active -= 1;
      request.settled = true;
      return "settled";
    };
    rt::Task<std::string> transaction = _transact<std::string>(handle.scope, action);
    std::string outcome = co_await std::move(transaction);
    co_return outcome;
  }

  rt::Task<void> PortableAuthority::cancelUndispatched(AdmissionHandle handle) {
    auto action = [this, &handle](AuthorityState& state) {
      AdmissionRecord& request = _request(state, handle);
      const std::vector<AttemptRecord*> attempts = request.attempts.values();
      if (std::any_of(attempts.begin(), attempts.end(), [](const AttemptRecord* attempt) { return attempt->state == "dispatch_intent" || attempt->state == "dispatched"; }))
        throw AuthorityError("dispatch_indeterminate");
      for (AttemptRecord* attempt : attempts) {
        if (attempt->state == "reserved")
          existingWindow(state.windows.get(request.windowKey), "state.windows[request.windowKey].paidReservedMicrocredits").paidReservedMicrocredits -=
            attempt->input.maximumExposureMicrocredits;
        attempt->state = "not_dispatched";
      }
      if (!request.semantic) request.semantic = "cancelled";
      if (!request.delivery) request.delivery = "interrupted";
    };
    rt::Task<void> transaction = _transact<void>(handle.scope, action);
    co_await std::move(transaction);
    rt::Task<std::string> settling = settle(handle);
    co_await std::move(settling);
  }
}
