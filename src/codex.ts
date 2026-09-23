import { config } from "./config.ts";
import {
  claimCodexRoutingProbe,
  electCodexResetRecoveryAccount,
  getCodexQuotaBlockFence,
  isCodexActiveAccountSnapshotCurrent,
  isCodexQuotaBlockFenceCurrent,
  markCodexCredentialInvalid,
  markCodexRecoveryProbeQuotaBlocked,
  reconcileCodexQuotaAfterStaleVerifiedReset,
  reconcileCodexQuotaAfterVerifiedReset,
  reconcileCodexRoutingAccount,
  refreshCodexActiveAccountAdmission,
  releaseCodexRoutingProbe,
  selectCodexRoutingAccountsStrong,
} from "./codex_account_routing.ts";
import { RoutingAccount } from "./codex_routing_state.ts";

import { markCodexQuotaBlocked } from "./codex_429.ts";
import { isCodexActiveAccountSelectionCurrent } from "./codex_routing_state.ts";
import { CodexProbeCircuit } from "./codex_routing_state.ts";

import { CodexActiveAccountSnapshot } from "./codex_routing_state.ts";
import { CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY } from "./codex_routing_state.ts";
import { CODEX_ACCOUNT_ROUTING_KV_KEY } from "./codex_routing_state.ts";
import { CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY } from "./codex_routing_state.ts";
import {
  type CodexBankedResetConfig,
  type CodexBankedResetDependencies,
  type CodexBankedResetEvent,
  type CodexBankedResetTelemetryFields,
  loadCodexBankedResetConfig,
  reportCodexBankedResetEvent,
  reportCodexBankedResetMetric,
} from "./codex_banked_reset.ts";
import { evaluateCodexBankedResetPool } from "./codex_banked_reset_pool.ts";
import { reconcileCodexBankedReset } from "./codex_banked_reset_submission.ts";
import { createUpstreamCodexUsageResetProvider, unavailableCodexUsageResetProvider } from "./codex_banked_reset_provider.ts";

import { getKv } from "./kv.ts";
import { ApiKeyQuotaDispatchError } from "./api_key_policy.ts";

import { recordProviderCapacityResetEvent, triggerProviderCapacitySample } from "./provider_capacity_events.ts";
import type { SentinelUpstreamRecorder } from "./sentinel_upstream_capture.ts";
import { sha256Hex } from "./utils.ts";
import type { CodexAuthState } from "./types.ts";

import type { CodexAuthAccountEntry, CodexDispatchAccountEntry } from "./codex_auth.ts";
import {
  CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS,
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_AUTH_REAUTH_MESSAGE,
  CODEX_AUTH_REAUTH_WARNING,
  CODEX_ORIGINATOR,
  CODEX_QUOTA_BLOCKED_ERROR_CODE,
  CodexError,
  accessTokenExpired,
  codexAuthWarningForError,
  codexProbeByResponse,
  codexRoutingErrors,
  codexUserAgent,
  getAuthPoolEntry,
  inheritCodexResponseActiveTelemetry,
  needsRefresh,
  parseCodexAuthPool,
  setCodexResponseAccountTelemetry,
  setCodexResponseActiveTelemetry,
  withCodexAuthWarning,
  withCodexWarnings,
} from "./codex_auth.ts";
import { awaitWithoutCancellingSharedWork, delay, getValidAuth, refreshAuthCoordinated, sameCodexCredentials } from "./codex_auth_refresh.ts";
import type {
  CodexAttemptPhase,
  CodexBankedResetCandidate,
  CodexProviderDispatchCoordinator,
  FetchCodexResponsesOptions,
  PreparedCodexSubscriptionRequest,
} from "./codex_dispatch.ts";
import {
  CodexActiveAccountFenceError,
  CodexBankedResetRetryFenceError,
  applyNativeSessionHeaders,
  awaitPendingCodexProbeTransitions,
  cancelResponseBody,
  codexErrorClass,
  codexStatusClass,
  createCodexProviderDispatchCoordinator,
  dispatchFailureAsError,
  fetchCodexResponseWithAuth,
  initialCodexSelectionResponse,
  isCodexSiblingTransportFailure,
  logCodexRouting,
  prepareCodexSubscriptionRequest,
  recordCodexResponseHealth,
  recordCodexThrownHealth,
  reportCodexResponseTiming,
  requestedCodexModel,
  routingErrorResponse,
  runCodexSerialAdmissionLoop,
  upstreamTimeoutCircuitResponse,
  waitForCodexRetry,
} from "./codex_dispatch.ts";

