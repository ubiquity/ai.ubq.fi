// Codex account routing: probes, recovery, serial selection (remainder of the original module), split out of src/codex_account_routing.ts.

import { getKv } from "./kv.ts";
import { codexAccountEligibility, codexSubscriptionHash, loadProviderSelectionCached } from "./provider_selection.ts";
import { getString, isRecord } from "./utils.ts";
import { CodexAuthPoolState, CodexAuthState } from "./types.ts";
import {
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY,
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY,
  CODEX_HALF_OPEN_LEASE_MS,
  CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS,
  CodexAccountRoutingState,
  CodexActiveAccountSelection,
  CodexActiveAccountSnapshot,
  CodexActiveAccountTransitionReason,
  CodexBlockedRoutingAccount,
  CodexCapacityRoutingObservation,
  CodexProbeCircuit,
  CodexQuotaClass,
  CodexQuotaClassBlock,
  CodexRoutingSlot,
  RouteSelection,
  RoutingAccount,
  hasUnresolvedLegacyResetIdentity,
  isSafeMs,
  isVersionstamp,
  parseCodexAccountRoutingState,
  parseCodexActiveAccountSelection,
  setRoutingStateCache,
  getCachedRoutingState,
} from "./codex_routing_state.ts";
import {
  capacityHeadroomForObservation,
  capacityObservationIsFresh,
  loadCodexAccountRouting,
  loadCodexCapacityRoutingObservations,
  neutralSlot,
  normalizeRoutingState,
  openRoutingKv,
  parseStoredCapacityObservationStore,
  probeLeaseMatchesRoutingAccount,
  quotaBlockForClass,
  quotaBlockKeyForClass,
  quotaClass,
  quotaHeadroomFor,
  quotaSignalObservedAtForClass,
  recheckQuotaClasses,
  reconcileCapacityRoutingState,
  releaseQuotaClassProbe,
  rotateCredentialForSameAccount,
  routingAccountIdentity,
  routingProbeCircuit,
  slotFor,
  slotMatchesRoutingAccount,
  updateRoutingState,
  withLegacyQuotaClassMap,
  withSlot,
  withoutQuotaClass,
} from "./codex_capacity_routing.ts";
import { Codex429Classification, CodexKnownProbeLease, markCodexQuotaBlockedWithMode } from "./codex_429.ts";

export const markCodexRecoveryProbeQuotaBlocked = async (account: RoutingAccount, response: Response, now = Date.now()): Promise<Codex429Classification> =>
  await markCodexQuotaBlockedWithMode(account, response, now, true);

/**
 * Legacy transition retained for state-migration tests. Live inference no
 * longer calls it; normalized durable reads discard the resulting fence.
 */
export const markCodexUpstreamTimeout = async (account: RoutingAccount, now = Date.now()): Promise<void> => {
  const blockedUntil = now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS;
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (!slotMatchesRoutingAccount(current, account)) return null;
    if (account.probeGeneration === null && current.probe_lease !== null) return null;
    if (account.probeGeneration !== null && !probeLeaseMatchesRoutingAccount(current, account)) return null;
    const priorTimeout = current.upstream_timeout_blocked_until_ms ?? 0;
    return withSlot(state, account.slot, {
      ...current,
      account_id_hash: account.accountIdHash,
      upstream_timeout_blocked_until_ms: Math.max(priorTimeout, blockedUntil),
      generation: current.generation + 1,
      probe_lease: null,
    });
  });
};

export const markCodexCredentialInvalid = async (account: RoutingAccount): Promise<void> => {
  const now = Date.now();
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (!slotMatchesRoutingAccount(current, account)) return null;
    if (account.probeGeneration !== null && !probeLeaseMatchesRoutingAccount(current, account)) return null;
    return withSlot(state, account.slot, {
      ...current,
      account_id_hash: account.accountIdHash,
      quota_blocked_until_ms: null,
      quota_block_source: null,
      quota_blocked_classes: [],
      quota_blocks_by_class: {},
      upstream_timeout_blocked_until_ms: null,
      invalid_credential_version: account.credentialVersion,
      quota_signal_observed_at_ms: now,
      probe_lease: null,
    });
  });
};

/**
 * OAuth refresh can replace a token while an inference attempt is in flight.
 * Treat the token as a new credential version while retaining the account's
 * stable reset observation, so a refresh cannot manufacture a second key.
 */
export const reconcileCodexRoutingAccount = async (account: RoutingAccount, auth: CodexAuthState): Promise<RoutingAccount> => {
  const { accountIdHash, credentialVersion } = await routingAccountIdentity(auth);
  // The normal refresh check can return unchanged credentials. Preserve any
  // half-open probe fence so a successful response can clear the quota
  // circuit claimed for this request.
  if (credentialVersion === account.credentialVersion) return { ...account, auth, accountIdHash };

  const reconciled: RoutingAccount = {
    ...account,
    auth,
    accountIdHash,
    credentialVersion,
    probeRequired: false,
    probeGeneration: null,
    probeToken: null,
    probeCircuit: null,
  };

  // Credential rotation is exceptional. For the same account it releases
  // ordinary routing but retains a stable reset identity as lookup-only; only
  // a genuinely different account starts a neutral reset scope.
  const nextState = await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (current.credential_version === credentialVersion) return null;
    if (!slotMatchesRoutingAccount(current, account)) return null;
    return withSlot(
      state,
      account.slot,
      auth.account_id === account.auth.account_id
        ? rotateCredentialForSameAccount(current, { accountIdHash, credentialVersion })
        : neutralSlot(credentialVersion, accountIdHash)
    );
  });
  const nextSlot = nextState?.slots.at(account.slot);
  const nextProbeLease = nextSlot?.probe_lease ?? null;
  const retainedProbe =
    auth.account_id === account.auth.account_id &&
    account.probeGeneration !== null &&
    account.probeToken !== null &&
    nextSlot?.credential_version === credentialVersion &&
    nextProbeLease !== null &&
    nextProbeLease.token === account.probeToken &&
    nextProbeLease.circuit === routingProbeCircuit(account) &&
    nextProbeLease.generation === nextSlot.generation;
  return retainedProbe
    ? {
        ...reconciled,
        probeGeneration: nextSlot.generation,
        probeToken: nextProbeLease.token,
        probeCircuit: nextProbeLease.circuit,
      }
    : reconciled;
};

export const releaseCodexRoutingProbe = async (account: RoutingAccount): Promise<void> => {
  if (account.probeGeneration === null || !account.probeToken) return;
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (!slotMatchesRoutingAccount(current, account) || !probeLeaseMatchesRoutingAccount(current, account)) return null;
    const released = routingProbeCircuit(account) === "quota" ? releaseQuotaClassProbe(current, quotaClass(account.requestedModel)) : current;
    return withSlot(state, account.slot, {
      ...released,
      account_id_hash: account.accountIdHash,
      upstream_timeout_blocked_until_ms: routingProbeCircuit(account) === "upstream_timeout" ? null : current.upstream_timeout_blocked_until_ms,
      probe_lease: null,
    });
  });
};

/** A successful recovery probe is the only trusted way to clear reset ambiguity. */
export const markCodexSuccess = async (account: RoutingAccount): Promise<void> => {
  if (account.probeGeneration === null || !account.probeToken) return;
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (!slotMatchesRoutingAccount(current, account) || !probeLeaseMatchesRoutingAccount(current, account)) return null;
    const classAwareCurrent = withLegacyQuotaClassMap(current);
    const released =
      routingProbeCircuit(account) === "quota"
        ? withoutQuotaClass(
            classAwareCurrent,
            quotaBlockKeyForClass(classAwareCurrent, quotaClass(account.requestedModel)) ?? quotaClass(account.requestedModel)
          )
        : current;
    const hasOtherQuotaClasses = Object.keys(released.quota_blocks_by_class ?? {}).length > 0;
    return withSlot(state, account.slot, {
      ...released,
      account_id_hash: account.accountIdHash,
      upstream_timeout_blocked_until_ms: routingProbeCircuit(account) === "upstream_timeout" ? null : current.upstream_timeout_blocked_until_ms,
      observed_reset_at_ms: hasOtherQuotaClasses ? released.observed_reset_at_ms : null,
      observed_reset_at_is_stable: hasOtherQuotaClasses ? released.observed_reset_at_is_stable : false,
      banked_reset_generation_ambiguous: hasOtherQuotaClasses ? released.banked_reset_generation_ambiguous : false,
      banked_reset_recovery_probe_pending: hasOtherQuotaClasses ? released.banked_reset_recovery_probe_pending : false,
      probe_lease: null,
    });
  });
};

/**
 * Pure exact-fence predicate used by both strong-read helpers and the banked
 * reset's atomic KV checks. Equality (rather than `>=`) prevents a shorter
 * later Retry-After from authorizing a reset against a longer existing block;
 * an unresolved legacy account association denies every new claim.
 */
