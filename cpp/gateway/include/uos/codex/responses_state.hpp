#pragma once
// Port of src/codex/responses-state.ts.
// Codex Responses execution state and operations interface, split out of src/codex.ts.
//
// `CodexResponseContext` is the TS intersection `CodexResponseState & CodexResponseOperations &
// CodexResponseRunners`: one object. createCodexResponseState returns it (std::shared_ptr, so the
// installed closures can refer to a stable object) with only its CodexResponseState part set;
// installCodexResponseOperations and installCodexResponseRunners assign the std::function members
// (`ctx.x = ...` in the TS; an empty member is one not installed yet). Closures stored on the context
// must not own it (a reference cycle): capture `ctx.get()` or a std::weak_ptr. fetchCodexResponses
// keeps the context alive for the whole request.
//
// In this module `CodexBankedResetCandidate` is dispatch's candidate (`import type { ... } from
// "./dispatch.ts"`), written dispatch::CodexBankedResetCandidate. The `typeof evaluateCodexBankedResetPool`
// and `typeof reconcileCodexBankedReset` result types are banked_reset's outcome structs.
#include "uos/codex/auth.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::codex::responses_state {
  struct CodexCredentialAttempt {
    ::uos::types::CodexAuthState auth;
    routing_state::RoutingAccount routing;
  };

  // `Readonly<{ kind: "response"; response: Response }> | Readonly<{ kind: "next_account" }> |
  // Readonly<{ kind: "reselect_active" }> | Readonly<{ kind: "stop" }>`
  struct CodexAccountAttemptOutcomeResponse {
    static constexpr std::string_view kind = "response";
    rt::Response response;
  };
  struct CodexAccountAttemptOutcomeNextAccount {
    static constexpr std::string_view kind = "next_account";
  };
  struct CodexAccountAttemptOutcomeReselectActive {
    static constexpr std::string_view kind = "reselect_active";
  };
  struct CodexAccountAttemptOutcomeStop {
    static constexpr std::string_view kind = "stop";
  };
  using CodexAccountAttemptOutcome = std::variant<CodexAccountAttemptOutcomeResponse, CodexAccountAttemptOutcomeNextAccount,
                                                  CodexAccountAttemptOutcomeReselectActive, CodexAccountAttemptOutcomeStop>;

  struct CodexRetryCandidate {
    auth::CodexAuthAccountEntry accountEntry;
    ::uos::types::CodexAuthState auth;
    routing_state::RoutingAccount routing;
    double delayMs;
    double readyAtMs;
    double expiresAtMs;
  };

  struct CodexResetCohortSnapshot {
    std::string poolJson;
    std::string capacityJson;
  };

  struct PendingOverageFallback {
    std::string reason;
    auth::CodexDispatchAccountEntry accountEntry;
    routing_state::RoutingAccount routing;
  };

  struct EvaluatedBlockedReset {
    dispatch::CodexBankedResetCandidate candidate;
    // `Awaited<ReturnType<typeof reconcileCodexBankedReset>>`
    banked_reset::CodexBankedResetOutcome reset;
    /** A verified record that predates the current routing generation. */
    bool staleVerified;
    /** The active-selection row observed before this reset transaction began. */
    routing_state::CodexActiveAccountSnapshot originalActive;
  };

  // `Readonly<{ kind: "evaluated"; evaluated: EvaluatedBlockedReset }> | Readonly<{ kind: "none" }> |
  // Readonly<{ kind: "blocked"; reason: string }>`
  struct PreexistingBankedResetEvaluated {
    static constexpr std::string_view kind = "evaluated";
    EvaluatedBlockedReset evaluated;
  };
  struct PreexistingBankedResetNone {
    static constexpr std::string_view kind = "none";
  };
  struct PreexistingBankedResetBlocked {
    static constexpr std::string_view kind = "blocked";
    std::string reason;
  };
  using PreexistingBankedReset = std::variant<PreexistingBankedResetEvaluated, PreexistingBankedResetNone, PreexistingBankedResetBlocked>;

  // `type BankedResetFence = { key: KvKey; isCurrent: (value: unknown) => boolean }` (module-private in the
  // TS): the routing-mutations reconciliation fence, the same shape (banked_reset::CodexBankedResetFence too).
  using BankedResetFence = routing_mutations::CodexQuotaResetReconciliationFence;

  // `type BankedResetInput = { accountId; credentialVersion; quotaResetAtMs; routingGeneration;
  // fences: BankedResetFence[]; requestId: string | null; signal: AbortSignal | undefined }` (module-private in
  // the TS). The TS passes it wherever a banked-reset CodexBankedResetCandidate is expected and spells out the
  // same members in the same order, so the C++ type is that record (authorization and selectedCredit unset).
  using BankedResetInput = banked_reset::CodexBankedResetCandidate;

  // `type CodexResponseOperations`: assigned by installCodexResponseOperations.
  struct CodexResponseOperations {
    // requestActiveReselection's `options?: Readonly<{ preserveRetry?: boolean; routing?: RoutingAccount }>`.
    // The TS reads it only through `?.`, so `{}` stands for omitted options.
    struct RequestActiveReselectionOptions {
      std::optional<bool> preserveRetry{};
      std::optional<routing_state::RoutingAccount> routing{};
    };
    // captureBankedResetCandidate's `disposition: Readonly<{ usageLimitReached: boolean; retryAtMs: number | null;
    // resetDeadlineIsStable: boolean }>` (every caller passes those members of a Codex429Classification).
    struct CaptureBankedResetCandidateDisposition {
      bool usageLimitReached;
      std::optional<double> retryAtMs;
      bool resetDeadlineIsStable;
    };

    std::function<banked_reset::CodexBankedResetConfig()> reloadConfiguredBankedResetConfig;
    std::function<void(RequestActiveReselectionOptions options)> requestActiveReselection;
    // `error: unknown` (a caught value)
    std::function<void(rt::Reason error)> noteCodexAuthFailure;
    std::function<rt::Response(rt::Response response)> decorateAuthWarning;
    std::function<rt::Task<bool>()> freshFullCohortExhaustedProof;
    std::function<rt::Task<bool>()> freshFullCohortUnavailableProof;
    // `retryAtMs?: number | null`: std::nullopt is null and undefined alike.
    std::function<rt::Task<rt::Response>(std::optional<double> retryAtMs)> quotaBlockedOrRetryableResponse;
    // `error: CodexError | null`
    std::function<rt::Response(std::optional<auth::CodexError> error)> authFailureResponse;
    std::function<rt::Task<void>(routing_state::RoutingAccount routing)> ensureActiveRoutingAdmissionCurrent;
    // The result is null as std::nullopt.
    std::function<rt::Task<std::optional<dispatch::CodexBankedResetCandidate>>(dispatch::CodexBankedResetCandidate candidate)> refreshBankedResetCandidate;
    std::function<dispatch::CodexBankedResetCandidate(dispatch::CodexBankedResetCandidate candidate, routing_state::RoutingAccount probe)> withResetRecoveryProbe;
    std::function<BankedResetFence(dispatch::CodexBankedResetCandidate candidate)> authPoolFence;
    std::function<BankedResetFence(dispatch::CodexBankedResetCandidate candidate)> activeSelectionFence;
    std::function<BankedResetFence(routing_state::CodexActiveAccountSnapshot snapshot)> activeSnapshotFence;
    std::function<rt::Task<void>(dispatch::CodexBankedResetCandidate candidate)> ensurePostResetRetryAuthCurrent;
    std::function<BankedResetFence(rt::KvKey key, std::string expectedJson)> exactValueFence;
    std::function<std::vector<BankedResetFence>(dispatch::CodexBankedResetCandidate candidate, double routingGeneration,
                                                routing_state::CodexActiveAccountSnapshot originalActive, CodexResetCohortSnapshot cohort)>
      resetFences;
    std::function<std::vector<BankedResetFence>(dispatch::CodexBankedResetCandidate candidate, double routingGeneration)> electedResetFences;
    std::function<BankedResetInput(dispatch::CodexBankedResetCandidate candidate, double routingGeneration,
                                   routing_state::CodexActiveAccountSnapshot originalActive, CodexResetCohortSnapshot cohort)>
      resetInput;
    std::function<banked_reset::CodexBankedResetDependencies(dispatch::CodexBankedResetCandidate candidate)> resetDependenciesForCandidate;
    // `Promise<"captured" | "ineligible" | "routing_fence_unavailable">`
    std::function<rt::Task<std::string>(auth::CodexAuthAccountEntry accountEntry, routing_state::RoutingAccount routing, ::uos::types::CodexAuthState auth,
                                        CaptureBankedResetCandidateDisposition disposition)>
      captureBankedResetCandidate;
    std::function<void(banked_reset::CodexBankedResetEvent event, banked_reset::CodexBankedResetTelemetryFields fields)> logBankedResetEvent;
    std::function<rt::Task<void>()> reportHealthyFallback;
    std::function<rt::Task<PreexistingBankedReset>(std::vector<dispatch::CodexBankedResetCandidate> localCandidates,
                                                   routing_state::CodexActiveAccountSnapshot originalActive, CodexResetCohortSnapshot cohort)>
      preexistingBankedReset;
    // `evaluated: Awaited<ReturnType<typeof evaluateCodexBankedResetPool>>`; the result is null as std::nullopt.
    std::function<std::optional<EvaluatedBlockedReset>(banked_reset::CodexBankedResetPoolOutcome evaluated,
                                                       std::vector<dispatch::CodexBankedResetCandidate> localCandidates,
                                                       routing_state::CodexActiveAccountSnapshot originalActive)>
      selectedBankedResetCandidate;
    std::function<rt::Task<std::optional<EvaluatedBlockedReset>>()> evaluateBlockedCohortBankedReset;
    std::function<rt::Task<std::optional<rt::Response>>()> runOverageFallbackAttempt;
    // `beforeTransport?: () => Promise<void>` (an empty std::function is undefined) and
    // `activeAdmissionAlreadyFenced?: boolean` (std::nullopt is undefined, which the operation defaults to false).
    std::function<rt::Task<rt::Response>(auth::CodexAuthAccountEntry accountEntry, ::uos::types::CodexAuthState auth, routing_state::RoutingAccount routing,
                                         dispatch::CodexAttemptPhase phase, std::function<rt::Task<void>()> beforeTransport,
                                         std::optional<bool> activeAdmissionAlreadyFenced)>
      fetchAttempt;
    // `trigger: "401" | "proactive"`; the result `Readonly<{ auth; routing }>` is a CodexCredentialAttempt.
    std::function<rt::Task<CodexCredentialAttempt>(routing_state::RoutingAccount routing, ::uos::types::CodexAuthState auth, std::string trigger)>
      refreshAfter401;
    std::function<rt::Task<rt::Response>(auth::CodexAuthAccountEntry accountEntry, routing_state::RoutingAccount routing, ::uos::types::CodexAuthState auth,
                                         rt::Response response)>
      classify429;
    std::function<rt::Task<rt::Response>(rt::Response response)> authorizePaidFallbackForCompleteCapacityEvidence;
    // `record: NonNullable<Awaited<ReturnType<typeof reconcileCodexBankedReset>>>["record"]` (`CodexResetRedemptionRecord | null`)
    std::function<rt::Task<void>(dispatch::CodexBankedResetCandidate candidate, std::optional<::uos::types::CodexResetRedemptionRecord> record)>
      persistVerifiedCapacityReset;
  };

  // `type CodexResponseRunners`: assigned by installCodexResponseRunners. `Response | null` is
  // std::optional<rt::Response>.
  struct CodexResponseRunners {
    // claimAttemptRouting's `Readonly<{ kind: "claimed"; routing: RoutingAccount } | { kind: "unavailable" }>`
    struct ClaimAttemptRoutingResultClaimed {
      static constexpr std::string_view kind = "claimed";
      routing_state::RoutingAccount routing;
    };
    struct ClaimAttemptRoutingResultUnavailable {
      static constexpr std::string_view kind = "unavailable";
    };
    using ClaimAttemptRoutingResult = std::variant<ClaimAttemptRoutingResultClaimed, ClaimAttemptRoutingResultUnavailable>;

    // recoverCodexAuthFailure's `Readonly<{ kind: "recovered"; response: Response } | { kind: "reselect_active" }>`
    struct RecoverCodexAuthFailureResultRecovered {
      static constexpr std::string_view kind = "recovered";
      rt::Response response;
    };
    struct RecoverCodexAuthFailureResultReselectActive {
      static constexpr std::string_view kind = "reselect_active";
    };
    using RecoverCodexAuthFailureResult = std::variant<RecoverCodexAuthFailureResultRecovered, RecoverCodexAuthFailureResultReselectActive>;

    // prepareTwoSecondRetry's `Readonly<{ kind: "ready"; attempt: CodexCredentialAttempt; delayMs: number } |
    // { kind: "respond"; response: Response }>`
    struct PrepareTwoSecondRetryResultReady {
      static constexpr std::string_view kind = "ready";
      CodexCredentialAttempt attempt;
      double delayMs;
    };
    struct PrepareTwoSecondRetryResultRespond {
      static constexpr std::string_view kind = "respond";
      rt::Response response;
    };
    using PrepareTwoSecondRetryResult = std::variant<PrepareTwoSecondRetryResultReady, PrepareTwoSecondRetryResultRespond>;

    // recoverTwoSecondRetryAuthFailure's `Readonly<{ kind: "recovered"; response: Response } |
    // { kind: "respond"; response: Response }>`
    struct RecoverTwoSecondRetryAuthFailureResultRecovered {
      static constexpr std::string_view kind = "recovered";
      rt::Response response;
    };
    struct RecoverTwoSecondRetryAuthFailureResultRespond {
      static constexpr std::string_view kind = "respond";
      rt::Response response;
    };
    using RecoverTwoSecondRetryAuthFailureResult =
      std::variant<RecoverTwoSecondRetryAuthFailureResultRecovered, RecoverTwoSecondRetryAuthFailureResultRespond>;

    // reselectedActiveIdentity's `Readonly<{ accountIdHash: string | undefined; generation: number | undefined }>`
    struct ReselectedActiveIdentityResult {
      std::optional<std::string> accountIdHash;
      std::optional<double> generation;
    };

    std::function<bool(::uos::types::CodexAuthState auth, rt::Response response)> responseIsCodexAuthFailure;
    std::function<rt::Task<rt::Response>(dispatch::CodexBankedResetCandidate retryCandidate, rt::Response retried, std::optional<rt::Response> normalResponse)>
      finalizePostResetRetry;
    std::function<rt::Task<std::optional<rt::Response>>(EvaluatedBlockedReset evaluated, std::optional<rt::Response> normalResponse)> runPostResetRetry;
    std::function<rt::Task<rt::Response>(rt::Response normalResponse)> redeemAndRetryOnce;
    std::function<rt::Task<std::optional<rt::Response>>()> recoverBlockedReset;
    // `retryAtMs: number | null`
    std::function<rt::Task<rt::Response>(std::optional<double> retryAtMs)> exhaustedQuotaBlockedCohort;
    std::function<rt::Task<ClaimAttemptRoutingResult>(routing_state::RoutingAccount routing)> claimAttemptRouting;
    // `attempt` is shared: the TS mutates the caller's attempt object (`attempt.auth = refreshed.auth`), which
    // attemptCodexAccount then passes on to classifyAccountAttemptResponse.
    std::function<rt::Task<RecoverCodexAuthFailureResult>(auth::CodexDispatchAccountEntry accountEntry, std::shared_ptr<CodexCredentialAttempt> attempt,
                                                          rt::Response response)>
      recoverCodexAuthFailure;
    std::function<rt::Task<CodexAccountAttemptOutcome>(auth::CodexDispatchAccountEntry accountEntry, CodexCredentialAttempt attempt, rt::Response response)>
      classifyAccountAttemptResponse;
    std::function<rt::Task<CodexAccountAttemptOutcome>(auth::CodexError error, routing_state::RoutingAccount routing)> absorbSiblingTransportFailure;
    // `error: unknown` (a caught value)
    std::function<rt::Task<CodexAccountAttemptOutcome>(rt::Reason error, routing_state::RoutingAccount routing)> handleAccountAttemptFailure;
    std::function<rt::Task<CodexAccountAttemptOutcome>(auth::CodexDispatchAccountEntry accountEntry)> attemptCodexAccount;
    std::function<rt::Task<std::optional<rt::Response>>()> dispatchEligibleAccounts;
    std::function<rt::Task<std::optional<rt::Response>>()> terminalTransportOutcome;
    std::function<rt::Task<rt::Response>()> redeemOrReportQuotaBlocked;
    std::function<rt::Task<PrepareTwoSecondRetryResult>(CodexRetryCandidate candidate)> prepareTwoSecondRetry;
    // `attempt` is shared, as in recoverCodexAuthFailure (runTwoSecondRetryCandidate reads the refreshed attempt).
    std::function<rt::Task<RecoverTwoSecondRetryAuthFailureResult>(CodexRetryCandidate candidate, std::shared_ptr<CodexCredentialAttempt> attempt,
                                                                   rt::Response response)>
      recoverTwoSecondRetryAuthFailure;
    std::function<rt::Task<rt::Response>(CodexRetryCandidate candidate, CodexCredentialAttempt attempt, rt::Response response)> classifyTwoSecondRetryResponse;
    std::function<rt::Task<rt::Response>(CodexRetryCandidate candidate)> runTwoSecondRetryCandidate;
    std::function<rt::Task<rt::Response>()> exhaustedCodexDispatchResponse;
    std::function<rt::Task<std::optional<rt::Response>>()> runPendingShortRetry;
    // The result is null as std::nullopt.
    std::function<std::optional<ReselectedActiveIdentityResult>()> reselectedActiveIdentity;
    std::function<bool()> restorePreservedShortRetry;
    std::function<rt::Task<std::optional<rt::Response>>()> advanceActiveReselection;
  };

  struct CodexResponseContext;

  // createCodexResponseState's result (inferred in the TS), by `kind`.
  struct CreateCodexResponseStateResultTerminal {
    static constexpr std::string_view kind = "terminal";
    rt::Response response;
  };
  struct CreateCodexResponseStateResultState {
    static constexpr std::string_view kind = "state";
    // The TS state object, which fetchCodexResponses then uses as the context: only its
    // CodexResponseState part is set.
    std::shared_ptr<CodexResponseContext> state;
  };
  using CreateCodexResponseStateResult = std::variant<CreateCodexResponseStateResultTerminal, CreateCodexResponseStateResultState>;

  [[nodiscard]] rt::Task<CreateCodexResponseStateResult> createCodexResponseState(std::shared_ptr<const dispatch::PreparedCodexSubscriptionRequest> prepared,
                                                                                  dispatch::FetchCodexResponsesOptions options,
                                                                                  dispatch::CodexProviderDispatchCoordinator providerDispatch);

  // `Extract<Awaited<ReturnType<typeof createCodexResponseState>>, { kind: "state" }>["state"]`: the
  // members createCodexResponseState builds, in its order.
  struct CodexResponseState {
    // `authFailureState: { error: CodexError | null }`
    struct AuthFailureState {
      std::optional<auth::CodexError> error;
    };
    // `transportState: { started: boolean }`
    struct TransportState {
      bool started;
    };
    // `retryState: { candidate: CodexRetryCandidate | null }`
    struct RetryState {
      std::optional<CodexRetryCandidate> candidate;
    };
    // `preservedShortRetryState: { current: Readonly<{ candidate; accountIdHash; activeGeneration }> | null }`
    struct PreservedShortRetryState {
      struct Current {
        CodexRetryCandidate candidate;
        std::string accountIdHash;
        double activeGeneration;
      };
      std::optional<Current> current;
    };

    std::shared_ptr<const dispatch::PreparedCodexSubscriptionRequest> prepared;
    dispatch::FetchCodexResponsesOptions options;
    dispatch::CodexProviderDispatchCoordinator providerDispatch;
    std::optional<std::string> requestedModel;
    auth::CodexAuthPoolEntry poolEntry;
    routing_state::RouteSelection selected;
    std::vector<auth::CodexDispatchAccountEntry> accountEntries;
    std::string url;
    rt::Headers baseHeaders;
    // `options.bankedReset` and its `config`: std::nullopt is undefined.
    std::optional<dispatch::CodexBankedResetOptions> configuredBankedReset;
    std::optional<banked_reset::CodexBankedResetConfig> configuredBankedResetConfig;
    banked_reset::CodexBankedResetDependencies bankedResetDependencies;
    std::optional<rt::Response> lastResponse;
    // `lastError: unknown`, initially null: a null Reason (read by dispatchFailureAsError as absent).
    rt::Reason lastError;
    std::optional<auth::CodexError> transportFailure;
    std::optional<std::string> authWarning;
    AuthFailureState authFailureState;
    bool probeUnavailable;
    std::optional<routing_state::CodexProbeCircuit> probeUnavailableCircuit;
    double attemptNumber;
    struct StreamAttempt {
      json::object log;
      std::string accountId;
      bool retryAllowed;
    };
    std::optional<StreamAttempt> streamAttempt{};
    bool streamRetryPending = false;
    TransportState transportState;
    // `Set<number>` of 0-based slots.
    rt::OrderedSet<double> refreshedSlots;
    RetryState retryState;
    // `Map<number, CodexBankedResetCandidate>` keyed by 0-based slot, in insertion order.
    rt::OrderedMap<double, dispatch::CodexBankedResetCandidate> bankedResetCandidates;
    bool activeReselectionRequested;
    PreservedShortRetryState preservedShortRetryState;
    std::optional<PendingOverageFallback> overageFallbackPending;
  };

  // `CodexResponseState & CodexResponseOperations & CodexResponseRunners`
  struct CodexResponseContext : CodexResponseState, CodexResponseOperations, CodexResponseRunners {};
}
