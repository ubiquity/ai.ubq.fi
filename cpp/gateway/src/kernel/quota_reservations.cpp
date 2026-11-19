#include "uos/kernel/quota_reservations.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace uos::kernel::quota_reservations {
  namespace {
    namespace contracts = authority::native_kernel_contracts;
    namespace native_kernel = authority::native_kernel;
    using contracts::KernelQuotaReservationRowV2;
    using contracts::KernelQuotaScope;
    using contracts::KernelQuotaWindowV2;
    using contracts::KernelReservationSettlement;
    using native_kernel::MAX_KV_RETRIES;
    using quota_v2::KERNEL_QUOTA_CONSTANTS;
    using quota_v2::kernelQuotaRead;
    using quota_v2::kernelQuotaValue;

    // `{ consistency: "strong" }`
    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    rt::KvListOptions strongList() {
      rt::KvListOptions options;
      options.consistency = rt::KvConsistencyLevel::strong;
      return options;
    }

    // The `cause` of `new Error(message, { cause: lastError })`: the Error the caught reason
    // holds (none for the initial `null`).
    std::shared_ptr<const rt::JsError> causeOf(const rt::Reason& reason) {
      if (!reason) return nullptr;
      try {
        std::rethrow_exception(reason);
      } catch (const rt::JsError& error) {
        return std::make_shared<const rt::JsError>(error);
      } catch (const std::exception& error) {
        return std::make_shared<const rt::JsError>(error.what());
      } catch (...) {
        return nullptr;
      }
    }

    // `Object.hasOwn(value, key)` on a stored value: only a plain object has named own members.
    bool hasOwnMember(const rt::KvValue& value, std::string_view key) {
      const rt::KvObject* object = std::get_if<rt::KvObject>(&value.variant());
      return object != nullptr && object->contains(key);
    }

    // `{ scope, owner, repo, windowCreatedAtMs, requestId, route }`
    contracts::KernelQuotaReservationIdentity reservationIdentity(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo,
                                                                  double windowCreatedAtMs, const std::string& requestId, const std::string& route) {
      contracts::KernelQuotaReservationIdentity identity;
      identity.scope = scope;
      identity.owner = owner;
      identity.repo = repo;
      identity.requestId = requestId;
      identity.route = route;
      identity.windowCreatedAtMs = windowCreatedAtMs;
      return identity;
    }

    rt::Response kernelQuotaUnavailableResponse(std::string message = "Kernel quota is unavailable") {
      http::OpenaiErrorOptions options;
      options.type = "server_error";
      return http::openaiError(503, std::move(message), std::string("server_error"), std::move(options));
    }
  }

  quota_v2::KernelQuotaReservationDecision kernelQuotaUnavailable() { return kernelQuotaUnavailable("Kernel quota is unavailable"); }

  quota_v2::KernelQuotaReservationDecision kernelQuotaUnavailable(std::string message) {
    return quota_v2::KernelQuotaReservationDecisionNotOk{.response = kernelQuotaUnavailableResponse(std::move(message))};
  }

  rt::Task<KernelQuotaPolicyState> readKernelQuotaPolicyState(std::shared_ptr<rt::Kv> kv, std::string owner, std::string repo) {
    rt::Promise<rt::KvEntryMaybe<>> repoRead(kv->get(native_kernel::kernelRepoPolicyKey(owner, repo), STRONG));
    rt::Promise<rt::KvEntryMaybe<>> orgRead(kv->get(native_kernel::kernelOrgPolicyKey(owner), STRONG));
    auto reads = rt::all(repoRead, orgRead);
    auto readEntries = co_await reads.wait();
    auto& [repoEntry, orgEntry] = readEntries;
    const native_kernel::KernelQuotaScopeSnapshot snapshot{.repoPolicy = kernelQuotaRead(repoEntry), .orgPolicy = kernelQuotaRead(orgEntry)};
    const native_kernel::KernelQuotaScopeDecision state = native_kernel::decideKernelQuotaScope(snapshot, owner, repo, KERNEL_QUOTA_CONSTANTS);
    KernelQuotaPolicyState resolved{.limit_scope = state.limit_scope, .has_policy = state.has_policy, .repo_entry = std::move(repoEntry), .org_entry = std::move(orgEntry)};
    co_return resolved;
  }

  rt::Task<quota_v2::KernelQuotaPolicyStateDecision> resolveKernelQuotaPolicyState(std::string owner, std::string repo) {
    return resolveKernelQuotaPolicyState(std::move(owner), std::move(repo), KernelQuotaPolicyStateOptions{});
  }

  rt::Task<quota_v2::KernelQuotaPolicyStateDecision> resolveKernelQuotaPolicyState(std::string owner, std::string repo, KernelQuotaPolicyStateOptions options) {
    rt::Reason failure;
    try {
      std::shared_ptr<rt::Kv> kv;
      if (options.kv) {
        kv = *options.kv;
      } else {
        auto opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      }
      if (!kv) co_return quota_v2::KernelQuotaPolicyStateDecisionNotOk{.response = kernelQuotaUnavailableResponse()};
      rt::Task<KernelQuotaPolicyState> reading = readKernelQuotaPolicyState(kv, owner, repo);
      const KernelQuotaPolicyState state = co_await std::move(reading);
      co_return quota_v2::KernelQuotaPolicyStateDecisionOk{.limit_scope = state.limit_scope, .has_policy = state.has_policy};
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to resolve kernel quota policy scope:", failure);
    co_return quota_v2::KernelQuotaPolicyStateDecisionNotOk{.response = kernelQuotaUnavailableResponse()};
  }

  rt::Task<std::optional<KernelQuotaWindowV2>> reconcileKernelQuotaWindowReservations(std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> entry, KernelQuotaScope scope,
                                                                                     std::string owner, std::optional<std::string> repo) {
    const std::optional<KernelQuotaWindowV2> window = native_kernel::normalizeKernelQuotaWindowV2(kernelQuotaValue(entry.value), scope, owner, repo);
    if (!window || hasOwnMember(entry.value, "reserved_requests")) co_return window;
    double reservedRequests = 0;
    const rt::KvKey prefix = native_kernel::kernelReservationWindowPrefix(scope, owner, repo, window->created_at_ms);
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix}, strongList());
    while (std::optional<rt::KvEntry<>> reservationEntry = co_await iterator->next()) {
      const KernelQuotaReservationRowV2 reservation =
        native_kernel::requireKernelReservationRow(reservationEntry->key, kernelQuotaValue(reservationEntry->value), scope, owner, repo, window->created_at_ms);
      if (reservation.state == "reserved") reservedRequests += 1;
    }
    KernelQuotaWindowV2 reconciled = *window;
    reconciled.reserved_requests = reservedRequests;
    co_return reconciled;
  }

  namespace {
    // `const kernelQuotaLocks = new Map<string, Promise<void>>()` (its order is never observed).
    std::unordered_map<std::string, rt::Promise<void>>& kernelQuotaLocks() {
      static std::unordered_map<std::string, rt::Promise<void>> locks;
      return locks;
    }

    // `JSON.stringify(key)` of a scope window key (every part is a string).
    std::string stringifyKey(const rt::KvKey& key) {
      json::array parts;
      for (const rt::KvKeyPart& part : key) parts.push_back(json::value(json::string(part.string())));
      return rt::jsonStringify(json::value(std::move(parts)));
    }
  }

  namespace detail {
    KernelQuotaLockTurn enterKernelQuotaLock(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo) {
      std::string lockKey = stringifyKey(native_kernel::kernelScopedWindowKey(scope, owner, repo));
      std::unordered_map<std::string, rt::Promise<void>>& locks = kernelQuotaLocks();
      std::optional<rt::Promise<void>> previous;
      if (const auto found = locks.find(lockKey); found != locks.end()) previous = found->second;
      rt::Resolvers<void> resolvers = rt::withResolvers<void>();
      locks.insert_or_assign(lockKey, resolvers.promise);
      return KernelQuotaLockTurn{.lockKey = std::move(lockKey), .previous = std::move(previous), .current = resolvers.promise, .release = resolvers.resolve};
    }

    void leaveKernelQuotaLock(const KernelQuotaLockTurn& turn) {
      turn.release();
      std::unordered_map<std::string, rt::Promise<void>>& locks = kernelQuotaLocks();
      if (const auto found = locks.find(turn.lockKey); found != locks.end() && found->second == turn.current) locks.erase(found);
    }
  }

  namespace {
    rt::Task<std::variant<KernelReservationSettlement, double>> recordKernelReservationTerminalIntentUnlocked(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope,
                                                                                                             std::string owner, std::optional<std::string> repo,
                                                                                                             double windowCreatedAtMs, std::string requestId,
                                                                                                             std::string route, std::string terminalState, double nowMs) {
      const rt::KvKey reservationKey = native_kernel::kernelReservationKey(scope, owner, repo, windowCreatedAtMs, requestId);
      const rt::KvEntryMaybe<> reservationEntry = co_await kv->get(reservationKey, STRONG);
      const contracts::KernelQuotaTerminalPlan plan = native_kernel::decideKernelQuotaTerminalIntent(
        reservationIdentity(scope, owner, repo, windowCreatedAtMs, requestId, route), kernelQuotaRead(reservationEntry), terminalState, nowMs);
      if (const auto* unchanged = std::get_if<contracts::KernelQuotaTerminalPlanUnchanged>(&plan)) co_return unchanged->outcome;
      const contracts::KernelQuotaTerminalPlanIntent& intent = std::get<contracts::KernelQuotaTerminalPlanIntent>(plan);
      rt::KvSetOptions retention;
      retention.expireIn = intent.expireIn;
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(reservationEntry).set(reservationKey, rt::KvValue(json::value_from(intent.reservation)), retention);
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) co_return intent.leaseExpiresAtMs;
      co_return KernelReservationSettlement("conflict");
    }

    // `number | null | "retry"`: `retry` is "retry"; otherwise the lease expiry, std::nullopt for null.
    struct TerminalIntentAttempt {
      bool retry;
      std::optional<double> leaseExpiresAtMs;
    };

    /**
     * One attempt at recording the terminal intent: the lease expiry when the intent
     * was written, null when it was already settled, "retry" on a lost compare-and-set.
     */
    rt::Task<TerminalIntentAttempt> attemptRecordKernelReservationTerminalIntent(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                                std::optional<std::string> repo, double windowCreatedAtMs, std::string requestId,
                                                                                std::string route, std::string terminalState, double nowMs) {
      rt::Task<std::variant<KernelReservationSettlement, double>> recording =
        recordKernelReservationTerminalIntentUnlocked(kv, scope, owner, repo, windowCreatedAtMs, requestId, route, terminalState, nowMs);
      const std::variant<KernelReservationSettlement, double> outcome = co_await std::move(recording);
      if (const double* leaseExpiresAtMs = std::get_if<double>(&outcome)) co_return TerminalIntentAttempt{.retry = false, .leaseExpiresAtMs = *leaseExpiresAtMs};
      const KernelReservationSettlement& settlement = std::get<KernelReservationSettlement>(outcome);
      if (settlement == "settled") co_return TerminalIntentAttempt{.retry = false, .leaseExpiresAtMs = std::nullopt};
      if (settlement == "conflict") co_return TerminalIntentAttempt{.retry = true, .leaseExpiresAtMs = std::nullopt};
      if (settlement == "terminal_mismatch") {
        throw rt::JsError(std::string("Kernel quota reservation was already ") + (terminalState == "committed" ? "released" : "committed"));
      }
      throw rt::JsError(settlement == "missing" ? "Kernel quota reservation is missing" : "Kernel quota reservation is malformed");
    }

    // std::nullopt is null.
    rt::Task<std::optional<double>> recordKernelReservationTerminalIntent(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                          std::optional<std::string> repo, double windowCreatedAtMs, std::string requestId,
                                                                          std::string route, std::string terminalState) {
      std::function<rt::Task<std::optional<double>>()> operation = [kv, scope, owner, repo, windowCreatedAtMs, requestId, route,
                                                                    terminalState]() -> rt::Task<std::optional<double>> {
        rt::Reason lastError;
        for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
          try {
            rt::Task<TerminalIntentAttempt> attempting =
              attemptRecordKernelReservationTerminalIntent(kv, scope, owner, repo, windowCreatedAtMs, requestId, route, terminalState, rt::dateNow());
            const TerminalIntentAttempt outcome = co_await std::move(attempting);
            if (!outcome.retry) co_return outcome.leaseExpiresAtMs;
          } catch (...) {
            lastError = std::current_exception();
          }
        }
        throw rt::JsError("Kernel quota reservation terminal intent could not be recorded", "Error", causeOf(lastError));
      };
      rt::Task<std::optional<double>> locked = withKernelQuotaLock<std::optional<double>>(scope, owner, repo, std::move(operation));
      std::optional<double> leaseExpiresAtMs = co_await std::move(locked);
      co_return leaseExpiresAtMs;
    }

    rt::Task<KernelReservationSettlement> settleKernelReservationUnlocked(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                          std::optional<std::string> repo, double windowCreatedAtMs, std::string requestId,
                                                                          std::string route, std::string terminalState, std::string reason, double nowMs) {
      const rt::KvKey windowKey = native_kernel::kernelScopedWindowKey(scope, owner, repo);
      const rt::KvKey reservationKey = native_kernel::kernelReservationKey(scope, owner, repo, windowCreatedAtMs, requestId);
      rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(windowKey, STRONG));
      rt::Promise<rt::KvEntryMaybe<>> reservationRead(kv->get(reservationKey, STRONG));
      auto reads = rt::all(windowRead, reservationRead);
      const auto readEntries = co_await reads.wait();
      const auto& [windowEntry, reservationEntry] = readEntries;
      const std::optional<KernelQuotaReservationRowV2> reservation =
        native_kernel::normalizeKernelQuotaReservationRowV2(kernelQuotaValue(reservationEntry.value), scope, owner, repo);
      if (!reservation) co_return KernelReservationSettlement(reservationEntry.value.isNull() ? "missing" : "invalid");
      const std::optional<KernelReservationSettlement> precondition =
        native_kernel::kernelReservationPrecondition(*reservation, requestId, route, windowCreatedAtMs, terminalState);
      if (precondition) co_return *precondition;

      rt::Task<std::optional<KernelQuotaWindowV2>> reconciling = reconcileKernelQuotaWindowReservations(kv, windowEntry, scope, owner, repo);
      const std::optional<KernelQuotaWindowV2> normalizedWindow = co_await std::move(reconciling);
      const native_kernel::KernelQuotaSettlementSnapshot snapshot{.window = normalizedWindow, .windowPresent = !windowEntry.value.isNull()};
      const contracts::KernelQuotaSettlePlan plan = native_kernel::decideKernelQuotaSettlement(*reservation, snapshot, terminalState, reason, nowMs);
      if (const auto* unchanged = std::get_if<contracts::KernelQuotaSettlePlanUnchanged>(&plan)) co_return unchanged->outcome;
      const contracts::KernelQuotaSettlePlanSettle& settle = std::get<contracts::KernelQuotaSettlePlanSettle>(plan);
      rt::KvSetOptions retention;
      retention.expireIn = settle.expireIn;
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(windowEntry).check(reservationEntry);
      if (settle.window) operation->set(windowKey, rt::KvValue(json::value_from(*settle.window)), retention);
      operation->set(reservationKey, rt::KvValue(json::value_from(settle.reservation)), retention);
      const rt::KvCommitResult committed = co_await operation->commit();
      co_return KernelReservationSettlement(committed.ok ? "settled" : "conflict");
    }

    rt::Task<void> settleKernelReservation(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                           double windowCreatedAtMs, std::string requestId, std::string route, std::string terminalState, std::string reason) {
      std::function<rt::Task<void>()> operation = [kv, scope, owner, repo, windowCreatedAtMs, requestId, route, terminalState, reason]() -> rt::Task<void> {
        rt::Reason lastError;
        for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
          try {
            rt::Task<KernelReservationSettlement> settling =
              settleKernelReservationUnlocked(kv, scope, owner, repo, windowCreatedAtMs, requestId, route, terminalState, reason, rt::dateNow());
            const KernelReservationSettlement outcome = co_await std::move(settling);
            if (outcome == "settled") co_return;
            if (outcome == "conflict") continue;
            if (outcome == "terminal_mismatch") {
              throw rt::JsError(std::string("Kernel quota reservation was already ") + (terminalState == "committed" ? "released" : "committed"));
            }
            throw rt::JsError(outcome == "missing" ? "Kernel quota reservation is missing" : "Kernel quota reservation is malformed");
          } catch (...) {
            lastError = std::current_exception();
          }
        }
        throw rt::JsError("Kernel quota reservation could not be settled", "Error", causeOf(lastError));
      };
      rt::Task<void> locked = withKernelQuotaLock<void>(scope, owner, repo, std::move(operation));
      co_await std::move(locked);
    }

    rt::Task<double> renewKernelReservation(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                            double windowCreatedAtMs, std::string requestId, std::string route) {
      rt::Reason lastError;
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        try {
          const rt::KvKey reservationKey = native_kernel::kernelReservationKey(scope, owner, repo, windowCreatedAtMs, requestId);
          const rt::KvEntryMaybe<> entry = co_await kv->get(reservationKey, STRONG);
          const native_kernel::KernelQuotaRenewal plan = native_kernel::decideKernelQuotaRenewal(
            reservationIdentity(scope, owner, repo, windowCreatedAtMs, requestId, route), kernelQuotaRead(entry), rt::dateNow());
          rt::KvSetOptions retention;
          retention.expireIn = plan.expireIn;
          const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          operation->check(entry).set(reservationKey, rt::KvValue(json::value_from(plan.reservation)), retention);
          const rt::KvCommitResult committed = co_await operation->commit();
          if (committed.ok) co_return plan.leaseExpiresAtMs;
        } catch (...) {
          lastError = std::current_exception();
        }
      }
      throw rt::JsError("Kernel quota reservation lease could not be renewed", "Error", causeOf(lastError));
    }

    /**
     * Timer handles of the reservation lease. Each slot is optional because the
     * matching timer is absent before it is first scheduled and after it is cleared.
     */
    struct KernelQuotaTimerSlots {
      std::optional<rt::Timeout> renewal{};
      std::optional<rt::Timeout> leaseExpiry{};
      std::optional<rt::Timeout> settlementRetry{};
    };

    // The closure state of one kernelReservationContext: its arguments and `let` bindings. The
    // timers it arms capture it; clearing (or firing) a timer releases that reference.
    struct ReservationContextState {
      std::shared_ptr<rt::Kv> kv;
      KernelQuotaScope scope;
      std::string owner;
      std::optional<std::string> repo;
      double windowCreatedAtMs = 0;
      std::string requestId;
      std::string route;
      rt::AbortController leaseAbort;
      double leaseExpiresAtMs = 0;
      std::optional<rt::Promise<void>> renewalInFlight{};
      std::optional<std::string> terminalIntent{};  // "committed" | "released" | null
      std::string terminalReason = "request_incomplete";
      std::optional<std::string> terminalSettled{};  // "committed" | "released" | null
      std::optional<rt::Promise<void>> terminalInFlight{};
      KernelQuotaTimerSlots timers{};
      double settlementRetryDelayMs = native_kernel::KERNEL_QUOTA_SETTLEMENT_RETRY_MS;
    };
    using ReservationContext = std::shared_ptr<ReservationContextState>;

    void stopRenewal(const ReservationContext& context) {
      if (!context->timers.renewal) return;
      rt::clearInterval(*context->timers.renewal);
      context->timers.renewal.reset();
    }

    struct ReservationClient {
      explicit ReservationClient(ReservationContext live) : context(std::move(live)) {}
      ~ReservationClient() {
        if (!context->terminalIntent) stopRenewal(context);
      }
      ReservationContext context;
    };

    void clearLeaseExpiry(const ReservationContext& context) {
      if (!context->timers.leaseExpiry) return;
      rt::clearTimeout(*context->timers.leaseExpiry);
      context->timers.leaseExpiry.reset();
    }

    void stopAfterSettlement(const ReservationContext& context) {
      stopRenewal(context);
      clearLeaseExpiry(context);
      if (context->timers.settlementRetry) {
        rt::clearTimeout(*context->timers.settlementRetry);
        context->timers.settlementRetry.reset();
      }
    }

    void scheduleLeaseExpiry(const ReservationContext& context) {
      clearLeaseExpiry(context);
      if (context->terminalSettled) return;
      context->timers.leaseExpiry = rt::setTimeout(
        [context] {
          context->timers.leaseExpiry.reset();
          if (context->terminalSettled) return;
          if (rt::dateNow() < context->leaseExpiresAtMs) {
            scheduleLeaseExpiry(context);
            return;
          }
          if (!context->leaseAbort.signal().aborted()) {
            context->leaseAbort.abort(rt::makeReason(rt::DOMException("Kernel quota reservation lease expired", "TimeoutError")));
          }
          // A terminal intent still needs lease renewal and durable settlement.
          // For in-flight inference, abort is the fail-closed terminal condition.
          if (!context->terminalIntent) stopRenewal(context);
        },
        rt::Math::max(1, context->leaseExpiresAtMs - rt::dateNow()));
    }

    void updateLease(const ReservationContext& context, double nextLeaseExpiresAtMs) {
      context->leaseExpiresAtMs = nextLeaseExpiresAtMs;
      scheduleLeaseExpiry(context);
    }

    void renew(const ReservationContext& context) {
      if (context->terminalSettled || context->renewalInFlight) return;
      rt::Promise<double> renewing(
        renewKernelReservation(context->kv, context->scope, context->owner, context->repo, context->windowCreatedAtMs, context->requestId, context->route));
      // `renewalInFlight === current` compares with the promise `.finally` itself returns.
      auto self = std::make_shared<std::optional<rt::Promise<void>>>();
      rt::Promise<void> current =
        renewing.then([context](double nextLeaseExpiresAtMs) { updateLease(context, nextLeaseExpiresAtMs); })
          .catch_([](const rt::Reason& error) { rt::console::warn("[ai.ubq.fi] Failed to renew kernel quota reservation lease:", error); })
          .finally_([context, self] {
            if (context->renewalInFlight && *self && *context->renewalInFlight == **self) context->renewalInFlight.reset();
          });
      *self = current;
      context->renewalInFlight = current;
    }

    rt::Promise<void> launchSettlement(const ReservationContext& context);

    void scheduleSettlementRetry(const ReservationContext& context) {
      if (context->terminalSettled || context->timers.settlementRetry) return;
      const double delayMs = context->settlementRetryDelayMs;
      context->settlementRetryDelayMs = rt::Math::min(context->settlementRetryDelayMs * 2, 30'000);
      context->timers.settlementRetry = rt::setTimeout(
        [context] {
          context->timers.settlementRetry.reset();
          (void)launchSettlement(context).catch_(
            [](const rt::Reason& error) { rt::console::warn("[ai.ubq.fi] Failed to settle kernel quota reservation; retrying:", error); });
        },
        delayMs);
    }

    // The `(async () => { ... })()` of launchSettlement.
    rt::Task<void> settleTerminalIntent(ReservationContext context, std::string state) {
      rt::Task<std::optional<double>> recording = recordKernelReservationTerminalIntent(context->kv, context->scope, context->owner, context->repo,
                                                                                       context->windowCreatedAtMs, context->requestId, context->route, state);
      const std::optional<double> nextLeaseExpiresAtMs = co_await std::move(recording);
      if (nextLeaseExpiresAtMs) updateLease(context, *nextLeaseExpiresAtMs);
      rt::Task<void> settling = settleKernelReservation(context->kv, context->scope, context->owner, context->repo, context->windowCreatedAtMs, context->requestId,
                                                        context->route, state, context->terminalReason);
      co_await std::move(settling);
    }

    rt::Promise<void> launchSettlement(const ReservationContext& context) {
      if (!context->terminalIntent) return rt::Promise<void>::reject(rt::makeReason(rt::JsError("Kernel quota reservation has no terminal state")));
      if (context->terminalSettled == context->terminalIntent) return rt::Promise<void>::resolve();
      if (context->terminalInFlight) return *context->terminalInFlight;
      const std::string state = *context->terminalIntent;
      // `terminalInFlight === current` compares with the promise `.finally` itself returns.
      auto self = std::make_shared<std::optional<rt::Promise<void>>>();
      rt::Promise<void> running(settleTerminalIntent(context, state));
      rt::Promise<void> current = running
                                    .then([context, state] {
                                      context->terminalSettled = state;
                                      stopAfterSettlement(context);
                                    })
                                    .catch_([context](const rt::Reason& error) {
                                      scheduleSettlementRetry(context);
                                      rt::rethrow(error);
                                    })
                                    .finally_([context, self] {
                                      if (context->terminalInFlight && *self && *context->terminalInFlight == **self) context->terminalInFlight.reset();
                                    });
      *self = current;
      context->terminalInFlight = current;
      return current;
    }

    // `settle(state, reason)`
    rt::Promise<void> settle(const ReservationContext& context, const std::string& state, const std::string& reason) {
      if (!context->terminalIntent) {
        context->terminalIntent = state;
        context->terminalReason = state == "committed" ? std::string("completed") : reason;
      }
      if (context->terminalIntent != state) {
        return rt::Promise<void>::reject(rt::makeReason(rt::JsError("Kernel quota reservation terminal state is already " + *context->terminalIntent)));
      }
      if (context->terminalSettled == state) return rt::Promise<void>::resolve();
      return launchSettlement(context);
    }
  }

  quota_v2::KernelQuotaReservation kernelReservationContext(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                                            double windowCreatedAtMs, std::string requestId, std::string route, double initialLeaseExpiresAtMs,
                                                            std::optional<double> renewalIntervalMs) {
    const ReservationContext context = std::make_shared<ReservationContextState>();
    context->kv = std::move(kv);
    context->scope = std::move(scope);
    context->owner = std::move(owner);
    context->repo = std::move(repo);
    context->windowCreatedAtMs = windowCreatedAtMs;
    context->requestId = std::move(requestId);
    context->route = std::move(route);
    context->leaseExpiresAtMs = initialLeaseExpiresAtMs;
    const auto client = std::make_shared<ReservationClient>(context);
    context->timers.renewal = rt::setInterval([context] { renew(context); }, renewalIntervalMs.value_or(native_kernel::KERNEL_QUOTA_RESERVATION_RENEWAL_MS));
    scheduleLeaseExpiry(context);
    return quota_v2::KernelQuotaReservation{
      .signal = context->leaseAbort.signal(),
      .commit = [client] { return settle(client->context, "committed", "completed"); },
      .release = [client](std::optional<std::string> reason) { return settle(client->context, "released", reason.value_or("request_incomplete")); },
    };
  }

  namespace {
    /**
     * Settles one expired reservation. Returns true once the row is terminal; when
     * every attempt loses its compare-and-set the caller moves on to the next row.
     */
    rt::Task<bool> settleExpiredKernelReservation(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                                  KernelQuotaWindowV2 window, KernelQuotaReservationRowV2 reservation, std::string terminalState, double nowMs) {
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        rt::Task<KernelReservationSettlement> settling =
          settleKernelReservationUnlocked(kv, scope, owner, repo, window.created_at_ms, reservation.request_id, reservation.route, terminalState,
                                          terminalState == "committed" ? "completed" : "lease_expired", nowMs);
        const KernelReservationSettlement outcome = co_await std::move(settling);
        if (outcome == "conflict") continue;
        if (outcome == "invalid") throw rt::JsError("Kernel quota reservation is malformed");
        co_return true;
      }
      co_return false;
    }
  }

  rt::Task<bool> reclaimExpiredKernelReservationUnlocked(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                                         KernelQuotaWindowV2 window, double nowMs) {
    bool sawExpired = false;
    const rt::KvKey prefix = native_kernel::kernelReservationWindowPrefix(scope, owner, repo, window.created_at_ms);
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix}, strongList());
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const KernelQuotaReservationRowV2 reservation =
        native_kernel::requireKernelReservationRow(entry->key, kernelQuotaValue(entry->value), scope, owner, repo, window.created_at_ms);
      const std::optional<std::string> terminalState = native_kernel::decideKernelQuotaReclaim(reservation, nowMs);
      if (!terminalState) continue;
      sawExpired = true;
      rt::Task<bool> settling = settleExpiredKernelReservation(kv, scope, owner, repo, window, reservation, *terminalState, nowMs);
      const bool settled = co_await std::move(settling);
      if (settled) {
        co_await iterator->return_();
        co_return true;
      }
    }
    if (sawExpired) throw rt::JsError("Kernel quota reservation changed concurrently");
    co_return false;
  }

  namespace {
    /** True when the scope has an explicit policy, which supersedes the defaults. */
    rt::Task<bool> kernelScopeHasPolicy(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo) {
      auto key = native_kernel::kernelScopedPolicyKey(scope, owner, repo);
      auto reading = kv->get(std::move(key), STRONG);
      const rt::KvEntryMaybe<> policyEntry = co_await std::move(reading);
      if (policyEntry.value.isNull()) co_return false;
      if (!quota_v2::normalizeKernelQuotaPolicyV2(kernelQuotaValue(policyEntry.value), scope, owner, repo)) {
        throw rt::JsError("Kernel quota policy is malformed");
      }
      co_return true;
    }

    /** True when the scope window holds reservations that have not yet expired. */
    rt::Task<bool> kernelWindowHasLiveReservations(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo) {
      const rt::KvKey windowKey = native_kernel::kernelScopedWindowKey(scope, owner, repo);
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const double nowMs = rt::dateNow();
        const rt::KvEntryMaybe<> windowEntry = co_await kv->get(windowKey, STRONG);
        rt::Task<std::optional<KernelQuotaWindowV2>> reconciling = reconcileKernelQuotaWindowReservations(kv, windowEntry, scope, owner, repo);
        const std::optional<KernelQuotaWindowV2> window = co_await std::move(reconciling);
        if (!windowEntry.value.isNull() && !window) throw rt::JsError("Kernel quota window is malformed");
        if (!native_kernel::hasLiveKernelReservations(window, nowMs)) co_return false;
        rt::Task<bool> reclaiming = reclaimExpiredKernelReservationUnlocked(kv, scope, owner, repo, *window, nowMs);
        const bool reclaimed = co_await std::move(reclaiming);
        if (!reclaimed) co_return true;
      }
      throw rt::JsError("Kernel quota reservations changed concurrently");
    }

    rt::Task<bool> hasLiveDefaultBackedKernelReservations(std::shared_ptr<rt::Kv> kv) {
      static const std::array<std::string_view, 2> SCOPES{"repo", "org"};
      for (std::size_t index = 0; index < SCOPES.size(); ++index) {
        const KernelQuotaScope scope(SCOPES[index]);
        const rt::KvKey prefix = scope == "repo" ? native_kernel::KERNEL_REPO_WINDOW_V2_PREFIX : native_kernel::KERNEL_ORG_WINDOW_V2_PREFIX;
        const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix}, strongList());
        while (std::optional<rt::KvEntry<>> listedEntry = co_await iterator->next()) {
          const native_kernel::KernelWindowScopeTarget target = native_kernel::kernelWindowScopeTarget(scope, listedEntry->key, prefix);
          std::function<rt::Task<bool>()> operation = [kv, scope, owner = target.owner, repo = target.repo]() -> rt::Task<bool> {
            rt::Task<bool> checking = kernelScopeHasPolicy(kv, scope, owner, repo);
            const bool hasPolicy = co_await std::move(checking);
            if (hasPolicy) co_return false;
            rt::Task<bool> inspecting = kernelWindowHasLiveReservations(kv, scope, owner, repo);
            const bool live = co_await std::move(inspecting);
            co_return live;
          };
          rt::Task<bool> locked = withKernelQuotaLock<bool>(scope, target.owner, target.repo, std::move(operation));
          const bool hasLive = co_await std::move(locked);
          if (hasLive) {
            co_await iterator->return_();
            co_return true;
          }
        }
      }
      co_return false;
    }
  }

  rt::Task<void> releaseKernelDefaultWindowCutover(std::shared_ptr<rt::Kv> kv, quota_v2::KernelDefaultWindowCutoverGuard guard) {
    rt::Reason failure;
    try {
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(guard.entry).delete_(guard.key);
      co_await operation->commit();
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) rt::console::warn("[ai.ubq.fi] Failed to release Kernel default-window cutover guard:", failure);
  }

  rt::Task<quota_v2::KernelDefaultWindowCutoverDecision> acquireKernelDefaultWindowCutover(std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> expectedLimitEntry,
                                                                                           rt::KvEntryMaybe<> expectedWindowEntry) {
    using Refused = quota_v2::KernelDefaultWindowCutoverDecisionNotOk;
    std::optional<quota_v2::KernelDefaultWindowCutoverGuard> guard;
    rt::Reason failure;
    try {
      const rt::KvEntryMaybe<> markerEntry = co_await kv->get(native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, STRONG);
      if (!markerEntry.value.isNull()) {
        const contracts::KernelQuotaCutoverPlan plan = native_kernel::decideKernelDefaultWindowCutover(kernelQuotaRead(markerEntry), "", 0);
        if (const auto* refused = std::get_if<contracts::KernelQuotaCutoverPlanRefused>(&plan)) co_return Refused{.reason = refused->reason};
      }
      const double nowMs = rt::dateNow();
      const contracts::KernelQuotaCutoverPlan plan = native_kernel::decideKernelDefaultWindowCutover(kernelQuotaRead(markerEntry), rt::crypto::randomUUID(), nowMs);
      if (const auto* refused = std::get_if<contracts::KernelQuotaCutoverPlanRefused>(&plan)) co_return Refused{.reason = refused->reason};
      const contracts::KernelQuotaCutoverPlanAcquire& acquire = std::get<contracts::KernelQuotaCutoverPlanAcquire>(plan);
      const contracts::KernelDefaultWindowCutoverV2 marker = acquire.marker;
      rt::KvSetOptions lease;
      lease.expireIn = acquire.expireIn;
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(markerEntry)
        .check(expectedLimitEntry)
        .check(expectedWindowEntry)
        .set(native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, rt::KvValue(json::value_from(marker)), lease);
      const rt::KvCommitResult acquired = co_await operation->commit();
      if (!acquired.ok) co_return Refused{.reason = "concurrent_change"};
      const rt::KvEntryMaybe<> acquiredEntry = co_await kv->get(native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, STRONG);
      if (!native_kernel::kernelDefaultWindowCutoverAcquired(kernelQuotaRead(acquiredEntry), marker.id)) co_return Refused{.reason = "unavailable"};
      // `acquiredEntry as KvEntry<...>`: kernelDefaultWindowCutoverAcquired required its versionstamp.
      guard = quota_v2::KernelDefaultWindowCutoverGuard{
        .key = native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY,
        .entry = rt::KvEntry<>{acquiredEntry.key, acquiredEntry.value, acquiredEntry.versionstamp.value_or("")},
      };
      rt::Task<bool> inspecting = hasLiveDefaultBackedKernelReservations(kv);
      const bool live = co_await std::move(inspecting);
      if (live) {
        rt::Task<void> releasing = releaseKernelDefaultWindowCutover(kv, *guard);
        co_await std::move(releasing);
        co_return Refused{.reason = "active_reservations"};
      }
      co_return quota_v2::KernelDefaultWindowCutoverDecisionOk{.guard = *guard};
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to prepare Kernel default-window cutover:", failure);
    if (guard) {
      rt::Task<void> releasing = releaseKernelDefaultWindowCutover(kv, *guard);
      co_await std::move(releasing);
    }
    co_return Refused{.reason = "unavailable"};
  }
}