export const isCodexQuotaBlockFenceCurrent = (value: unknown, account: RoutingAccount, quotaResetAtMs: number, routingGeneration: number): boolean => {
  if (!isSafeMs(quotaResetAtMs) || !Number.isSafeInteger(routingGeneration) || routingGeneration < 0) return false;
  const state = parseCodexAccountRoutingState(value);
  const current = state?.slots[account.slot];
  const classBlock = current ? quotaBlockForClass(current, quotaClass(account.requestedModel)) : null;
  return (
    !hasUnresolvedLegacyResetIdentity(state) &&
    current?.credential_version === account.credentialVersion &&
    current.account_id_hash === account.accountIdHash &&
    current.generation === routingGeneration &&
    classBlock !== null &&
    classBlock.observed_reset_at_ms === quotaResetAtMs &&
    classBlock.observed_reset_at_is_stable &&
    !classBlock.banked_reset_generation_ambiguous &&
    classBlock.blocked_until_ms === quotaResetAtMs
  );
};

/**
 * Returns a durable routing fence for a fully persisted quota observation.
 * Account routing otherwise fails open when KV is down; banked redemption is
 * stricter and must refuse to start without this strong-read proof.
 */
export const getCodexQuotaBlockFence = async (account: RoutingAccount, quotaResetAtMs: number): Promise<number | null> => {
  if (!isSafeMs(quotaResetAtMs)) return null;
  let kv: Deno.Kv | null;
  try {
    kv = await getKv();
  } catch {
    return null;
  }
  if (!kv) return null;
  try {
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
    const state = parseCodexAccountRoutingState(entry.value);
    const generation = state?.slots[account.slot]?.generation;
    return generation !== undefined && isCodexQuotaBlockFenceCurrent(entry.value, account, quotaResetAtMs, generation) ? generation : null;
  } catch {
    return null;
  }
};

/**
 * An additional immutable record that must remain current while a verified
 * reset clears its routing circuit. The gateway uses this for the auth-pool
 * slot, so a credential rotation cannot turn a verified reset into a retry
 * with a removed credential.
 */
export type CodexQuotaResetReconciliationFence = Readonly<{
  key: Deno.KvKey;
  isCurrent: (value: unknown) => boolean;
}>;

const readCurrentReconciliationFences = async (
  kv: Deno.Kv,
  fences: readonly CodexQuotaResetReconciliationFence[]
): Promise<readonly Deno.KvEntryMaybe<unknown>[] | null> => {
  if (!Array.isArray(fences) || !fences.every((fence) => isRecord(fence) && Array.isArray(fence.key) && typeof fence.isCurrent === "function")) return null;
  const entries: Deno.KvEntryMaybe<unknown>[] = [];
  for (const fence of fences) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get(fence.key, { consistency: "strong" });
      if (!fence.isCurrent(entry.value)) return null;
    } catch {
      return null;
    }
    entries.push(entry);
  }
  return entries;
};

const withReconciliationFences = (operation: Deno.AtomicOperation, entries: readonly Deno.KvEntryMaybe<unknown>[]): Deno.AtomicOperation => {
  let next = operation;
  for (const entry of entries) next = next.check(entry);
  return next;
};

type CodexQuotaProbeEligibility = (state: CodexAccountRoutingState, current: CodexRoutingSlot, nowMs: number) => boolean;

/** Apply the fenced half-open claim a verified reset just persisted. */
const applyRecoveryProbeLease = (account: RoutingAccount, next: CodexAccountRoutingState, lease: CodexKnownProbeLease): RoutingAccount | null => {
  const claimedSlot = next.slots.at(account.slot);
  if (claimedSlot === undefined) return null;
  return {
    ...account,
    quotaHeadroom: quotaHeadroomFor(claimedSlot),
    probeRequired: false,
    probeGeneration: lease.generation,
    probeToken: lease.token,
    probeCircuit: lease.circuit,
  };
};

/**
 * One strong-read compare-and-set attempt for a verified-reset recovery probe.
 * Returns "retry" when a concurrent writer won the CAS, and null when the
 * attempt must fail closed.
 */
const attemptCodexQuotaRecoveryProbe = async (
  kv: Deno.Kv,
  account: RoutingAccount,
  input: Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
    fences?: readonly CodexQuotaResetReconciliationFence[];
  }>,
  isEligible: CodexQuotaProbeEligibility
): Promise<RoutingAccount | "retry" | null> => {
  try {
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
    const state = parseCodexAccountRoutingState(entry.value);
    const current = state?.slots.at(account.slot);
    const nowMs = Date.now();
    if (!state || current === undefined || !isEligible(state, current, nowMs)) return null;
    const fenceEntries = input.fences === undefined ? [] : await readCurrentReconciliationFences(kv, input.fences);
    if (!fenceEntries) return null;
    const nextGeneration = current.generation + 1;
    const probeExpiresAtMs = nowMs + CODEX_HALF_OPEN_LEASE_MS;
    if (!Number.isSafeInteger(nextGeneration) || !isSafeMs(nowMs) || !isSafeMs(probeExpiresAtMs)) return null;
    const requestedQuotaClass = quotaClass(account.requestedModel);
    const recoveryLease: CodexKnownProbeLease = {
      token: crypto.randomUUID(),
      expires_at_ms: probeExpiresAtMs,
      generation: nextGeneration,
      circuit: "quota",
      quota_class: requestedQuotaClass,
    };
    const classAwareCurrent = withLegacyQuotaClassMap(current);
    const releasedClassBlock = quotaBlockForClass(classAwareCurrent, requestedQuotaClass);
    const released = withoutQuotaClass(classAwareCurrent, quotaBlockKeyForClass(classAwareCurrent, requestedQuotaClass) ?? requestedQuotaClass);
    const next = withSlot(state, account.slot, {
      ...released,
      // The verified reset makes normal routing eligible, but an absolute
      // Retry-After cannot prove whether a delayed response names this old
      // window or a new one. Keep the observation lookup-only until a
      // recovery probe independently proves the circuit healthy.
      observed_reset_at_ms: releasedClassBlock?.observed_reset_at_ms ?? current.observed_reset_at_ms,
      observed_reset_at_is_stable: releasedClassBlock?.observed_reset_at_is_stable ?? current.observed_reset_at_is_stable,
      banked_reset_generation_ambiguous: true,
      banked_reset_recovery_probe_pending: true,
      generation: nextGeneration,
      probe_lease: recoveryLease,
    });
    const committed = await withReconciliationFences(kv.atomic().check(entry), fenceEntries).set(CODEX_ACCOUNT_ROUTING_KV_KEY, next).commit();
    if (!committed.ok) return "retry";
    setRoutingStateCache(next, committed.versionstamp);
    return applyRecoveryProbeLease(account, next, recoveryLease);
  } catch {
    return null;
  }
};

/**
 * Claim a fenced recovery probe after a verified reset. The eligibility
 * predicate is kept separate because stale ledger records need a different
 * proof than the current-generation fence, while the CAS/lease transition
 * must remain identical.
 */
const reconcileCodexQuotaAfterProbe = async (
  account: RoutingAccount,
  input: Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
    fences?: readonly CodexQuotaResetReconciliationFence[];
  }>,
  isEligible: CodexQuotaProbeEligibility
): Promise<RoutingAccount | null> => {
  if (!isSafeMs(input.quotaResetAtMs) || !Number.isSafeInteger(input.routingGeneration) || input.routingGeneration < 0) return null;
  const kv = await openRoutingKv();
  if (!kv) return null;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const claimed = await attemptCodexQuotaRecoveryProbe(kv, account, input, isEligible);
    if (claimed === null) return null;
    if (claimed !== "retry") return claimed;
  }
  return null;
};

/**
 * A verified reset may release only the exact circuit observation that caused
 * it. Credential and generation checks prevent an old provider transaction
 * from admitting a newly rotated credential or a later quota window.
 */
export const reconcileCodexQuotaAfterVerifiedReset = async (
  account: RoutingAccount,
  input: Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
    fences?: readonly CodexQuotaResetReconciliationFence[];
  }>
): Promise<RoutingAccount | null> =>
  await reconcileCodexQuotaAfterProbe(account, input, (state) => isCodexQuotaBlockFenceCurrent(state, account, input.quotaResetAtMs, input.routingGeneration));

/**
 * Recover an account whose verified reset record predates later routing
 * transitions. The ledger proves the reset was already spent, so this path
 * only fences a new inference probe; it never submits another reset.
 */