const fetchPreparedCodexResponses = async (
  prepared: PreparedCodexSubscriptionRequest,
  options: FetchCodexResponsesOptions,
  providerDispatch: CodexProviderDispatchCoordinator
): Promise<Response> => {
  await awaitPendingCodexProbeTransitions();
  const body = prepared.body;
  const requestedModel = requestedCodexModel(body);
  let poolEntry = await getAuthPoolEntry(true, true);
  let selected = await selectCodexRoutingAccountsStrong(poolEntry.pool, poolEntry.pool.accounts, Date.now(), requestedModel);
  const terminalSelection = initialCodexSelectionResponse(selected);
  if (terminalSelection) return terminalSelection;
  let accountEntries: CodexDispatchAccountEntry[] =
    selected.kind === "eligible" ? selected.accounts.map((routing) => ({ ...poolEntry, auth: routing.auth, routing })) : [];
  const url = `${config.codexBaseUrl}/responses`;
  const serializedBody = prepared.serializedBody;
  const baseHeaders = new Headers({
    originator: CODEX_ORIGINATOR,
    "user-agent": codexUserAgent(options.clientVersion),
    "Content-Type": "application/json",
    Accept: "text/event-stream",
    conversation_id: prepared.conversationIdentity,
  });
  applyNativeSessionHeaders(baseHeaders, prepared.nativeSessionIdentity);
  const configuredBankedReset = options.bankedReset;
  const configuredBankedResetConfig = configuredBankedReset?.config;
  // A pinned test configuration reloads to itself; without one the process
  // environment stays authoritative on every reload.
  const reloadConfiguredBankedResetConfig = (): CodexBankedResetConfig => configuredBankedResetConfig ?? loadCodexBankedResetConfig();
  const bankedResetDependencies: CodexBankedResetDependencies = {
    config: configuredBankedResetConfig ?? loadCodexBankedResetConfig(),
    reloadConfig: configuredBankedReset?.reloadConfig ?? (configuredBankedResetConfig ? reloadConfiguredBankedResetConfig : loadCodexBankedResetConfig),
    provider: configuredBankedReset?.provider ?? unavailableCodexUsageResetProvider,
    kv: configuredBankedReset?.kv,
    now: configuredBankedReset?.now,
    newOwnerToken: configuredBankedReset?.newOwnerToken,
    hash: configuredBankedReset?.hash,
    telemetry: configuredBankedReset?.telemetry,
    // A supplied provider exists only as the hermetic `fetchCodexResponses`
    // test seam. Real traffic creates its account-bound adapter below and
    // therefore always enforces a durable decision before live submission.
    allowLiveWithoutShadowForTest: configuredBankedReset?.provider !== undefined,
  };
  let lastResponse: Response | null = null;
  let lastError: unknown = null;
  let transportFailure: CodexError | null = null;
  let authWarning: string | null = null;
  // Written from callbacks that control-flow analysis cannot follow: an auth
  // failure is recorded by `noteCodexAuthFailure` and the transport-start flag
  // by the dispatch hook. Holders keep both read sites honestly
  // nullable/boolean instead of narrowing them to their initial literals.
  const authFailureState: { error: CodexError | null } = { error: null };
  let probeUnavailable = false;
  let probeUnavailableCircuit: CodexProbeCircuit | null = null;
  let attemptNumber = 0;
  const transportState: { started: boolean } = { started: false };
  const refreshedSlots = new Set<number>();
  const retryState: {
    candidate: Readonly<{
      accountEntry: CodexAuthAccountEntry;
      auth: CodexAuthState;
      routing: RoutingAccount;
      delayMs: number;
      readyAtMs: number;
      expiresAtMs: number;
    }> | null;
  } = { candidate: null };
  const bankedResetCandidates = new Map<number, CodexBankedResetCandidate>();
  let activeReselectionRequested = false;

  /** The credentials and routing one attempt dispatches with; a refresh replaces both. */
  type CodexCredentialAttempt = { auth: CodexAuthState; routing: RoutingAccount };
  type CodexAccountAttemptOutcome =
    | Readonly<{ kind: "response"; response: Response }>
    | Readonly<{ kind: "next_account" }>
    | Readonly<{ kind: "reselect_active" }>
    | Readonly<{ kind: "stop" }>;
  type CodexRetryCandidate = NonNullable<(typeof retryState)["candidate"]>;

  /**
   * A short bounded retry captured by an authoritative quota classification.
   * It survives the strong reselect only when the same opaque active identity
   * and active generation are re-admitted; any real switch discards it. The
   * holder keeps closure writes visible to control-flow analysis.
   */
  const preservedShortRetryState: {
    current: Readonly<{ candidate: CodexRetryCandidate; accountIdHash: string; activeGeneration: number }> | null;
  } = { current: null };

  const requestActiveReselection = (options?: Readonly<{ preserveRetry?: boolean; routing?: RoutingAccount }>): void => {
    activeReselectionRequested = true;
    const candidate = retryState.candidate;
    retryState.candidate = null;
    const routing = options?.routing;
    if (options?.preserveRetry === true && candidate !== null && routing !== undefined) {
      const activeGeneration = routing.activeGeneration;
      if (activeGeneration !== undefined && Number.isSafeInteger(activeGeneration)) {
        preservedShortRetryState.current = { candidate, accountIdHash: routing.accountIdHash, activeGeneration };
        return;
      }
    }
    preservedShortRetryState.current = null;
  };

  const noteCodexAuthFailure = (error: unknown): void => {
    authWarning ??= codexAuthWarningForError(error) ?? CODEX_AUTH_REAUTH_WARNING;
    if (error instanceof CodexError && error.status === 401) authFailureState.error ??= error;
  };

  const decorateAuthWarning = (response: Response): Response => (authWarning ? withCodexAuthWarning(response, authWarning) : response);

  /**
   * Paid fallback requires a fresh strong proof that every current pool
   * account is authoritatively exhausted. A prior request-local proof is never
   * reused: a reset, probe, credential change or pool rotation after the
   * classification invalidates it.
   */
  const freshFullCohortExhaustedProof = async (): Promise<boolean> => {
    try {
      const currentPoolEntry = await getAuthPoolEntry(true, true);
      const currentSelection = await selectCodexRoutingAccountsStrong(currentPoolEntry.pool, currentPoolEntry.pool.accounts, Date.now(), requestedModel);
      return currentSelection.kind === "quota_blocked" && currentSelection.fullCohortExhausted;
    } catch {
      return false;
    }
  };

  /** A freshly proven all-exhausted cohort keeps the authoritative quota marker; anything less stays retryable. */
  const quotaBlockedOrRetryableResponse = async (retryAtMs: number | null = null): Promise<Response> =>
    (await freshFullCohortExhaustedProof())
      ? routingErrorResponse(429, "All configured Codex accounts are quota-blocked; retry after their next reset.", CODEX_QUOTA_BLOCKED_ERROR_CODE, retryAtMs)
      : routingErrorResponse(429, "Codex capacity is temporarily unavailable; retry the request.", "codex_capacity_unavailable", retryAtMs);

  const authFailureResponse = (error: CodexError | null): Response => {
    const response = routingErrorResponse(
      401,
      error?.message ?? CODEX_AUTH_REAUTH_MESSAGE,
      error?.code === "refresh_token_reused" ? error.code : "codex_auth_invalid"
    );
    return decorateAuthWarning(response);
  };

  /**
   * The selector linearizes every admission. Recheck the durable active,
   * auth-pool, routing and capacity rows immediately before transport. The
   * same-account probe claim or credential refresh this request performed
   * refreshes only the fence fields; a concurrent active switch or pool
   * rotation fails closed and asks for reselection.
   */
  const ensureActiveRoutingAdmissionCurrent = async (routing: RoutingAccount): Promise<void> => {
    if (
      routing.activeGeneration === undefined ||
      !Number.isSafeInteger(routing.activeGeneration) ||
      routing.activeGeneration < 1 ||
      typeof routing.activePoolVersionstamp !== "string" ||
      routing.activePoolVersionstamp.length === 0
    ) {
      throw new CodexActiveAccountFenceError();
    }
    try {
      if (!(await refreshCodexActiveAccountAdmission(routing))) throw new CodexActiveAccountFenceError();
    } catch (error) {
      if (error instanceof CodexActiveAccountFenceError) throw error;
      throw new CodexActiveAccountFenceError();
    }
  };

  const refreshBankedResetCandidate = async (candidate: CodexBankedResetCandidate): Promise<CodexBankedResetCandidate | null> => {
    try {
      // A banked reset is stricter than normal routing: force-read the auth
      // pool immediately before the durable side-effect path. A rotated or
      // reordered account is not eligible for an old candidate.
      const currentPoolEntry = await getAuthPoolEntry(true, true);
      const currentAuth = currentPoolEntry.pool.accounts.at(candidate.routing.slot);
      if (!currentAuth) return null;
      if (currentAuth.account_id !== candidate.auth.account_id || !sameCodexCredentials(currentAuth, candidate.auth)) return null;
      return {
        ...candidate,
        auth: currentAuth,
        accountEntry: { ...currentPoolEntry, auth: currentAuth, routing: candidate.routing },
      };
    } catch {
      return null;
    }
  };

  /**
   * Reconciliation returns a fenced recovery probe. Keep the newly read auth
   * material but dispatch the one permitted post-reset retry through that
   * probe so a late pre-reset 429 cannot clear or rewrite its tombstone.
   */
  const withResetRecoveryProbe = (candidate: CodexBankedResetCandidate, probe: RoutingAccount): CodexBankedResetCandidate => {
    const routing = { ...probe, auth: candidate.auth };
    return {
      ...candidate,
      routing,
      accountEntry: { ...candidate.accountEntry, auth: candidate.auth, routing },
    };
  };

  const authPoolFence = (candidate: CodexBankedResetCandidate) => ({
    key: CODEX_AUTH_POOL_KV_KEY,
    isCurrent: (value: unknown): boolean => {
      const current = parseCodexAuthPool(value)?.accounts[candidate.routing.slot];
      return current?.account_id === candidate.auth.account_id && sameCodexCredentials(current, candidate.auth);
    },
  });

  const activeSelectionFence = (candidate: CodexBankedResetCandidate) => ({
    key: CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY,
    isCurrent: (value: unknown): boolean =>
      candidate.routing.activeGeneration !== undefined && isCodexActiveAccountSelectionCurrent(value, candidate.routing, candidate.routing.activeGeneration),
  });

  /**
   * Inventory, arming and consume bind to the active-selection row exactly as
   * the strong decision observed it, including a validly absent row. The
   * verified account is elected only after the consume, so an inactive
   * sibling's credit can be considered without letting it take over.
   */
  const activeSnapshotFence = (snapshot: CodexActiveAccountSnapshot) => ({
    key: CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY,
    isCurrent: (value: unknown): boolean => isCodexActiveAccountSnapshotCurrent(value, snapshot),
  });

  /**
   * This runs inside the transport's final dispatch hook, after any API-key
   * reservation hook and immediately before `fetch`. It closes the last
   * awaitable auth-pool rotation window for a post-reset retry.
   */
  const ensurePostResetRetryAuthCurrent = async (candidate: CodexBankedResetCandidate): Promise<void> => {
    const currentPoolEntry = await getAuthPoolEntry(true, true);
    const currentAuth = currentPoolEntry.pool.accounts.at(candidate.routing.slot);
    if (!currentAuth) throw new CodexBankedResetRetryFenceError();
    if (currentAuth.account_id !== candidate.auth.account_id || !sameCodexCredentials(currentAuth, candidate.auth)) {
      throw new CodexBankedResetRetryFenceError();
    }
  };

  /**
   * The exact full auth-pool and stored-capacity snapshots the strong
   * quota-blocked decision observed. A positive capacity sample or any pool
   * membership/credential/update change after preflight invalidates every
   * pre-election reset fence, so an unnecessary consume cannot proceed.
   */
  type CodexResetCohortSnapshot = Readonly<{ poolJson: string; capacityJson: string }>;

  const exactValueFence = (key: Deno.KvKey, expectedJson: string) => ({
    key,
    isCurrent: (value: unknown): boolean => JSON.stringify(value ?? null) === expectedJson,
  });

  const resetFences = (
    candidate: CodexBankedResetCandidate,
    routingGeneration: number,
    originalActive: CodexActiveAccountSnapshot,
    cohort: CodexResetCohortSnapshot
  ) => [
    {
      key: CODEX_ACCOUNT_ROUTING_KV_KEY,
      isCurrent: (value: unknown): boolean => isCodexQuotaBlockFenceCurrent(value, candidate.routing, candidate.quotaResetAtMs, routingGeneration),
    },
    {
      key: CODEX_AUTH_POOL_KV_KEY,
      isCurrent: (value: unknown): boolean => authPoolFence(candidate).isCurrent(value),
    },
    activeSnapshotFence(originalActive),
    exactValueFence(CODEX_AUTH_POOL_KV_KEY, cohort.poolJson),
    exactValueFence(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, cohort.capacityJson),
  ];

  /** Post-election probe fences: the elected active row plus the candidate's auth proof. */
  const electedResetFences = (candidate: CodexBankedResetCandidate, routingGeneration: number) => [
    {
      key: CODEX_ACCOUNT_ROUTING_KV_KEY,
      isCurrent: (value: unknown): boolean => isCodexQuotaBlockFenceCurrent(value, candidate.routing, candidate.quotaResetAtMs, routingGeneration),
    },
    {
      key: CODEX_AUTH_POOL_KV_KEY,
      isCurrent: (value: unknown): boolean => authPoolFence(candidate).isCurrent(value),
    },
    activeSelectionFence(candidate),
  ];

  const resetInput = (
    candidate: CodexBankedResetCandidate,
    routingGeneration: number,
    originalActive: CodexActiveAccountSnapshot,
    cohort: CodexResetCohortSnapshot
  ) => ({
    accountId: candidate.auth.account_id,
    credentialVersion: candidate.routing.credentialVersion,
    quotaResetAtMs: candidate.quotaResetAtMs,
    routingGeneration,
    fences: resetFences(candidate, routingGeneration, originalActive, cohort),
    requestId: options.requestId ?? null,
    signal: options.signal,
  });

  /**
   * Bind the reset transport to the freshly fenced credentials that produced
   * this candidate. Credentials stay in this closure rather than entering the
   * durable transaction context. Tests inject a provider explicitly and never
   * reach this transport.
   */
  const resetDependenciesForCandidate = (candidate: CodexBankedResetCandidate): CodexBankedResetDependencies => {
    if (configuredBankedReset?.provider) return bankedResetDependencies;
    try {
      return {
        ...bankedResetDependencies,
        provider: createUpstreamCodexUsageResetProvider({
          codexBaseUrl: config.codexBaseUrl,
          accountId: candidate.auth.account_id,
          accessToken: candidate.auth.access_token,
          userAgent: codexUserAgent(options.clientVersion),
          now: bankedResetDependencies.now,
        }),
      };
    } catch {
      return bankedResetDependencies;
    }
  };

  const captureBankedResetCandidate = async (
    accountEntry: CodexAuthAccountEntry,
    routing: RoutingAccount,
    auth: CodexAuthState,
    disposition: Readonly<{
      usageLimitReached: boolean;
      retryAtMs: number | null;
      resetDeadlineIsStable: boolean;
    }>
  ): Promise<"captured" | "ineligible" | "routing_fence_unavailable"> => {
    // Relative Retry-After delays are valid for ordinary routing, but their
    // Date.now-derived deadline cannot name a durable reset window. A
    // canonical absolute HTTP-date is only a provisional identity; the
    // durable no-revision fence is what permits it to receive a key.
    if (!disposition.usageLimitReached || disposition.retryAtMs === null || !disposition.resetDeadlineIsStable) {
      return "ineligible";
    }
    const routingGeneration = await getCodexQuotaBlockFence(routing, disposition.retryAtMs);
    if (routingGeneration === null) {
      // A revised quota window invalidates any older observation for this
      // slot. It must not remain selectable just because capture could not
      // establish a fresh fence for the new deadline.
      bankedResetCandidates.delete(routing.slot);
      return "routing_fence_unavailable";
    }
    bankedResetCandidates.set(routing.slot, {
      accountEntry,
      auth,
      routing,
      quotaResetAtMs: disposition.retryAtMs,
      routingGeneration,
    });
    return "captured";
  };

  const logBankedResetEvent = (event: CodexBankedResetEvent, fields: CodexBankedResetTelemetryFields): void => {
    reportCodexBankedResetEvent(bankedResetDependencies.telemetry, event, fields);
  };

  const reportHealthyFallback = async (): Promise<void> => {
    for (const candidate of bankedResetCandidates.values()) {
      try {
        const hash = bankedResetDependencies.hash ?? sha256Hex;
        const accountIdHash = await hash(candidate.auth.account_id);
        const quotaGeneration = await hash(`uos_ai\u0000codex_reset_generation\u0000${accountIdHash}\u0000${candidate.quotaResetAtMs}`);
        logBankedResetEvent("codex_reset_skipped_healthy_fallback", {
          request_id: options.requestId ?? null,
          account_id_hash: accountIdHash,
          quota_generation: `v1:${quotaGeneration}`,
          routing_generation: candidate.routingGeneration,
        });
      } catch {
        // A telemetry hash failure must not delay a healthy inference response.
      }
    }
  };

  type EvaluatedBlockedReset = Readonly<{
    candidate: CodexBankedResetCandidate;
    reset: Awaited<ReturnType<typeof reconcileCodexBankedReset>>;
    /** A verified record that predates the current routing generation. */
    staleVerified: boolean;
    /** The active-selection row observed before this reset transaction began. */
    originalActive: CodexActiveAccountSnapshot;
  }>;

  type PreexistingBankedReset = Readonly<{ kind: "evaluated"; evaluated: EvaluatedBlockedReset } | { kind: "none" } | { kind: "blocked" }>;

  /**
   * Preserve a pre-existing durable transaction before considering a new
   * shadow/live decision. A pending or rejected legacy transaction blocks a
   * replacement spend; a verified one is returned for its single retry. The
   * recovery probe is acquired only after the verified account is elected.
   */
  const preexistingBankedReset = async (
    localCandidates: readonly CodexBankedResetCandidate[],
    originalActive: CodexActiveAccountSnapshot,
    cohort: CodexResetCohortSnapshot
  ): Promise<PreexistingBankedReset> => {
    for (const candidate of localCandidates) {
      const reset = await reconcileCodexBankedReset(
        resetInput(candidate, candidate.routingGeneration, originalActive, cohort),
        resetDependenciesForCandidate(candidate)
      );
      if (reset.kind === "verified") {
        return { kind: "evaluated", evaluated: { candidate, reset, staleVerified: false, originalActive } };
      }
      if (reset.reason === "verified_routing_generation_stale") {
        return { kind: "evaluated", evaluated: { candidate, reset, staleVerified: true, originalActive } };
      }
      if (reset.reason !== "no_existing_transaction") return { kind: "blocked" };
    }
    return { kind: "none" };
  };

  /** The pooled evaluation only spends a reset for a candidate in the proven cohort. */
  const selectedBankedResetCandidate = (
    evaluated: Awaited<ReturnType<typeof evaluateCodexBankedResetPool>>,
    localCandidates: readonly CodexBankedResetCandidate[],
    originalActive: CodexActiveAccountSnapshot
  ): EvaluatedBlockedReset | null => {
    if (!evaluated.selected || !evaluated.reset) return null;
    const selectedSlot = evaluated.selected.slot;
    const candidate = localCandidates.find((item) => item.routing.slot === selectedSlot);
    return candidate ? { candidate, reset: evaluated.reset, staleVerified: false, originalActive } : null;
  };

  /** Re-read the all-exhausted pool before the one permitted banked-reset evaluation. */
  const evaluateBlockedCohortBankedReset = async (): Promise<EvaluatedBlockedReset | null> => {
    if (probeUnavailable) return null;
    let currentPoolEntry: Awaited<ReturnType<typeof getAuthPoolEntry>>;
    try {
      currentPoolEntry = await getAuthPoolEntry(true, true);
    } catch {
      return null;
    }
    const routedPool = await selectCodexRoutingAccountsStrong(currentPoolEntry.pool, currentPoolEntry.pool.accounts, Date.now(), requestedModel);
    if (routedPool.kind === "routing_unavailable") {
      logCodexRouting("codex_banked_reset_preflight", {
        request_id: options.requestId ?? null,
        require_full_pool: "true",
        outcome: "routing_unavailable",
        reason: "strong_routing_read_failed",
      });
      return null;
    }
    // Every current account must be authoritatively exhausted. A held recovery
    // lease, an invalid credential, or unclassified state is not quota
    // exhaustion and must not spend a credit.
    if (routedPool.kind !== "quota_blocked" || !routedPool.fullCohortExhausted) return null;
    const blockedAccounts = routedPool.blockedAccounts;
    if (!blockedAccounts.length) return null;
    if (routedPool.poolSnapshotJson === null || routedPool.capacitySnapshotJson === null) return null;
    const cohort: CodexResetCohortSnapshot = { poolJson: routedPool.poolSnapshotJson, capacityJson: routedPool.capacitySnapshotJson };
    const originalActive = routedPool.activeSnapshot;
    // Consider every blocked account's credit inventory. The original active
    // snapshot fences inventory, arming and consume; the selected account is
    // elected only after its reset is verified.
    const localCandidates = blockedAccounts.map(
      (routing) =>
        ({
          accountEntry: { ...currentPoolEntry, auth: routing.auth, routing },
          auth: routing.auth,
          routing,
          quotaResetAtMs: routing.quotaResetAtMs,
          routingGeneration: routing.routingGeneration,
        }) satisfies CodexBankedResetCandidate
    );

    const preexisting = await preexistingBankedReset(localCandidates, originalActive, cohort);
    if (preexisting.kind === "blocked") return null;
    if (preexisting.kind === "evaluated") return preexisting.evaluated;

    const poolCandidates = localCandidates.map((candidate) => ({
      slot: candidate.routing.slot,
      candidate: resetInput(candidate, candidate.routingGeneration, originalActive, cohort),
      provider: resetDependenciesForCandidate(candidate).provider,
    }));
    const evaluated = await evaluateCodexBankedResetPool(poolCandidates, bankedResetDependencies);
    logCodexRouting("codex_banked_reset_preflight", {
      request_id: options.requestId ?? null,
      require_full_pool: "true",
      outcome: evaluated.kind,
      reason: evaluated.reason,
      candidate_count: poolCandidates.length,
      selected_slot: evaluated.selected === null ? null : evaluated.selected.slot + 1,
    });
    return selectedBankedResetCandidate(evaluated, localCandidates, originalActive);
  };

  const fetchAttempt = async (
    accountEntry: CodexAuthAccountEntry,
    auth: CodexAuthState,
    routing: RoutingAccount,
    phase: CodexAttemptPhase,
    beforeTransport?: () => Promise<void>,
    activeAdmissionAlreadyFenced = false
  ): Promise<Response> => {
    attemptNumber += 1;
    transportState.started = false;
    // Assigned inside the actual onDispatch callback; a holder avoids control
    // flow narrowing `null` to `never` across the opaque transport call.
    const upstreamAttempt: { current: ReturnType<SentinelUpstreamRecorder["startAttempt"]> | null } = {
      current: null,
    };
    try {
      await providerDispatch.claim();
      const response = await fetchCodexResponseWithAuth(
        auth,
        url,
        serializedBody,
        baseHeaders,
        options.signal,
        async () => {
          await beforeTransport?.();
          if (!activeAdmissionAlreadyFenced) await ensureActiveRoutingAdmissionCurrent(routing);
        },
        () => {
          providerDispatch.markTransportStarted();
          transportState.started = true;
          reportCodexResponseTiming(options.timing?.onDispatch);
          upstreamAttempt.current = options.sentinelUpstreamRecorder?.startAttempt("chatgpt_codex") ?? null;
        }
      );
      // Wrap before the response WeakMap account/health registration
      // so the wrapper is the canonical identity every consumer observes.
      const recordedResponse = upstreamAttempt.current ? upstreamAttempt.current.wrap(response) : response;
      setCodexResponseAccountTelemetry(recordedResponse, routing.slot + 1, auth.account_id);
      setCodexResponseActiveTelemetry(recordedResponse, routing);
      reportCodexResponseTiming(options.timing?.onHeaders);
      void recordCodexResponseHealth(auth.account_id, recordedResponse, auth);
      logCodexRouting("codex_attempt", {
        request_id: options.requestId ?? null,
        attempt: attemptNumber,
        slot: routing.slot + 1,
        phase,
        status: recordedResponse.status,
        status_class: codexStatusClass(recordedResponse.status),
        active_generation: routing.activeGeneration ?? null,
        active_transition_reason: routing.activeTransitionReason ?? null,
      });
      return recordedResponse;
    } catch (error) {
      upstreamAttempt.current?.recordFetchError();
      const signalReason = options.signal?.reason;
      const clientCancelled = options.signal?.aborted === true && !(signalReason instanceof Error && signalReason.name === "TimeoutError");
      const siblingTransportFailure = isCodexSiblingTransportFailure(error);
      if (
        !(error instanceof CodexBankedResetRetryFenceError) &&
        !(error instanceof CodexActiveAccountFenceError) &&
        !(error instanceof ApiKeyQuotaDispatchError) &&
        !siblingTransportFailure &&
        !clientCancelled
      ) {
        void recordCodexThrownHealth(accountEntry.auth.account_id, error);
      }
      logCodexRouting("codex_attempt", {
        request_id: options.requestId ?? null,
        attempt: attemptNumber,
        slot: routing.slot + 1,
        phase,
        status: error instanceof CodexError ? error.status : null,
        status_class: error instanceof CodexBankedResetRetryFenceError ? "banked_reset_fenced" : codexErrorClass(error),
        active_generation: routing.activeGeneration ?? null,
        active_transition_reason: routing.activeTransitionReason ?? null,
      });
      throw error;
    }
  };

  const refreshAfter401 = async (
    routing: RoutingAccount,
    auth: CodexAuthState,
    trigger: "401" | "proactive"
  ): Promise<Readonly<{ auth: CodexAuthState; routing: RoutingAccount }>> => {
    refreshedSlots.add(routing.slot);
    try {
      const refreshed =
        trigger === "proactive"
          ? await awaitWithoutCancellingSharedWork(getValidAuth({ ...poolEntry, auth, routing }), options.signal)
          : await awaitWithoutCancellingSharedWork(refreshAuthCoordinated({ ...poolEntry, auth, routing }), options.signal);
      const reconciled = await reconcileCodexRoutingAccount(routing, refreshed);
      logCodexRouting("codex_token_refresh", {
        request_id: options.requestId ?? null,
        slot: routing.slot + 1,
        trigger,
        outcome: "succeeded",
        status_class: "2xx",
        active_generation: routing.activeGeneration ?? null,
        active_transition_reason: routing.activeTransitionReason ?? null,
      });
      return { auth: refreshed, routing: reconciled };
    } catch (error) {
      logCodexRouting("codex_token_refresh", {
        request_id: options.requestId ?? null,
        slot: routing.slot + 1,
        trigger,
        outcome: "failed",
        status_class: codexErrorClass(error),
        active_generation: routing.activeGeneration ?? null,
        active_transition_reason: routing.activeTransitionReason ?? null,
      });
      throw error;
    }
  };

  const classify429 = async (accountEntry: CodexAuthAccountEntry, routing: RoutingAccount, auth: CodexAuthState, response: Response): Promise<Response> => {
    const disposition = await markCodexQuotaBlocked(routing, response);
    // Ordinary exhaustion is a valid usage-limit classification with a known
    // deadline, relative or absolute. A stable absolute deadline remains
    // required only to mint a banked-reset candidate.
    const ordinaryQuotaExhaustion = disposition.usageLimitReached && disposition.retryAtMs !== null;
    let candidateOutcome: "captured" | "ineligible" | "routing_fence_unavailable" = "ineligible";
    if (ordinaryQuotaExhaustion) {
      candidateOutcome = await captureBankedResetCandidate(accountEntry, routing, auth, disposition);
    } else {
      // A generic 429 is not proof that the account is exhausted. Do not spend
      // a reset based on an older candidate in the same failover pass.
      bankedResetCandidates.clear();
    }
    logCodexRouting("codex_quota_classification", {
      request_id: options.requestId ?? null,
      slot: routing.slot + 1,
      usage_limit_reached: disposition.usageLimitReached ? "true" : "false",
      retry_at_ms: disposition.retryAtMs,
      quota_block_source: disposition.quotaBlockSource,
      reset_deadline_is_stable: disposition.resetDeadlineIsStable ? "true" : "false",
      reset_deadline_conflict: disposition.resetDeadlineConflict ? "true" : "false",
      candidate_outcome: candidateOutcome,
      active_generation: routing.activeGeneration ?? null,
      active_transition_reason: routing.activeTransitionReason ?? null,
    });
    const classifiedAtMs = Date.now();
    const retryAfterDelay = disposition.retryAtMs === null ? 0 : Math.max(0, disposition.retryAtMs - classifiedAtMs);
    const mayRetryWithinBound = !disposition.usageLimitReached || retryAfterDelay <= CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS;
    if (mayRetryWithinBound) {
      const candidate = {
        accountEntry,
        auth,
        routing: {
          ...routing,
          probeRequired: disposition.usageLimitReached && disposition.retryAtMs !== null,
          probeGeneration: null,
          probeToken: null,
          probeCircuit: disposition.usageLimitReached && disposition.retryAtMs !== null ? ("quota" as const) : null,
        },
        delayMs: Math.min(retryAfterDelay, CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS),
        readyAtMs: disposition.retryAtMs ?? classifiedAtMs,
        expiresAtMs: classifiedAtMs + CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS,
      };
      if (!retryState.candidate || candidate.delayMs < retryState.candidate.delayMs) {
        retryState.candidate = candidate;
      }
    }
    if (ordinaryQuotaExhaustion) {
      // Authoritative exhaustion is a global transition reason. Its former
      // short retry is preserved across the reselect only when the same active
      // identity and generation are re-admitted.
      requestActiveReselection({ preserveRetry: true, routing });
    }
    return disposition.response;
  };

  /**
   * Paid fallback requires a fresh strong full-current-cohort exhaustion proof.
   * Captured banked-reset candidates are telemetry and redemption input only;
   * they never authorize a paid tier.
   */
  const authorizePaidFallbackForCompleteQuotaEvidence = async (response: Response): Promise<Response> => {
    if (response.status === 429 && (await freshFullCohortExhaustedProof())) {
      codexRoutingErrors.set(response, CODEX_QUOTA_BLOCKED_ERROR_CODE);
    }
    return response;
  };

  const persistVerifiedCapacityReset = async (
    candidate: CodexBankedResetCandidate,
    record: NonNullable<Awaited<ReturnType<typeof reconcileCodexBankedReset>>>["record"]
  ): Promise<void> => {
    if (record?.state !== "verified" || record.verified_at_ms === null) return;
    const slot = candidate.routing.slot + 1;
    if (slot !== 1 && slot !== 2) return;
    try {
      const kv = bankedResetDependencies.kv ?? (await getKv());
      if (!kv) return;
      await recordProviderCapacityResetEvent(
        {
          v: 1,
          event_id: record.idempotency_key_hash,
          slot,
          observed_at_ms: record.verified_at_ms,
        },
        kv
      );
      // A verified reset changes the capacity picture; refresh the sample.
      triggerProviderCapacitySample();
    } catch {
      // Capacity telemetry is best effort and must never change inference.
    }
  };

  // Codex can report an expired bearer as a quota-shaped 403. Only classify
  // that status as auth when the locally decoded JWT is already expired.
  const responseIsCodexAuthFailure = (auth: CodexAuthState, response: Response): boolean =>
    response.status === 401 || (response.status === 403 && accessTokenExpired(auth));

  /**
   * Record the outcome of the one permitted post-reset inference attempt:
   * cancel the superseded normal response, quarantine credentials the retry
   * proved invalid, classify a quota answer, and keep the probe lease
   * consistent with the result. Order is observable and must not change.
   */
  const finalizePostResetRetry = async (retryCandidate: CodexBankedResetCandidate, retried: Response, normalResponse: Response | null): Promise<Response> => {
    if (normalResponse) cancelResponseBody(normalResponse);
    if (responseIsCodexAuthFailure(retryCandidate.auth, retried)) {
      await markCodexCredentialInvalid(retryCandidate.routing);
      authWarning ??= CODEX_AUTH_REAUTH_WARNING;
    }
    let response = retried;
    if (retried.status === 429) {
      // This is the one permitted post-reset inference attempt. Preserve a
      // replayable normal 429 and never feed it back into reset selection.
      const disposition = await markCodexRecoveryProbeQuotaBlocked(retryCandidate.routing, retried);
      response = disposition.response;
      inheritCodexResponseActiveTelemetry(response, retried);
      if (disposition.usageLimitReached && disposition.retryAtMs !== null) {
        await captureBankedResetCandidate(retryCandidate.accountEntry, retryCandidate.routing, retryCandidate.auth, disposition);
      } else {
        bankedResetCandidates.clear();
      }
    } else if (!retried.ok) {
      await releaseCodexRoutingProbe(retryCandidate.routing);
    }
    if (response.ok && retryCandidate.routing.probeGeneration !== null) {
      codexProbeByResponse.set(response, retryCandidate.routing);
    }
    return response;
  };

  const runPostResetRetry = async (evaluated: EvaluatedBlockedReset, normalResponse: Response | null): Promise<Response | null> => {
    const { candidate, reset, originalActive } = evaluated;
    const resetRecord = reset.record;
    if (!resetRecord || (reset.kind !== "verified" && !evaluated.staleVerified)) {
      return normalResponse;
    }
    await persistVerifiedCapacityReset(candidate, resetRecord);
    // Elect the verified account before any recovery-probe acquisition or
    // inference. The original active snapshot, the candidate's pool credential
    // and quota fence, and the full-cohort exhaustion proof must all still hold.
    const elected = await electCodexResetRecoveryAccount(candidate.routing, originalActive, candidate.quotaResetAtMs, evaluated.staleVerified);
    if (!elected) return normalResponse;
    const electedCandidate: CodexBankedResetCandidate = {
      ...candidate,
      routing: elected,
      accountEntry: { ...candidate.accountEntry, auth: elected.auth, routing: elected },
    };
    const reconciled = evaluated.staleVerified
      ? await reconcileCodexQuotaAfterStaleVerifiedReset(elected, {
          quotaResetAtMs: candidate.quotaResetAtMs,
          // The stale predicate proves the current circuit, not the ledger's
          // older generation.
          routingGeneration: candidate.routingGeneration,
          fences: [authPoolFence(electedCandidate), activeSelectionFence(electedCandidate)],
        })
      : await reconcileCodexQuotaAfterVerifiedReset(elected, {
          quotaResetAtMs: candidate.quotaResetAtMs,
          routingGeneration: resetRecord.routing_generation,
          fences: electedResetFences(electedCandidate, resetRecord.routing_generation),
        });
    if (!reconciled) return normalResponse;
    const refreshedCandidate = await refreshBankedResetCandidate(electedCandidate);
    if (!refreshedCandidate) return normalResponse;
    const retryCandidate = withResetRecoveryProbe(refreshedCandidate, reconciled);

    logBankedResetEvent("codex_reset_inference_retry", {
      request_id: options.requestId ?? null,
      account_id_hash: reset.accountIdHash,
      quota_generation: reset.quotaGeneration,
      idempotency_key_hash: reset.idempotencyKeyHash,
      routing_generation: resetRecord.routing_generation,
    });
    let retried: Response;
    try {
      retried = await fetchAttempt(retryCandidate.accountEntry, retryCandidate.auth, retryCandidate.routing, "post_banked_reset", () =>
        ensurePostResetRetryAuthCurrent(retryCandidate)
      );
    } catch (error) {
      if (error instanceof CodexBankedResetRetryFenceError || error instanceof CodexActiveAccountFenceError) return normalResponse;
      logBankedResetEvent("codex_reset_inference_retry_result", {
        request_id: options.requestId ?? null,
        account_id_hash: reset.accountIdHash,
        quota_generation: reset.quotaGeneration,
        idempotency_key_hash: reset.idempotencyKeyHash,
        routing_generation: resetRecord.routing_generation,
        status: error instanceof CodexError ? error.status : null,
      });
      reportCodexBankedResetMetric(bankedResetDependencies.telemetry, "codex_reset_post_retry_total", 1, {
        request_id: options.requestId ?? null,
        account_id_hash: reset.accountIdHash,
        quota_generation: reset.quotaGeneration,
        idempotency_key_hash: reset.idempotencyKeyHash,
        routing_generation: resetRecord.routing_generation,
        status: error instanceof CodexError ? error.status : null,
      });
      throw error;
    }
    retried = await finalizePostResetRetry(retryCandidate, retried, normalResponse);
    logBankedResetEvent("codex_reset_inference_retry_result", {
      request_id: options.requestId ?? null,
      account_id_hash: reset.accountIdHash,
      quota_generation: reset.quotaGeneration,
      idempotency_key_hash: reset.idempotencyKeyHash,
      routing_generation: resetRecord.routing_generation,
      status: retried.status,
    });
    reportCodexBankedResetMetric(bankedResetDependencies.telemetry, "codex_reset_post_retry_total", 1, {
      request_id: options.requestId ?? null,
      account_id_hash: reset.accountIdHash,
      quota_generation: reset.quotaGeneration,
      idempotency_key_hash: reset.idempotencyKeyHash,
      routing_generation: resetRecord.routing_generation,
      status: retried.status,
    });
    return decorateAuthWarning(await authorizePaidFallbackForCompleteQuotaEvidence(retried));
  };

  const redeemAndRetryOnce = async (normalResponse: Response): Promise<Response> => {
    // A half-open account could still be healthy, but another isolate owns
    // its recovery probe. Keep the normal quota response instead of spending
    // a reset that was inferred only from a sibling's 429.
    if (probeUnavailable) return normalResponse;
    const evaluated = await evaluateBlockedCohortBankedReset();
    if (!evaluated) return normalResponse;
    return (await runPostResetRetry(evaluated, normalResponse)) ?? normalResponse;
  };

  const recoverBlockedReset = async (): Promise<Response | null> => {
    const evaluated = await evaluateBlockedCohortBankedReset();
    return evaluated ? await runPostResetRetry(evaluated, null) : null;
  };

  /** An all-exhausted cohort still gets its one banked-reset chance before the quota block is reported. */
  const exhaustedQuotaBlockedCohort = async (retryAtMs: number | null): Promise<Response> => {
    const recovered = await recoverBlockedReset();
    if (recovered) return recovered;
    return await quotaBlockedOrRetryableResponse(retryAtMs);
  };

  /**
   * Probe-required routing must own the lease before it dispatches, and a lease
   * another isolate holds is reported rather than guessed at.
   */
  const claimAttemptRouting = async (routing: RoutingAccount): Promise<Readonly<{ kind: "claimed"; routing: RoutingAccount } | { kind: "unavailable" }>> => {
    if (!routing.probeRequired) return { kind: "claimed", routing };
    const claimed = await claimCodexRoutingProbe(poolEntry.pool, routing);
    if (!claimed) {
      probeUnavailable = true;
      if (routing.probeCircuit === "upstream_timeout") probeUnavailableCircuit = "upstream_timeout";
      return { kind: "unavailable" };
    }
    return { kind: "claimed", routing: claimed };
  };

  /**
   * Each slot may re-authenticate once per request; a second auth failure
   * quarantines the credential so an eligible sibling can serve the request.
   * Refreshed credentials are published on `attempt` as soon as the refresh
   * resolves, so any later failure is attributed to the routing it actually
   * used.
   */
  const recoverCodexAuthFailure = async (
    accountEntry: CodexDispatchAccountEntry,
    attempt: CodexCredentialAttempt,
    response: Response
  ): Promise<Readonly<{ kind: "recovered"; response: Response } | { kind: "reselect_active" }>> => {
    if (!responseIsCodexAuthFailure(attempt.auth, response)) return { kind: "recovered", response };
    if (refreshedSlots.has(attempt.routing.slot)) {
      await markCodexCredentialInvalid(attempt.routing);
      requestActiveReselection();
      authWarning ??= CODEX_AUTH_REAUTH_WARNING;
      return { kind: "recovered", response };
    }
    cancelResponseBody(response);
    try {
      const refreshed = await refreshAfter401(attempt.routing, attempt.auth, "401");
      attempt.auth = refreshed.auth;
      attempt.routing = refreshed.routing;
      const retried = await fetchAttempt(accountEntry, attempt.auth, attempt.routing, "post_refresh");
      if (responseIsCodexAuthFailure(attempt.auth, retried)) {
        await markCodexCredentialInvalid(attempt.routing);
        requestActiveReselection();
        authWarning ??= CODEX_AUTH_REAUTH_WARNING;
      }
      return { kind: "recovered", response: retried };
    } catch (error) {
      if (error instanceof CodexError && error.status === 401) {
        await markCodexCredentialInvalid(attempt.routing);
        requestActiveReselection();
        noteCodexAuthFailure(error);
        lastError = error;
        return { kind: "reselect_active" };
      }
      throw error;
    }
  };

  /**
   * Classify a completed attempt: a quota answer is recorded before the
   * telemetry fences, an auth or quota status keeps the response
   * for a possible banked reset, and a success is served.
   */
  const classifyAccountAttemptResponse = async (
    accountEntry: CodexDispatchAccountEntry,
    attempt: CodexCredentialAttempt,
    response: Response
  ): Promise<CodexAccountAttemptOutcome> => {
    let current = response;
    if (current.status === 429) {
      current = await classify429(accountEntry, attempt.routing, attempt.auth, current);
      setCodexResponseAccountTelemetry(current, attempt.routing.slot + 1, attempt.auth.account_id);
      inheritCodexResponseActiveTelemetry(current, response);
    } else if (!current.ok) {
      await releaseCodexRoutingProbe(attempt.routing);
    }
    if (current.ok && attempt.routing.probeGeneration !== null) codexProbeByResponse.set(current, attempt.routing);
    if (current.status === 401 || current.status === 403 || current.status === 429) {
      if (lastResponse) cancelResponseBody(lastResponse);
      lastResponse = current;
      return { kind: "next_account" };
    }
    if (lastResponse) cancelResponseBody(lastResponse);
    if (current.ok) await reportHealthyFallback();
    return { kind: "response", response: decorateAuthWarning(current) };
  };

  /**
   * A sibling transport failure is absorbed on another account only when
   * upstream work provably never started; otherwise the ambiguity is fatal.
   */
  const absorbSiblingTransportFailure = async (error: CodexError, routing: RoutingAccount): Promise<CodexAccountAttemptOutcome> => {
    if (transportState.started) {
      if (error.code !== "gateway_timeout") await releaseCodexRoutingProbe(routing);
      if (lastResponse) cancelResponseBody(lastResponse);
      throw error;
    }
    transportFailure = error;
    await releaseCodexRoutingProbe(routing);
    if (lastResponse) {
      cancelResponseBody(lastResponse);
      lastResponse = null;
    }
    retryState.candidate = null;
    bankedResetCandidates.clear();
    if (options.signal?.aborted) return { kind: "stop" };
    return { kind: "next_account" };
  };

  /**
   * A deterministic OAuth rejection is attributable to this credential even
   * when it happens before the first inference fetch. Quarantine it and let an
   * eligible sibling serve the request; transient refresh outages deliberately
   * stay on this path and never trigger a switch.
   */
  const handleAccountAttemptFailure = async (error: unknown, routing: RoutingAccount): Promise<CodexAccountAttemptOutcome> => {
    lastError = error;
    if (error instanceof CodexError && error.status === 401) {
      await markCodexCredentialInvalid(routing);
      requestActiveReselection();
      noteCodexAuthFailure(error);
      return { kind: "reselect_active" };
    }
    if (error instanceof CodexActiveAccountFenceError) {
      // The admission never reached transport, so its half-open lease is
      // released before the bounded loop re-elects the current active account.
      await releaseCodexRoutingProbe(routing);
      requestActiveReselection();
      return { kind: "reselect_active" };
    }
    if (isCodexSiblingTransportFailure(error)) return await absorbSiblingTransportFailure(error, routing);
    await releaseCodexRoutingProbe(routing);
    // Other request-local failures never open a shared provider gate.
    if (lastResponse) cancelResponseBody(lastResponse);
    throw error;
  };

  /** One account's initial attempt, including its single 401-driven re-authentication. */
  const attemptCodexAccount = async (accountEntry: CodexDispatchAccountEntry): Promise<CodexAccountAttemptOutcome> => {
    const claim = await claimAttemptRouting(accountEntry.routing);
    if (claim.kind === "unavailable") return { kind: "next_account" };
    const attempt: CodexCredentialAttempt = { auth: accountEntry.auth, routing: claim.routing };
    try {
      if (needsRefresh(attempt.auth)) {
        const refreshed = await refreshAfter401(attempt.routing, attempt.auth, "proactive");
        attempt.auth = refreshed.auth;
        attempt.routing = refreshed.routing;
      }
      const response = await fetchAttempt(accountEntry, attempt.auth, attempt.routing, "initial");
      const recovered = await recoverCodexAuthFailure(accountEntry, attempt, response);
      if (recovered.kind === "reselect_active") return { kind: "reselect_active" };
      return await classifyAccountAttemptResponse(accountEntry, attempt, recovered.response);
    } catch (error) {
      return await handleAccountAttemptFailure(error, attempt.routing);
    }
  };

  /** Dispatch the one admitted active account until it serves the request. */
  const dispatchEligibleAccounts = async (): Promise<Response | null> => {
    for (const accountEntry of accountEntries) {
      const outcome = await attemptCodexAccount(accountEntry);
      if (outcome.kind === "next_account") continue;
      if (outcome.kind === "stop") break;
      // An authoritative exhaustion or final credential failure asks the
      // bounded admission loop to re-elect the current global active account
      // instead of walking a stale sibling list.
      if (outcome.kind === "reselect_active") return null;
      return outcome.response;
    }
    return null;
  };

  /**
   * A recorded sibling transport failure is fatal. A competing upstream-timeout
   * probe keeps its 503 classification, re-read from routing when available.
   */
  const terminalTransportOutcome = async (): Promise<Response | null> => {
    if (transportFailure) {
      if (lastResponse) cancelResponseBody(lastResponse);
      throw transportFailure;
    }
    if (probeUnavailableCircuit !== "upstream_timeout") return null;
    if (lastResponse) cancelResponseBody(lastResponse);
    let retryAtMs: number | null = null;
    try {
      const currentPoolEntry = await getAuthPoolEntry(true, true);
      const currentSelection = await selectCodexRoutingAccountsStrong(currentPoolEntry.pool, currentPoolEntry.pool.accounts, Date.now(), requestedModel);
      if (currentSelection.kind === "upstream_blocked") retryAtMs = currentSelection.retryAtMs;
    } catch {
      // The failed claim already proves a competing timeout probe. Preserve its
      // 503 classification if a fresh routing read is unavailable.
    }
    return upstreamTimeoutCircuitResponse(retryAtMs);
  };

  /** An expired two-second retry window spends a banked reset instead, or reports the quota block. */
  const redeemOrReportQuotaBlocked = async (): Promise<Response> =>
    lastResponse ? await redeemAndRetryOnce(lastResponse) : await quotaBlockedOrRetryableResponse();

  /**
   * A two-second retry is only worthwhile while the account is still inside its
   * bounded quota window and its probe lease can be re-claimed.
   */
  const prepareTwoSecondRetry = async (
    candidate: CodexRetryCandidate
  ): Promise<Readonly<{ kind: "ready"; attempt: CodexCredentialAttempt; delayMs: number } | { kind: "respond"; response: Response }>> => {
    const retryCheckAtMs = Date.now();
    const retryDelayMs = Math.max(0, candidate.readyAtMs - retryCheckAtMs);
    if (retryCheckAtMs > candidate.expiresAtMs || retryCheckAtMs + retryDelayMs > candidate.expiresAtMs) {
      return { kind: "respond", response: await redeemOrReportQuotaBlocked() };
    }
    await waitForCodexRetry(retryDelayMs, options.signal, options.retrySleep ?? delay);
    if (Date.now() > candidate.expiresAtMs) return { kind: "respond", response: await redeemOrReportQuotaBlocked() };
    const attempt: CodexCredentialAttempt = { auth: candidate.auth, routing: candidate.routing };
    if (attempt.routing.probeRequired) {
      const claimed = await claimCodexRoutingProbe(poolEntry.pool, attempt.routing, Math.max(Date.now(), candidate.readyAtMs));
      if (!claimed) {
        return {
          kind: "respond",
          response: lastResponse ?? (await quotaBlockedOrRetryableResponse()),
        };
      }
      attempt.routing = claimed;
    }
    if (Date.now() > candidate.expiresAtMs) {
      await releaseCodexRoutingProbe(attempt.routing);
      return { kind: "respond", response: await redeemOrReportQuotaBlocked() };
    }
    return { kind: "ready", attempt, delayMs: retryDelayMs };
  };

  /**
   * The two-second retry gets the same single re-authentication as an initial
   * attempt, but a repeated auth failure is final for this request.
   */
  const recoverTwoSecondRetryAuthFailure = async (
    candidate: CodexRetryCandidate,
    attempt: CodexCredentialAttempt,
    response: Response
  ): Promise<Readonly<{ kind: "recovered"; response: Response } | { kind: "respond"; response: Response }>> => {
    if (!responseIsCodexAuthFailure(attempt.auth, response)) return { kind: "recovered", response };
    if (refreshedSlots.has(attempt.routing.slot)) {
      await markCodexCredentialInvalid(attempt.routing);
      authWarning ??= CODEX_AUTH_REAUTH_WARNING;
      return { kind: "recovered", response };
    }
    cancelResponseBody(response);
    let retried: Response;
    try {
      const refreshed = await refreshAfter401(attempt.routing, attempt.auth, "401");
      attempt.auth = refreshed.auth;
      attempt.routing = refreshed.routing;
      retried = await fetchAttempt(candidate.accountEntry, attempt.auth, attempt.routing, "post_retry_refresh");
    } catch (error) {
      if (error instanceof CodexError && error.status === 401) {
        await markCodexCredentialInvalid(attempt.routing);
        noteCodexAuthFailure(error);
        return { kind: "respond", response: authFailureResponse(error) };
      }
      if (!(error instanceof CodexError && error.code === "gateway_timeout")) {
        await releaseCodexRoutingProbe(attempt.routing);
      }
      throw error;
    }
    if (responseIsCodexAuthFailure(attempt.auth, retried)) {
      await markCodexCredentialInvalid(attempt.routing);
      authWarning ??= CODEX_AUTH_REAUTH_WARNING;
    }
    return { kind: "recovered", response: retried };
  };

  /**
   * Classify the single two-second retry answer: a qualifying quota answer may
   * capture a banked-reset candidate, a non-qualifying one clears candidates,
   * a failure releases the probe, and a success is served.
   */
  const classifyTwoSecondRetryResponse = async (candidate: CodexRetryCandidate, attempt: CodexCredentialAttempt, response: Response): Promise<Response> => {
    let current = response;
    if (current.status === 429) {
      const disposition = await markCodexQuotaBlocked(attempt.routing, current);
      current = disposition.response;
      if (disposition.usageLimitReached && disposition.retryAtMs !== null) {
        await captureBankedResetCandidate(candidate.accountEntry, attempt.routing, attempt.auth, disposition);
      } else {
        // The ordinary bounded retry gave a generic answer. It is not evidence
        // that a banked reset is safe to spend.
        bankedResetCandidates.clear();
      }
      setCodexResponseAccountTelemetry(current, attempt.routing.slot + 1, attempt.auth.account_id);
      inheritCodexResponseActiveTelemetry(current, response);
    } else if (!current.ok) {
      // A 401/403 says this retrying account cannot serve, but does not erase
      // a separately verified quota-exhaustion candidate from another slot.
      // Other non-successes remain conservative and discard that candidate.
      if (current.status !== 401 && current.status !== 403) bankedResetCandidates.clear();
      await releaseCodexRoutingProbe(attempt.routing);
    }
    if (current.ok && attempt.routing.probeGeneration !== null) codexProbeByResponse.set(current, attempt.routing);
    // A successful ordinary bounded retry has already served the original
    // inference request. A captured exhaustion observation from before that
    // retry is no longer a reason to spend a reset or issue another request.
    if (current.ok) return decorateAuthWarning(current);
    return decorateAuthWarning(await authorizePaidFallbackForCompleteQuotaEvidence(await redeemAndRetryOnce(current)));
  };

  /** The one permitted ordinary two-second retry for the captured candidate. */
  const runTwoSecondRetryCandidate = async (candidate: CodexRetryCandidate): Promise<Response> => {
    const ready = await prepareTwoSecondRetry(candidate);
    if (ready.kind === "respond") return ready.response;
    const { attempt } = ready;
    logCodexRouting("codex_two_second_retry", {
      request_id: options.requestId ?? null,
      slot: candidate.routing.slot + 1,
      delay_ms: ready.delayMs,
      active_generation: candidate.routing.activeGeneration ?? null,
      active_transition_reason: candidate.routing.activeTransitionReason ?? null,
    });
    if (lastResponse) cancelResponseBody(lastResponse);
    let response: Response;
    try {
      response = await fetchAttempt(candidate.accountEntry, attempt.auth, attempt.routing, "two_second_retry");
    } catch (error) {
      if (!(error instanceof CodexError && error.code === "gateway_timeout")) {
        await releaseCodexRoutingProbe(attempt.routing);
      }
      throw error;
    }
    const recovered = await recoverTwoSecondRetryAuthFailure(candidate, attempt, response);
    if (recovered.kind === "respond") return recovered.response;
    return await classifyTwoSecondRetryResponse(candidate, attempt, recovered.response);
  };

  /**
   * Nothing served this request: spend the last quota evidence, report a
   * recorded auth failure, or rethrow the original dispatch error.
   */
  const exhaustedCodexDispatchResponse = async (): Promise<Response> => {
    if (lastResponse) {
      return decorateAuthWarning(await authorizePaidFallbackForCompleteQuotaEvidence(await redeemAndRetryOnce(lastResponse)));
    }
    if (authFailureState.error) return authFailureResponse(authFailureState.error);
    if (lastError instanceof CodexError && lastError.status === 401) return authFailureResponse(lastError);
    if (probeUnavailable) {
      // A held recovery lease is retryable, not proof of quota exhaustion.
      return await quotaBlockedOrRetryableResponse();
    }
    throw dispatchFailureAsError(lastError, () => new CodexError("Codex auth pool is empty.", "codex_auth_missing", 503));
  };

  if (selected.kind === "quota_blocked") {
    return await exhaustedQuotaBlockedCohort(selected.retryAtMs);
  }

  /** A captured short bounded retry runs before any new dispatch or reset. */
  const runPendingShortRetry = async (): Promise<Response | null> => {
    const candidate = retryState.candidate;
    if (candidate === null) return null;
    retryState.candidate = null;
    try {
      return await runTwoSecondRetryCandidate(candidate);
    } catch (error) {
      if (!(error instanceof CodexActiveAccountFenceError)) throw error;
      // A concurrent transition superseded this request's admitted account;
      // re-admit the current global active instead of surfacing the fence.
      requestActiveReselection();
      return null;
    }
  };

  /** The active identity and generation the latest strong reselect observed. */
  const reselectedActiveIdentity = (): Readonly<{ accountIdHash: string | undefined; generation: number | undefined }> | null => {
    if (selected.kind === "eligible") {
      const active = selected.accounts[0];
      return { accountIdHash: active.accountIdHash, generation: active.activeGeneration };
    }
    if (selected.kind === "quota_blocked") {
      const active = selected.activeSnapshot.selection;
      return { accountIdHash: active?.account_id_hash, generation: active?.generation };
    }
    return null;
  };

  /**
   * Restore a preserved short retry only when the strong reselect re-admitted
   * the same opaque active identity and generation; any real switch discards it.
   */
  const restorePreservedShortRetry = (): boolean => {
    const preserved = preservedShortRetryState.current;
    preservedShortRetryState.current = null;
    if (preserved === null) return false;
    const currentActive = reselectedActiveIdentity();
    if (currentActive === null) return false;
    if (currentActive.accountIdHash !== preserved.accountIdHash || currentActive.generation !== preserved.activeGeneration) return false;
    // Same active opaque identity and generation: the unchanged account may
    // still spend its one bounded retry before any reset or redemption.
    retryState.candidate = preserved.candidate;
    return true;
  };

  /**
   * One strong reselection advance: reload the pool, rebuild the single-entry
   * admission, and return a terminal response or null to resume the loop.
   */
  const advanceActiveReselection = async (): Promise<Response | null> => {
    activeReselectionRequested = false;
    try {
      poolEntry = await getAuthPoolEntry(true, true);
      selected = await selectCodexRoutingAccountsStrong(poolEntry.pool, poolEntry.pool.accounts, Date.now(), requestedModel);
    } catch {
      return routingErrorResponse(503, "Codex routing state is temporarily unavailable; retry the request.", "codex_auth_missing");
    }
    if (selected.kind === "credentials_invalid" && authFailureState.error) {
      // Preserve the request-local actionable credential error (for example a
      // reused refresh token) instead of replacing it with the generic
      // all-credentials-invalid response.
      return authFailureResponse(authFailureState.error);
    }
    const reselectedTerminal = initialCodexSelectionResponse(selected);
    if (reselectedTerminal) return reselectedTerminal;
    if (restorePreservedShortRetry()) return null;
    if (selected.kind === "quota_blocked") {
      // A request that already holds the upstream quota answer keeps that
      // actionable body; a cold all-blocked cohort reports the routing error.
      if (lastResponse) return await exhaustedCodexDispatchResponse();
      return await exhaustedQuotaBlockedCohort(selected.retryAtMs);
    }
    if (selected.kind !== "eligible") return await exhaustedCodexDispatchResponse();
    accountEntries = selected.accounts.map((routing) => ({ ...poolEntry, auth: routing.auth, routing }));
    return null;
  };

  return await runCodexSerialAdmissionLoop({
    runPendingShortRetry,
    dispatchActive: dispatchEligibleAccounts,
    terminalTransportResponse: terminalTransportOutcome,
    hasQueuedRetry: () => retryState.candidate !== null,
    reselectionRequested: () => activeReselectionRequested,
    advanceReselection: advanceActiveReselection,
    exhaustedResponse: exhaustedCodexDispatchResponse,
  });
};