export const reconcileCodexQuotaAfterStaleVerifiedReset = async (
  account: RoutingAccount,
  input: Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
    fences?: readonly CodexQuotaResetReconciliationFence[];
  }>
): Promise<RoutingAccount | null> => {
  return await reconcileCodexQuotaAfterProbe(
    account,
    input,
    (state, current, nowMs) =>
      !state.banked_reset_legacy_identity_unresolved &&
      slotMatchesRoutingAccount(current, account) &&
      current.account_id_hash === account.accountIdHash &&
      current.credential_version === account.credentialVersion &&
      current.generation === input.routingGeneration &&
      current.invalid_credential_version !== account.credentialVersion &&
      (() => {
        const requestedClass = quotaClass(account.requestedModel);
        const classBlock = quotaBlockForClass(current, requestedClass);
        const legacyDeadline = requestedClass === "unknown" ? (current.quota_blocked_until_ms ?? 0) : 0;
        return (
          classBlock !== null &&
          Math.max(classBlock.blocked_until_ms, legacyDeadline) >= input.quotaResetAtMs &&
          classBlock.observed_reset_at_ms === input.quotaResetAtMs &&
          classBlock.observed_reset_at_is_stable
        );
      })() &&
      (current.probe_lease?.expires_at_ms ?? 0) <= nowMs
  );
};

/** Adopt a lease this isolate just minted as the account's half-open claim. */
const applyClaimedProbeLease = (account: RoutingAccount, lease: CodexKnownProbeLease): RoutingAccount => ({
  ...account,
  probeRequired: false,
  probeGeneration: lease.generation,
  probeToken: lease.token,
  probeCircuit: lease.circuit,
});

const buildExpiredProbeClaim = (
  base: CodexAccountRoutingState,
  account: RoutingAccount,
  now: number
): Readonly<{ next: CodexAccountRoutingState; lease: CodexKnownProbeLease }> | null => {
  const current = slotFor(base, account);
  const circuit = routingProbeCircuit(account);
  const requestedQuotaClass = quotaClass(account.requestedModel);
  const circuitDeadline =
    circuit === "upstream_timeout" ? current.upstream_timeout_blocked_until_ms : quotaBlockForClass(current, requestedQuotaClass)?.blocked_until_ms;
  if (
    !slotMatchesRoutingAccount(current, account) ||
    current.invalid_credential_version === account.credentialVersion ||
    !circuitDeadline ||
    circuitDeadline > now ||
    (current.probe_lease?.expires_at_ms ?? 0) > now
  )
    return null;
  const lease: CodexKnownProbeLease = {
    token: crypto.randomUUID(),
    expires_at_ms: now + CODEX_HALF_OPEN_LEASE_MS,
    generation: current.generation,
    circuit,
    quota_class: circuit === "quota" ? requestedQuotaClass : null,
  };
  return {
    next: withSlot(base, account.slot, {
      ...current,
      account_id_hash: account.accountIdHash,
      probe_lease: lease,
    }),
    lease,
  };
};

/**
 * Claim locally. This retains availability when KV is unavailable, but not
 * cross-isolate coordination.
 */
const applyLocalExpiredProbeClaim = (state: CodexAccountRoutingState, account: RoutingAccount, now: number): RoutingAccount | null => {
  const claimed = buildExpiredProbeClaim(state, account, now);
  if (!claimed) return null;
  setRoutingStateCache(claimed.next);
  return applyClaimedProbeLease(account, claimed.lease);
};

const claimExpiredProbeFromKv = async (kv: Deno.Kv, account: RoutingAccount, state: CodexAccountRoutingState, now: number): Promise<RoutingAccount | null> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
    const durable = parseCodexAccountRoutingState(entry.value);
    const claimed = buildExpiredProbeClaim(durable ?? state, account, now);
    if (!claimed) return null;
    const commit = await kv.atomic().check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, claimed.next).commit();
    if (!commit.ok) continue;
    setRoutingStateCache(claimed.next, commit.versionstamp);
    return applyClaimedProbeLease(account, claimed.lease);
  }
  return null;
};

const claimExpiredProbe = async (state: CodexAccountRoutingState, account: RoutingAccount, now: number): Promise<RoutingAccount | null> => {
  try {
    const kv = await openRoutingKv();
    if (kv === null) return applyLocalExpiredProbeClaim(state, account, now);
    return await claimExpiredProbeFromKv(kv, account, state, now);
  } catch {
    // Fail open if KV itself is unavailable. This retains availability but not cross-isolate coordination.
    return applyLocalExpiredProbeClaim(state, account, now);
  }
};

export const claimCodexRoutingProbe = async (pool: CodexAuthPoolState, account: RoutingAccount, now = Date.now()): Promise<RoutingAccount | null> => {
  if (!account.probeRequired) return account;
  const state = await loadCodexAccountRouting(pool);
  return await claimExpiredProbe(state, account, now);
};

/** A slot identity resolved from the pool before its durable routing state is read. */
type CodexRoutingSlotIdentity = Readonly<{
  slot: number;
  accountIdHash: string;
  credentialVersion: string;
}>;

/** A selectable account whose durable routing generation is already resolved. */
type CodexRoutedAccount = RoutingAccount & Readonly<{ routingGeneration: number }>;

type CodexRoutingAccountEvaluation = Readonly<{
  /** The resolved slot identity even when the account cannot serve now. */
  account: CodexRoutedAccount;
  /** Set when the account can take traffic now. */
  routedAccount: RoutingAccount | null;
  /** The 1-based slot number of a skipped account. */
  skippedSlot: number | null;
  /** The circuit that recorded the skip, or null for a credential fence. */
  blockedCircuit: CodexProbeCircuit | null;
  retryAtMs: number | null;
  blockedAccount: CodexBlockedRoutingAccount | null;
  /** Only these observed states may advance the global active selection. */
  activeTransitionReason: Exclude<CodexActiveAccountTransitionReason, "account_removed_or_replaced"> | null;
  /** A fresh, account-bound capacity sample proved the requested class empty. */
  capacityExhausted: boolean;
}>;

const skippedRoutingAccount = (
  account: CodexRoutedAccount,
  slotNumber: number,
  blockedCircuit: CodexProbeCircuit | null,
  retryAtMs: number | null = null,
  blockedAccount: CodexBlockedRoutingAccount | null = null,
  activeTransitionReason: Exclude<CodexActiveAccountTransitionReason, "account_removed_or_replaced"> | null = null
): CodexRoutingAccountEvaluation => ({
  account,
  routedAccount: null,
  skippedSlot: slotNumber,
  blockedCircuit,
  retryAtMs,
  blockedAccount,
  activeTransitionReason,
  capacityExhausted: false,
});

const routedRoutingAccount = (routedAccount: CodexRoutedAccount): CodexRoutingAccountEvaluation => ({
  account: routedAccount,
  routedAccount,
  skippedSlot: null,
  blockedCircuit: null,
  retryAtMs: null,
  blockedAccount: null,
  activeTransitionReason: null,
  capacityExhausted: false,
});

/** A stale capacity record is not an observation; only a fresh one may override a circuit. */
const freshCapacityObservation = (observation: CodexCapacityRoutingObservation | undefined, now: number): CodexCapacityRoutingObservation | null =>
  observation !== undefined && capacityObservationIsFresh(observation, now) ? observation : null;

/**
 * The last in-memory guard after the durable reconciliation CAS. It prevents a
 * stale local circuit from suppressing a fresh, positive account observation
 * that lost a concurrent write race.
 */
const resolveCodexCapacityDecision = (
  slot: CodexRoutingSlot,
  classAwareSlot: CodexRoutingSlot,
  requestedQuotaClass: CodexQuotaClass,
  observation: CodexCapacityRoutingObservation | undefined,
  model: string | null,
  now: number
): Readonly<{ quotaHeadroom: number | null; capacityOverride: boolean; capacityExhausted: boolean }> => {
  const freshObservation = freshCapacityObservation(observation, now);
  const observedCapacityHeadroom = freshObservation === null ? null : capacityHeadroomForObservation(freshObservation, model);
  const classQuotaSignalObservedAtMs = quotaSignalObservedAtForClass(classAwareSlot, requestedQuotaClass);
  const snapshotAtMs = freshObservation?.snapshot_at_ms;
  const quotaSignalNewer = snapshotAtMs !== undefined && classQuotaSignalObservedAtMs !== null && classQuotaSignalObservedAtMs >= snapshotAtMs;
  const capacityOverride = freshObservation !== null && observedCapacityHeadroom !== null && observedCapacityHeadroom > 0 && !quotaSignalNewer;
  const capacityExhausted = freshObservation !== null && observedCapacityHeadroom === 0 && !quotaSignalNewer;
  return {
    quotaHeadroom: freshObservation === null ? quotaHeadroomFor(slot) : observedCapacityHeadroom,
    capacityOverride,
    capacityExhausted,
  };
};

/** A running class circuit skips the account and keeps its reset identity available to banked redemption. */
const quotaBlockedSkipFor = (
  requestedClassBlock: CodexQuotaClassBlock | null,
  routedAccount: CodexRoutedAccount,
  now: number,
  capacityOverride: boolean
): Readonly<{ retryAtMs: number; blockedAccount: CodexBlockedRoutingAccount | null }> | null => {
  if (requestedClassBlock === null || requestedClassBlock.blocked_until_ms <= now || capacityOverride) return null;
  const blockedAccount =
    requestedClassBlock.observed_reset_at_ms !== null && requestedClassBlock.observed_reset_at_is_stable
      ? { ...routedAccount, quotaResetAtMs: requestedClassBlock.observed_reset_at_ms }
      : null;
  return { retryAtMs: requestedClassBlock.blocked_until_ms, blockedAccount };
};

/**
 * A verified banked reset releases the quota deadline but retains its
 * recovery-probe lease. Ordinary routing stays unavailable until that exact
 * fenced probe succeeds, fails, or expires.
 */
const probeLeaseSkipFor = (
  slot: CodexRoutingSlot,
  requestedQuotaClass: CodexQuotaClass,
  now: number
): Readonly<{ circuit: CodexProbeCircuit; retryAtMs: number }> | null => {
  const lease = slot.probe_lease;
  if (lease === null || lease.expires_at_ms <= now) return null;
  const leaseBlocksRequest =
    lease.circuit !== "upstream_timeout" && (lease.quota_class === null || lease.quota_class === undefined || lease.quota_class === requestedQuotaClass);
  return leaseBlocksRequest ? { circuit: lease.circuit, retryAtMs: lease.expires_at_ms } : null;
};

const evaluateCodexRoutingAccount = (
  state: CodexAccountRoutingState,
  auth: CodexAuthState,
  mapped: CodexRoutingSlotIdentity,
  model: string | null,
  observationsByAccount: ReadonlyMap<string, CodexCapacityRoutingObservation>,
  now: number
): CodexRoutingAccountEvaluation => {
  const account: RoutingAccount = {
    auth,
    slot: mapped.slot,
    accountIdHash: mapped.accountIdHash,
    credentialVersion: mapped.credentialVersion,
    quotaHeadroom: null,
    probeRequired: false,
    probeGeneration: null,
    probeToken: null,
    probeCircuit: null,
    requestedModel: model,
  };
  const storedSlot = slotFor(state, account);
  const slot = slotMatchesRoutingAccount(storedSlot, account) ? storedSlot : neutralSlot(account.credentialVersion, account.accountIdHash);
  const requestedQuotaClass = quotaClass(model);
  const classAwareSlot = withLegacyQuotaClassMap(slot);
  const requestedClassBlock = quotaBlockForClass(classAwareSlot, requestedQuotaClass);
  const capacity = resolveCodexCapacityDecision(slot, classAwareSlot, requestedQuotaClass, observationsByAccount.get(account.accountIdHash), model, now);
  const routedAccount: CodexRoutedAccount = { ...account, quotaHeadroom: capacity.quotaHeadroom, routingGeneration: slot.generation };
  if (slot.invalid_credential_version === account.credentialVersion) {
    return skippedRoutingAccount(routedAccount, mapped.slot + 1, null, null, null, "credential_invalid");
  }
  const quotaSkip = quotaBlockedSkipFor(requestedClassBlock, routedAccount, now, capacity.capacityOverride);
  if (quotaSkip !== null) {
    return skippedRoutingAccount(routedAccount, mapped.slot + 1, "quota", quotaSkip.retryAtMs, quotaSkip.blockedAccount, "quota_exhausted");
  }
  if (capacity.capacityExhausted) {
    return { ...skippedRoutingAccount(routedAccount, mapped.slot + 1, "quota", null, null, "quota_exhausted"), capacityExhausted: true };
  }
  const leaseSkip = probeLeaseSkipFor(slot, requestedQuotaClass, now);
  if (leaseSkip !== null) return skippedRoutingAccount(routedAccount, mapped.slot + 1, leaseSkip.circuit, leaseSkip.retryAtMs, null);
  // Claim the half-open lease only if request execution actually reaches this
  // slot. This preserves first/second order without abandoning a secondary
  // lease when the healthy first account returns directly.
  if (requestedClassBlock !== null) {
    return routedRoutingAccount({ ...routedAccount, probeRequired: !capacity.capacityOverride, probeCircuit: capacity.capacityOverride ? null : "quota" });
  }
  return routedRoutingAccount(routedAccount);
};

type CodexRoutingAccumulation = Readonly<{
  available: RoutingAccount[];
  blockedAccounts: CodexBlockedRoutingAccount[];
  skipped: number[];
  retryAt: number | null;
  hasQuotaBlock: boolean;
  hasUpstreamTimeoutBlock: boolean;
}>;

type CodexRoutingAccumulator = {
  available: RoutingAccount[];
  blockedAccounts: CodexBlockedRoutingAccount[];
  skipped: number[];
  retryAt: number | null;
  hasQuotaBlock: boolean;
  hasUpstreamTimeoutBlock: boolean;
};

/** Fold one evaluated account into the running selection accumulator. */
const foldCodexRoutingEvaluation = (accumulator: CodexRoutingAccumulator, evaluated: CodexRoutingAccountEvaluation): void => {
  if (evaluated.routedAccount !== null) {
    accumulator.available.push(evaluated.routedAccount);
    return;
  }
  if (evaluated.skippedSlot !== null) accumulator.skipped.push(evaluated.skippedSlot);
  if (evaluated.blockedCircuit === "upstream_timeout") accumulator.hasUpstreamTimeoutBlock = true;
  if (evaluated.blockedCircuit === "quota") accumulator.hasQuotaBlock = true;
  if (evaluated.retryAtMs !== null) {
    accumulator.retryAt = accumulator.retryAt === null ? evaluated.retryAtMs : Math.min(accumulator.retryAt, evaluated.retryAtMs);
  }
  if (evaluated.blockedAccount !== null) accumulator.blockedAccounts.push(evaluated.blockedAccount);
};

/** Accumulate every ordered account's routing outcome, preserving selection order. */
const accumulateCodexRoutingAccounts = (
  state: CodexAccountRoutingState,
  byId: ReadonlyMap<string, CodexRoutingSlotIdentity>,
  orderedAccounts: readonly CodexAuthState[],
  model: string | null,
  observationsByAccount: ReadonlyMap<string, CodexCapacityRoutingObservation>,
  now: number
): CodexRoutingAccumulation => {
  const accumulator: CodexRoutingAccumulator = {
    available: [],
    blockedAccounts: [],
    skipped: [],
    retryAt: null,
    hasQuotaBlock: false,
    hasUpstreamTimeoutBlock: false,
  };
  for (const auth of orderedAccounts) {
    const mapped = byId.get(auth.account_id);
    if (!mapped) continue;
    foldCodexRoutingEvaluation(accumulator, evaluateCodexRoutingAccount(state, auth, mapped, model, observationsByAccount, now));
  }
  return { ...accumulator };
};

const classifyCodexRouteSelection = (
  accumulated: CodexRoutingAccumulation,
  fullCohortExhausted = false,
  activeSnapshot: CodexActiveAccountSnapshot = { selection: null },
  poolSnapshotJson: string | null = null,
  capacitySnapshotJson: string | null = null
): RouteSelection => {
  const { available, blockedAccounts, skipped, retryAt, hasQuotaBlock, hasUpstreamTimeoutBlock } = accumulated;
  if (available.length) return { kind: "eligible", accounts: available, skippedSlots: skipped, blockedAccounts };
  if (hasUpstreamTimeoutBlock) {
    return { kind: "upstream_blocked", skippedSlots: skipped, retryAtMs: retryAt, blockedAccounts: [] };
  }
  if (hasQuotaBlock) {
    return {
      kind: "quota_blocked",
      skippedSlots: skipped,
      retryAtMs: retryAt,
      blockedAccounts,
      fullCohortExhausted,
      activeSnapshot,
      poolSnapshotJson,
      capacitySnapshotJson,
    };
  }
  return { kind: "credentials_invalid", skippedSlots: skipped };
};

const parseCodexAuthPoolSnapshot = (value: unknown): CodexAuthPoolState | null => {
  if (!isRecord(value) || !Array.isArray(value.accounts) || !isSafeMs(value.updated_at_ms) || value.accounts.length < 1) return null;
  const accountIds = new Set<string>();
  const accounts: CodexAuthState[] = [];
  for (const candidate of value.accounts) {
    if (!isRecord(candidate)) return null;
    const accountId = getString(candidate.account_id);
    const accessToken = getString(candidate.access_token);
    const refreshToken = getString(candidate.refresh_token);
    if (!accountId || !accessToken || !refreshToken || !isSafeMs(candidate.updated_at_ms) || accountIds.has(accountId)) return null;
    accountIds.add(accountId);
    accounts.push({ account_id: accountId, access_token: accessToken, refresh_token: refreshToken, updated_at_ms: candidate.updated_at_ms });
  }
  return { accounts, updated_at_ms: value.updated_at_ms };
};

const routingStateChangedByNormalization = (durable: CodexAccountRoutingState | null, normalized: CodexAccountRoutingState): boolean => {
  if (durable === null) return true;
  return (
    durable.banked_reset_legacy_identity_unresolved !== normalized.banked_reset_legacy_identity_unresolved ||
    durable.slots.length !== normalized.slots.length ||
    durable.slots.some((slot, index) => slot !== normalized.slots[index])
  );
};