export const fetchCodexResponses = async (body: unknown, options: FetchCodexResponsesOptions = {}): Promise<Response> => {
  const prepared = await prepareCodexSubscriptionRequest(body, options.cacheScope ?? null);
  const providerDispatch = createCodexProviderDispatchCoordinator(options.beforeDispatch);
  try {
    const response = await fetchPreparedCodexResponses(prepared, options, providerDispatch);
    return withCodexWarnings(response, prepared.warnings);
  } finally {
    await providerDispatch.cancelBeforeTransport();
  }
};

/**
 * One models attempt for one account: fetch, and on an expired bearer refresh
 * the coordinated token before retrying the same URL exactly once.
 */
export {
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_AUTH_REAUTH_MESSAGE,
  CODEX_AUTH_REAUTH_WARNING,
  CODEX_MODELS_KV_KEY,
  CODEX_QUOTA_BLOCKED_ERROR_CODE,
  CODEX_UPSTREAM_DEGRADED_ERROR_CODE,
  CodexError,
  cacheCodexAuthPool,
  getCodexAccountEmail,
  getCodexResponseAccountCohortId,
  getCodexResponseActiveTelemetry,
  getCodexResponseSlot,
  getCodexRoutingError,
  getCodexRoutingProbe,
  getJwtExpMs,
  markCodexResponseCompleted,
  markCodexResponseUpstreamError,
  parseCodexAuthFromAuthJson,
  parseCodexAuthPool,
  releaseCodexResponseProbe,
  resetCodexAuthCacheForTest,
  upsertCodexAuthAccount,
} from "./codex_auth.ts";

export {
  CodexCacheScopeExperimentError,
  beginCodexCacheScopeExperiment,
  fetchCodexResponsesForCacheScopeExperiment,
  getCodexCapacityAccounts,
  refreshCodexCacheScopeExperimentSlot,
} from "./codex_experiment.ts";
export type { CodexCacheScopeExperimentSession, CodexCapacityAccount } from "./codex_experiment.ts";
export { orderCodexAuthAccounts } from "./codex_dispatch.ts";
export {
  buildCodexRequest,
  fetchCodexModels,
  loadCodexModelsSnapshot,
  loadFullCodexModelsSnapshot,
  preserveCodexDefaultModel,
  storeCodexModelsSnapshot,
  validateCodexAuthJson,
} from "./codex_models_fetch.ts";
export type { CodexModelsSnapshot } from "./codex_models.ts";
export { getCodexModelsSnapshotDefaultModel } from "./codex_models.ts";