type SerialRoutingEvaluations = Readonly<{
  accumulated: CodexRoutingAccumulation;
  byAccountId: ReadonlyMap<string, CodexRoutingAccountEvaluation>;
  identityByAccountId: ReadonlyMap<string, CodexRoutingSlotIdentity>;
}>;

/** Evaluate every configured account, preserving pool order without headroom ranking. */
const evaluateSerialRoutingAccounts = async (
  state: CodexAccountRoutingState,
  pool: CodexAuthPoolState,
  model: string | null,
  capacityObservations: readonly CodexCapacityRoutingObservation[],
  now: number
): Promise<SerialRoutingEvaluations> => {
  const identities = await Promise.all(pool.accounts.map(routingAccountIdentity));
  const byId = new Map<string, CodexRoutingSlotIdentity>();
  for (const [slot, auth] of pool.accounts.entries()) {
    const identity = identities.at(slot);
    if (identity !== undefined) byId.set(auth.account_id, { slot, accountIdHash: identity.accountIdHash, credentialVersion: identity.credentialVersion });
  }
  const observationsByAccount = new Map(capacityObservations.map((observation) => [observation.account_id_hash, observation]));
  const accumulator: CodexRoutingAccumulator = {
    available: [],
    blockedAccounts: [],
    skipped: [],
    retryAt: null,
    hasQuotaBlock: false,
    hasUpstreamTimeoutBlock: false,
  };
  const byAccountId = new Map<string, CodexRoutingAccountEvaluation>();
  for (const auth of pool.accounts) {
    const mapped = byId.get(auth.account_id);
    if (!mapped) continue;
    const evaluated = evaluateCodexRoutingAccount(state, auth, mapped, model, observationsByAccount, now);
    byAccountId.set(auth.account_id, evaluated);
    foldCodexRoutingEvaluation(accumulator, evaluated);
  }
  return { accumulated: { ...accumulator }, byAccountId, identityByAccountId: byId };
};

const activeSelectionForAccount = (
  account: RoutingAccount,
  poolVersionstamp: string,
  generation: number,
  now: number,
  transitionReason: CodexActiveAccountTransitionReason | null
): CodexActiveAccountSelection | null => {
  if (account.routingGeneration === undefined || !isVersionstamp(poolVersionstamp) || !Number.isSafeInteger(generation) || generation < 1) return null;
  return {
    v: 1,
    account_id_hash: account.accountIdHash,
    credential_version: account.credentialVersion,
    pool_versionstamp: poolVersionstamp,
    slot: account.slot,
    routing_generation: account.routingGeneration,
    generation,
    transition_reason: transitionReason,
    updated_at_ms: now,
  };
};

const activeSelectionNeedsRefresh = (active: CodexActiveAccountSelection, account: RoutingAccount, poolVersionstamp: string): boolean =>
  active.account_id_hash !== account.accountIdHash ||
  active.credential_version !== account.credentialVersion ||
  active.pool_versionstamp !== poolVersionstamp ||
  active.slot !== account.slot ||
  active.routing_generation !== account.routingGeneration;

type SerialSelectionDecision = Readonly<{
  selection: RouteSelection;
  nextActive: CodexActiveAccountSelection | null;
}>;

type SerialSelectionContext = Readonly<{
  evaluations: SerialRoutingEvaluations;
  poolVersionstamp: string;
  now: number;
  poolSnapshotJson: string;
  capacitySnapshotJson: string;
  eligibleInPoolOrder: readonly RoutingAccount[];
  fullCohortExhausted: boolean;
  activeEvaluation: CodexRoutingAccountEvaluation | null;
}>;

/** One eligible account admitted under a committed active row. */
const eligibleSerialSelection = (account: RoutingAccount, selection: CodexActiveAccountSelection, evaluations: SerialRoutingEvaluations): RouteSelection => ({
  kind: "eligible",
  accounts: [
    {
      ...account,
      activeGeneration: selection.generation,
      activePoolVersionstamp: selection.pool_versionstamp,
      activeTransitionReason: selection.transition_reason,
    },
  ],
  skippedSlots: evaluations.accumulated.skipped,
  blockedAccounts: evaluations.accumulated.blockedAccounts,
});

/**
 * The blocked/credential-invalid result bound to the active row as this
 * decision leaves it: a same-identity fence refresh the decision commits is
 * the observed active row, not a stale pre-refresh value.
 */
const blockedSerialSelection = (context: SerialSelectionContext, current: CodexActiveAccountSelection | null): RouteSelection =>
  classifyCodexRouteSelection(
    context.evaluations.accumulated,
    context.fullCohortExhausted,
    { selection: current },
    context.poolSnapshotJson,
    context.capacitySnapshotJson
  );

/** A missing active row bootstraps the first account eligible under the requested model. */
const serialBootstrapDecision = (context: SerialSelectionContext): SerialSelectionDecision | null => {
  const bootstrap = context.eligibleInPoolOrder.at(0);
  if (!bootstrap) return { selection: blockedSerialSelection(context, null), nextActive: null };
  const nextActive = activeSelectionForAccount(bootstrap, context.poolVersionstamp, 1, context.now, null);
  if (!nextActive) return null;
  return { selection: eligibleSerialSelection(bootstrap, nextActive, context.evaluations), nextActive };
};

/**
 * A pool reorder preserves the opaque identity; removal or replacement is the
 * only membership observation that may select a sibling.
 */
const serialReplacementDecision = (context: SerialSelectionContext, active: CodexActiveAccountSelection): SerialSelectionDecision | null => {
  const replacement = context.eligibleInPoolOrder.at(0);
  if (!replacement) return { selection: blockedSerialSelection(context, active), nextActive: null };
  const nextActive = activeSelectionForAccount(replacement, context.poolVersionstamp, active.generation + 1, context.now, "account_removed_or_replaced");
  if (!nextActive) return null;
  return { selection: eligibleSerialSelection(replacement, nextActive, context.evaluations), nextActive };
};

/** The active account is still eligible; only its same-identity fences may refresh. */
const serialCurrentActiveDecision = (
  context: SerialSelectionContext,
  active: CodexActiveAccountSelection,
  current: RoutingAccount
): SerialSelectionDecision | null => {
  const needsRefresh = activeSelectionNeedsRefresh(active, current, context.poolVersionstamp);
  const refreshed = needsRefresh
    ? activeSelectionForAccount(current, context.poolVersionstamp, active.generation, context.now, active.transition_reason)
    : null;
  if (needsRefresh && refreshed === null) return null;
  return { selection: eligibleSerialSelection(current, refreshed ?? active, context.evaluations), nextActive: refreshed };
};

/** The active account is blocked, invalid, or probing: move only for an authoritative reason. */
const serialBlockedActiveDecision = (
  context: SerialSelectionContext,
  active: CodexActiveAccountSelection,
  activeEvaluation: CodexRoutingAccountEvaluation
): SerialSelectionDecision | null => {
  // A recovery-probe lease or transient circuit is not capacity evidence. It
  // must fail retryably without sending traffic to a healthy sibling, and it
  // must not unlock reset or paid fallback paths.
  if (activeEvaluation.activeTransitionReason === null) return { selection: { kind: "routing_unavailable" }, nextActive: null };
  const replacement = context.eligibleInPoolOrder.at(0);
  if (!replacement) {
    const needsRefresh = activeSelectionNeedsRefresh(active, activeEvaluation.account, context.poolVersionstamp);
    const refreshed = needsRefresh
      ? activeSelectionForAccount(activeEvaluation.account, context.poolVersionstamp, active.generation, context.now, active.transition_reason)
      : null;
    if (needsRefresh && refreshed === null) return null;
    return { selection: blockedSerialSelection(context, refreshed ?? active), nextActive: refreshed };
  }
  const nextActive = activeSelectionForAccount(
    replacement,
    context.poolVersionstamp,
    active.generation + 1,
    context.now,
    activeEvaluation.activeTransitionReason
  );
  if (!nextActive) return null;
  return { selection: eligibleSerialSelection(replacement, nextActive, context.evaluations), nextActive };
};

const serialSelectionDecision = (
  active: CodexActiveAccountSelection | null,
  pool: CodexAuthPoolState,
  evaluations: SerialRoutingEvaluations,
  poolVersionstamp: string,
  now: number,
  poolSnapshotJson: string,
  capacitySnapshotJson: string
): SerialSelectionDecision | null => {
  const eligibleInPoolOrder = pool.accounts
    .map((auth) => evaluations.byAccountId.get(auth.account_id)?.routedAccount ?? null)
    .filter((account): account is RoutingAccount => account !== null);
  const accountForActive =
    active === null ? null : pool.accounts.find((account) => evaluations.identityByAccountId.get(account.account_id)?.accountIdHash === active.account_id_hash);
  const activeEvaluation = accountForActive ? (evaluations.byAccountId.get(accountForActive.account_id) ?? null) : null;
  const fullCohortExhausted =
    pool.accounts.length > 0 && pool.accounts.every((auth) => evaluations.byAccountId.get(auth.account_id)?.activeTransitionReason === "quota_exhausted");
  const context: SerialSelectionContext = {
    evaluations,
    poolVersionstamp,
    now,
    poolSnapshotJson,
    capacitySnapshotJson,
    eligibleInPoolOrder,
    fullCohortExhausted,
    activeEvaluation,
  };
  if (active === null) return serialBootstrapDecision(context);
  if (activeEvaluation === null) return serialReplacementDecision(context, active);
  const current = activeEvaluation.routedAccount;
  if (current !== null) return serialCurrentActiveDecision(context, active, current);
  return serialBlockedActiveDecision(context, active, activeEvaluation);
};

/** One coherent strong snapshot of the four rows every admission decision linearizes on, in a fixed order. */
const readStrongCodexRoutingRows = async (kv: Deno.Kv) => {
  const [activeEntry, authEntry, routingEntry, capacityEntry] = await kv.getMany<[unknown, unknown, CodexAccountRoutingState, unknown]>(
    [CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, CODEX_AUTH_POOL_KV_KEY, CODEX_ACCOUNT_ROUTING_KV_KEY, CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY],
    { consistency: "strong" }
  );
  return { activeEntry, authEntry, routingEntry, capacityEntry };
};

type CodexSerialAdmissionRows = Readonly<{
  active: CodexActiveAccountSelection | null;
  durablePool: CodexAuthPoolState;
  durableRouting: CodexAccountRoutingState | null;
  normalized: CodexAccountRoutingState;
  poolVersionstamp: string;
  evaluations: SerialRoutingEvaluations;
  poolSnapshotJson: string;
  capacitySnapshotJson: string;
}>;

/**
 * The configured subscriptions the operator has left eligible for routing.
 * Durable slots keep tracking every configured account — the routing state is
 * passed the full pool — so re-enabling a subscription resumes its recorded
 * quota history instead of rebuilding it. Only the accounts an admission may
 * select are narrowed here.
 *
 * Narrowing the pool is itself the operator's decision, so a reduced cohort is
 * the cohort: "every account is exhausted" and the banked-reset cohort both
 * describe the subscriptions that are still switched on.
 */
const selectedCodexSubscriptionPool = async (pool: CodexAuthPoolState): Promise<CodexAuthPoolState> => {
  const eligibility = codexAccountEligibility(await loadProviderSelectionCached());
  if (eligibility.kind === "all") return pool;
  if (eligibility.kind === "none") return pool.accounts.length ? { ...pool, accounts: [] } : pool;
  const enabled = new Set<string>(eligibility.hashes);
  const accounts: CodexAuthState[] = [];
  for (const account of pool.accounts) {
    if (enabled.has(await codexSubscriptionHash(account.account_id))) accounts.push(account);
  }
  return accounts.length === pool.accounts.length ? pool : { ...pool, accounts };
};

/**
 * Parse one strong-read snapshot. Any malformed or unavailable durable row
 * fails the admission before any decision or write.
 */
const prepareCodexSerialAdmissionRows = async (
  rows: Awaited<ReturnType<typeof readStrongCodexRoutingRows>>,
  now: number,
  model: string | null
): Promise<CodexSerialAdmissionRows | null> => {
  const active = rows.activeEntry.value === null ? null : parseCodexActiveAccountSelection(rows.activeEntry.value);
  if (rows.activeEntry.value !== null && active === null) return null;
  const durablePool = rows.authEntry.value === null ? null : parseCodexAuthPoolSnapshot(rows.authEntry.value);
  const poolVersionstamp = rows.authEntry.versionstamp;
  if (!durablePool || !isVersionstamp(poolVersionstamp)) return null;
  const durableRouting = rows.routingEntry.value === null ? null : parseCodexAccountRoutingState(rows.routingEntry.value);
  if (rows.routingEntry.value !== null && durableRouting === null) return null;
  const normalized = await normalizeRoutingState(durableRouting, durablePool, now, true);
  const observations = parseStoredCapacityObservationStore(rows.capacityEntry.value);
  // The admission pool and the decision pool must be the same pool, or a
  // switched-off subscription could still be elected as the active account.
  const routingPool = await selectedCodexSubscriptionPool(durablePool);
  const evaluations = await evaluateSerialRoutingAccounts(normalized, routingPool, model, observations, now);
  return {
    active,
    durablePool: routingPool,
    durableRouting,
    normalized,
    poolVersionstamp,
    evaluations,
    poolSnapshotJson: JSON.stringify(rows.authEntry.value ?? null),
    capacitySnapshotJson: JSON.stringify(rows.capacityEntry.value ?? null),
  };
};

/**
 * Commit one admission decision against the exact rows it was derived from.
 * The routing write precedes the active write, and the isolate cache is
 * refreshed only after a successful commit. A conflict reloads every row.
 */
const commitCodexSerialAdmission = async (
  kv: Deno.Kv,
  rows: Awaited<ReturnType<typeof readStrongCodexRoutingRows>>,
  prepared: CodexSerialAdmissionRows,
  decision: SerialSelectionDecision
): Promise<boolean> => {
  const routingChanged = routingStateChangedByNormalization(prepared.durableRouting, prepared.normalized);
  if (!routingChanged && decision.nextActive === null) {
    // A check-only transaction linearizes the admission snapshot without
    // writing on the hot path. A concurrent active/pool/routing/capacity
    // update restarts the bounded strong-read loop before dispatch.
    const admitted = await kv.atomic().check(rows.activeEntry).check(rows.authEntry).check(rows.routingEntry).check(rows.capacityEntry).commit();
    if (!admitted.ok) return false;
    setRoutingStateCache(prepared.normalized, rows.routingEntry.versionstamp);
    return true;
  }
  let operation = kv.atomic().check(rows.activeEntry).check(rows.authEntry).check(rows.routingEntry).check(rows.capacityEntry);
  if (routingChanged) operation = operation.set(CODEX_ACCOUNT_ROUTING_KV_KEY, prepared.normalized);
  if (decision.nextActive !== null) operation = operation.set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, decision.nextActive);
  const committed = await operation.commit();
  if (!committed.ok) return false;
  setRoutingStateCache(prepared.normalized, routingChanged ? committed.versionstamp : rows.routingEntry.versionstamp);
  return true;
};

/**
 * FIFO admission tail per KV instance. Concurrent routing admissions in one
 * isolate otherwise snapshot the same pre-bootstrap rows and exhaust their
 * three CAS attempts as conflicts, so only the short strong-selection and
 * same-identity fence decisions are serialized; auth refresh, transport,
 * inference, and streams stay concurrent. Stored promises are release-only and
 * never reject.
 */
const admissionTails = new WeakMap<Deno.Kv, Promise<void>>();

/**
 * Run one short routing-admission action under the per-KV FIFO tail. This
 * caller's release-only promise is installed before it waits, and is always
 * released, so a failure can never poison later callers.
 */
const withCodexAdmission = async <T>(kv: Deno.Kv, action: () => Promise<T>): Promise<T> => {
  const priorAdmission = admissionTails.get(kv) ?? Promise.resolve();
  let releaseAdmission = (): void => {};
  const admissionRelease = new Promise<void>((resolve) => {
    releaseAdmission = resolve;
  });
  admissionTails.set(kv, admissionRelease);
  try {
    await priorAdmission;
    return await action();
  } finally {
    releaseAdmission();
    if (admissionTails.get(kv) === admissionRelease) admissionTails.delete(kv);
  }
};

/**
 * Every ordinary admission strongly reads the active selection, auth pool,
 * and routing state. A missing active row is the one bootstrap case; any
 * malformed or unavailable durable state fails before upstream dispatch.
 */
const selectSerialCodexRoutingAccounts = async (
  _suppliedPool: CodexAuthPoolState,
  _orderedAccounts: readonly CodexAuthState[],
  now: number,
  model: string | null
): Promise<RouteSelection> => {
  try {
    const kv = await getKv();
    if (!kv) return { kind: "routing_unavailable" };
    return await withCodexAdmission(kv, async (): Promise<RouteSelection> => {
      for (let attempt = 0; attempt < 3; attempt += 1) {
        const rows = await readStrongCodexRoutingRows(kv);
        const prepared = await prepareCodexSerialAdmissionRows(rows, now, model);
        if (prepared === null) return { kind: "routing_unavailable" };
        const decision = serialSelectionDecision(
          prepared.active,
          prepared.durablePool,
          prepared.evaluations,
          prepared.poolVersionstamp,
          now,
          prepared.poolSnapshotJson,
          prepared.capacitySnapshotJson
        );
        if (decision === null) return { kind: "routing_unavailable" };
        if (!(await commitCodexSerialAdmission(kv, rows, prepared, decision))) continue;
        return decision.selection;
      }
      return { kind: "routing_unavailable" };
    });
  } catch {
    return { kind: "routing_unavailable" };
  }
};

/** Exact value binding to the active row one strong read observed. */
export const isCodexActiveAccountSnapshotCurrent = (value: unknown, snapshot: CodexActiveAccountSnapshot): boolean => {
  const current = parseCodexActiveAccountSelection(value);
  const observed = snapshot.selection;
  if (current === null || observed === null) return current === null && observed === null;
  return (
    current.account_id_hash === observed.account_id_hash &&
    current.credential_version === observed.credential_version &&
    current.pool_versionstamp === observed.pool_versionstamp &&
    current.slot === observed.slot &&
    current.routing_generation === observed.routing_generation &&
    current.generation === observed.generation &&
    current.transition_reason === observed.transition_reason &&
    current.updated_at_ms === observed.updated_at_ms
  );
};

/**
 * Elect the account a verified banked reset selected. The original active
 * snapshot must still be current, the candidate's pool credential and quota
 * routing fence must match, and every current pool account must still be
 * authoritatively exhausted: a concurrent active switch, a recovered sibling,
 * or newly eligible ordinary capacity wins over the stale reset. The same
 * account keeps its active generation; an account change advances it.
 */
/**
 * A ledger record that predates later routing transitions cannot satisfy the
 * exact current-generation fence; the same proof the stale reconciliation is
 * about to apply is sufficient here.
 */
const staleResetRoutingIdentityCurrent = (value: unknown, account: RoutingAccount, quotaResetAtMs: number): boolean => {
  const state = parseCodexAccountRoutingState(value);
  const current = state?.slots.at(account.slot);
  if (!current) return false;
  const classBlock = quotaBlockForClass(current, quotaClass(account.requestedModel));
  return (
    current.account_id_hash === account.accountIdHash &&
    current.credential_version === account.credentialVersion &&
    current.generation === account.routingGeneration &&
    current.invalid_credential_version !== account.credentialVersion &&
    classBlock !== null &&
    classBlock.observed_reset_at_ms === quotaResetAtMs &&
    classBlock.observed_reset_at_is_stable
  );
};

/** The same account keeps its active generation; an account change advances it. */
const nextResetElectionGeneration = (active: CodexActiveAccountSelection | null, candidate: RoutingAccount): number => {
  if (active === null) return 1;
  if (active.account_id_hash === candidate.accountIdHash && active.slot === candidate.slot) return active.generation;
  return active.generation + 1;
};

/** The exact pool credential an elected slot must still carry. */
const routingAuthMatchesAccount = (currentAuth: CodexAuthState, account: RoutingAccount): boolean =>
  currentAuth.account_id === account.auth.account_id &&
  currentAuth.access_token === account.auth.access_token &&
  currentAuth.refresh_token === account.auth.refresh_token;

/** The normalized slot an election candidate must still own, including its quota fence. */
const resetCandidateSlotMatches = (currentSlot: CodexRoutingSlot, account: RoutingAccount, routingIdentityCurrent: boolean): boolean =>
  currentSlot.account_id_hash === account.accountIdHash &&
  currentSlot.credential_version === account.credentialVersion &&
  currentSlot.generation === account.routingGeneration &&
  routingIdentityCurrent;

type CodexResetElectionPreparation = Readonly<{
  normalized: CodexAccountRoutingState;
  routing: CodexAccountRoutingState | null;
  routedCandidate: RoutingAccount;
  next: CodexActiveAccountSelection;
}>;

/**
 * Prove the original active snapshot, exact credential, routing fence, and
 * complete exhausted cohort, then build the next active row. Normalization,
 * evaluation, and the row itself keep their separate clock reads.
 */
const prepareCodexResetRecoveryElection = async (
  account: RoutingAccount,
  routingGeneration: number,
  originalActive: CodexActiveAccountSnapshot,
  quotaResetAtMs: number,
  staleVerified: boolean,
  rows: Awaited<ReturnType<typeof readStrongCodexRoutingRows>>
): Promise<CodexResetElectionPreparation | null> => {
  if (!isCodexActiveAccountSnapshotCurrent(rows.activeEntry.value, originalActive)) return null;
  const active = originalActive.selection;
  const pool = rows.authEntry.value === null ? null : parseCodexAuthPoolSnapshot(rows.authEntry.value);
  const poolVersionstamp = rows.authEntry.versionstamp;
  if (!pool || !isVersionstamp(poolVersionstamp)) return null;
  const currentAuth = pool.accounts.at(account.slot);
  if (!currentAuth || !routingAuthMatchesAccount(currentAuth, account)) return null;
  const routing = rows.routingEntry.value === null ? null : parseCodexAccountRoutingState(rows.routingEntry.value);
  if (rows.routingEntry.value !== null && routing === null) return null;
  const normalized = await normalizeRoutingState(routing, pool, Date.now(), true);
  const currentSlot = normalized.slots.at(account.slot);
  const routingIdentityCurrent = staleVerified
    ? staleResetRoutingIdentityCurrent(rows.routingEntry.value, account, quotaResetAtMs)
    : isCodexQuotaBlockFenceCurrent(rows.routingEntry.value, account, quotaResetAtMs, routingGeneration);
  if (!currentSlot || !resetCandidateSlotMatches(currentSlot, account, routingIdentityCurrent)) return null;
  const accountIdHash = currentSlot.account_id_hash;
  if (accountIdHash === null) return null;
  const observations = parseStoredCapacityObservationStore(rows.capacityEntry.value);
  const evaluations = await evaluateSerialRoutingAccounts(normalized, pool, account.requestedModel ?? null, observations, Date.now());
  const fullCohortExhausted =
    pool.accounts.length > 0 && pool.accounts.every((auth) => evaluations.byAccountId.get(auth.account_id)?.activeTransitionReason === "quota_exhausted");
  if (!fullCohortExhausted) return null;
  const routedCandidate: RoutingAccount = {
    ...account,
    accountIdHash,
    credentialVersion: currentSlot.credential_version,
    routingGeneration: currentSlot.generation,
  };
  const next = activeSelectionForAccount(
    routedCandidate,
    poolVersionstamp,
    nextResetElectionGeneration(active, routedCandidate),
    Date.now(),
    "quota_exhausted"
  );
  if (!next) return null;
  return { normalized, routing, routedCandidate, next };
};

/** Commit the prepared election, writing routing before the active row. */
const commitCodexResetRecoveryElection = async (
  kv: Deno.Kv,
  rows: Awaited<ReturnType<typeof readStrongCodexRoutingRows>>,
  prepared: CodexResetElectionPreparation
): Promise<RoutingAccount | "conflict"> => {
  const routingChanged = routingStateChangedByNormalization(prepared.routing, prepared.normalized);
  let operation = kv.atomic().check(rows.activeEntry).check(rows.authEntry).check(rows.routingEntry).check(rows.capacityEntry);
  if (routingChanged) operation = operation.set(CODEX_ACCOUNT_ROUTING_KV_KEY, prepared.normalized);
  const committed = await operation.set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, prepared.next).commit();
  if (!committed.ok) return "conflict";
  setRoutingStateCache(prepared.normalized, routingChanged ? committed.versionstamp : rows.routingEntry.versionstamp);
  return {
    ...prepared.routedCandidate,
    activeGeneration: prepared.next.generation,
    activePoolVersionstamp: prepared.next.pool_versionstamp,
    activeTransitionReason: prepared.next.transition_reason,
  };
};

export const electCodexResetRecoveryAccount = async (
  account: RoutingAccount,
  originalActive: CodexActiveAccountSnapshot,
  quotaResetAtMs: number,
  staleVerified = false
): Promise<RoutingAccount | null> => {
  if (account.routingGeneration === undefined || !isSafeMs(quotaResetAtMs)) return null;
  const routingGeneration = account.routingGeneration;
  try {
    const kv = await getKv();
    if (!kv) return null;
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const rows = await readStrongCodexRoutingRows(kv);
      const prepared = await prepareCodexResetRecoveryElection(account, routingGeneration, originalActive, quotaResetAtMs, staleVerified, rows);
      if (prepared === null) return null;
      const elected = await commitCodexResetRecoveryElection(kv, rows, prepared);
      if (elected === "conflict") continue;
      return elected;
    }
  } catch {
    return null;
  }
  return null;
};

/** The routing generation an admission refresh must bind to, or null when its slot cannot be proven. */
const resolveAdmissionRefreshGeneration = async (account: RoutingAccount, pool: CodexAuthPoolState, routingValue: unknown): Promise<number | null> => {
  const routing = parseCodexAccountRoutingState(routingValue);
  if (routingValue !== null && routing === null) return null;
  const normalized = routing === null ? null : await normalizeRoutingState(routing, pool, Date.now(), true);
  const currentSlot = normalized?.slots.at(account.slot) ?? null;
  if (normalized !== null && currentSlot === null) return null;
  if (
    currentSlot !== null &&
    (currentSlot.account_id_hash !== account.accountIdHash ||
      currentSlot.credential_version !== account.credentialVersion ||
      currentSlot.invalid_credential_version === account.credentialVersion)
  )
    return null;
  return currentSlot?.generation ?? 0;
};

type CodexAdmissionRefreshPreparation =
  Readonly<{ kind: "current" }> | Readonly<{ kind: "refresh"; next: CodexActiveAccountSelection }> | Readonly<{ kind: "reject" }>;

/**
 * Prove the admitted active identity and generation, then prepare a same-identity
 * fence refresh. A current row returns without any write; every conflict rejects
 * so the caller reloads all four rows.
 */
const prepareCodexActiveAccountAdmissionRefresh = async (
  account: RoutingAccount,
  rows: Awaited<ReturnType<typeof readStrongCodexRoutingRows>>
): Promise<CodexAdmissionRefreshPreparation> => {
  const active = rows.activeEntry.value === null ? null : parseCodexActiveAccountSelection(rows.activeEntry.value);
  if (active === null) return { kind: "reject" };
  if (active.generation !== account.activeGeneration) return { kind: "reject" };
  if (active.account_id_hash !== account.accountIdHash || active.slot !== account.slot) return { kind: "reject" };
  const pool = rows.authEntry.value === null ? null : parseCodexAuthPoolSnapshot(rows.authEntry.value);
  const poolVersionstamp = rows.authEntry.versionstamp;
  if (!pool || !isVersionstamp(poolVersionstamp)) return { kind: "reject" };
  const currentAuth = pool.accounts.at(account.slot);
  if (!currentAuth || !routingAuthMatchesAccount(currentAuth, account)) return { kind: "reject" };
  const routingGeneration = await resolveAdmissionRefreshGeneration(account, pool, rows.routingEntry.value);
  if (routingGeneration === null) return { kind: "reject" };
  if (
    active.credential_version === account.credentialVersion &&
    active.pool_versionstamp === poolVersionstamp &&
    active.routing_generation === routingGeneration
  ) {
    return { kind: "current" };
  }
  return {
    kind: "refresh",
    next: {
      ...active,
      credential_version: account.credentialVersion,
      pool_versionstamp: poolVersionstamp,
      routing_generation: routingGeneration,
      updated_at_ms: Date.now(),
    },
  };
};

/**
 * Final pre-transport admission. The active identity and generation must still
 * match the admitted request; the request's own probe claim or same-identity
 * credential refresh may have advanced the credential/routing fences, so those
 * are refreshed atomically without changing the active identity or generation.
 * A concurrent active switch, pool rotation, or conflicting writer fails.
 */
export const refreshCodexActiveAccountAdmission = async (account: RoutingAccount): Promise<boolean> => {
  if (account.activeGeneration === undefined || !Number.isSafeInteger(account.activeGeneration) || account.activeGeneration < 1) return false;
  try {
    const kv = await getKv();
    if (!kv) return false;
    return await withCodexAdmission(kv, async (): Promise<boolean> => {
      for (let attempt = 0; attempt < 3; attempt += 1) {
        const rows = await readStrongCodexRoutingRows(kv);
        const prepared = await prepareCodexActiveAccountAdmissionRefresh(account, rows);
        if (prepared.kind === "reject") return false;
        if (prepared.kind === "current") return true;
        const committed = await kv
          .atomic()
          .check(rows.activeEntry)
          .check(rows.authEntry)
          .check(rows.routingEntry)
          .check(rows.capacityEntry)
          .set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, prepared.next)
          .commit();
        if (!committed.ok) continue;
        return true;
      }
      return false;
    });
  } catch {
    return false;
  }
};

const selectCodexRoutingAccountsFromState = async (
  state: CodexAccountRoutingState,
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now: number,
  model: string | null,
  capacityObservations: readonly CodexCapacityRoutingObservation[]
): Promise<RouteSelection> => {
  const identities = await Promise.all(pool.accounts.map(routingAccountIdentity));
  const byId = new Map<string, CodexRoutingSlotIdentity>();
  for (const [slot, auth] of pool.accounts.entries()) {
    const identity = identities.at(slot);
    if (identity === undefined) continue;
    byId.set(auth.account_id, { slot, accountIdHash: identity.accountIdHash, credentialVersion: identity.credentialVersion });
  }
  const observationsByAccount = new Map(capacityObservations.map((observation) => [observation.account_id_hash, observation]));
  return classifyCodexRouteSelection(accumulateCodexRoutingAccounts(state, byId, orderedAccounts, model, observationsByAccount, now));
};

/**
 * Slot-scoped eligibility for the internal prompt-cache experiment and other
 * pinned callers that already chose their account. It hydrates the shared
 * routing/capacity caches and reconciles existing capacity observations exactly
 * as the pre-cutover helper did, so a following transition CAS is based on the
 * row this read returned. It never reads or writes the global active-selection
 * row; ordinary inference admissions must use `selectCodexRoutingAccountsStrong`.
 */
export const selectCodexRoutingAccounts = async (
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now = Date.now(),
  model: string | null = null
): Promise<RouteSelection> =>
  await (async () => {
    const state = await loadCodexAccountRouting(pool);
    const observations = await loadCodexCapacityRoutingObservations(pool);
    const reconciled = await reconcileCapacityRoutingState(state, observations, now, model);
    return await selectCodexRoutingAccountsFromState(reconciled, pool, orderedAccounts, now, model, observations);
  })();

export const selectCodexRoutingAccountsStrong = async (
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now = Date.now(),
  model: string | null = null
): Promise<RouteSelection> => await selectSerialCodexRoutingAccounts(pool, orderedAccounts, now, model);

/**
 * A rechecked slot keeps its circuit deadline but loses the reset identity that
 * authorized it: an administrative deadline mutation is not provider proof of
 * the same quota generation, so a later recovery probe is required before a
 * stable identity may authorize a banked claim again.
 */
const recheckSlotTransition = (current: CodexRoutingSlot, recheckAtMs: number): CodexRoutingSlot => {
  const rechecked = recheckQuotaClasses(current, recheckAtMs);
  return {
    ...rechecked,
    quota_signal_observed_at_ms: recheckAtMs,
    banked_reset_generation_ambiguous:
      rechecked.banked_reset_generation_ambiguous ||
      rechecked.quota_blocked_until_ms !== null ||
      current.banked_reset_generation_ambiguous ||
      (current.observed_reset_at_ms !== null && current.observed_reset_at_is_stable),
    generation: current.generation + 1,
    probe_lease: null,
  };
};

/** Resolve a 1-based administrative slot number against the state's slots. */
const locateRoutingSlot = (state: CodexAccountRoutingState, slotNumber: number): Readonly<{ index: number; slot: CodexRoutingSlot }> | null => {
  if (slotNumber > state.slots.length) return null;
  const index = slotNumber - 1;
  const slot = state.slots.at(index);
  return slot === undefined ? null : { index, slot };
};

/** Strong-read recheck loop: three compare-and-set attempts, then give up. */
const recheckCodexRoutingSlotFromKv = async (kv: Deno.Kv, slotNumber: number, recheckAtMs: number): Promise<boolean> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
    const state = parseCodexAccountRoutingState(entry.value);
    if (!state) return false;
    const located = locateRoutingSlot(state, slotNumber);
    if (located === null) return false;
    if (!located.slot.quota_blocked_until_ms) {
      setRoutingStateCache(state, entry.versionstamp);
      return true;
    }
    const next = withSlot(state, located.index, recheckSlotTransition(located.slot, recheckAtMs));
    const committed = await kv.atomic().check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, next).commit();
    if (committed.ok) {
      setRoutingStateCache(next, committed.versionstamp);
      return true;
    }
  }
  return false;
};

/** Recheck against the cached record when KV is unavailable. */
const recheckCodexRoutingSlotLocally = (slotNumber: number, recheckAtMs: number): boolean => {
  const state = getCachedRoutingState();
  if (!state) return false;
  const located = locateRoutingSlot(state, slotNumber);
  if (located === null) return false;
  if (!located.slot.quota_blocked_until_ms) return true;
  setRoutingStateCache(withSlot(state, located.index, recheckSlotTransition(located.slot, recheckAtMs)));
  return true;
};

/**
 * Makes a manually redeemed reset eligible for one normal-request probe.
 * This never clears reset-generation ambiguity: only a successful probe can
 * prove recovery and authorize a later provisional identity.
 */
export const recheckCodexRoutingSlot = async (slotNumber: number): Promise<boolean> => {
  if (!Number.isInteger(slotNumber) || slotNumber < 1) return false;
  const recheckAtMs = Date.now();

  // Rechecks are rare, administrative transitions. Always use a strong read
  // when available so a cold isolate can release a persisted circuit and a
  // stale local cache cannot overwrite a newer quota deadline.
  const kv = await openRoutingKv();
  if (!kv) return recheckCodexRoutingSlotLocally(slotNumber, recheckAtMs);
  return await recheckCodexRoutingSlotFromKv(kv, slotNumber, recheckAtMs);
};

// The state, capacity and 429 layers moved into their own modules; they are
// re-exported here until the rest of this module is extracted too.
export * from "./codex_routing_state.ts";
export * from "./codex_capacity_routing.ts";
export * from "./codex_429.ts";
