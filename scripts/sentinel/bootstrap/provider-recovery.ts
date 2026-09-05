// Protected Provider Sentinel recovery operation (provider owner rollback).
//
// This module belongs to the bootstrap trust domain and imports only its
// protected siblings: the strict provider-state schema parser, the strict
// GitHub Actions executor-identity verifier, and the Deno deploy client. It
// never resolves a revision from list order, never reads candidate or stable
// health to authorize restoration, never promotes anything except the exact
// immutably attested previous revision, and never writes Git itself — every
// durable write goes through the injected exact-parent compare-and-set store.
//
// Activation contract: the injected verification/retention/lock adapters
// belong to the protected executor, never to candidate code. Production
// wiring MUST enforce the shared ai-ubq-fi deploy lock through
// withPromotionLock and MUST verify the restoration through an independent
// frozen authenticated acceptance that checks the exact managed body and
// headers, the custom-domain policy, and an authenticated inference against
// the frozen corpus with equivalent config. Those adapters are explicit
// unimplemented activation requirements of this unit, not proof from caller
// claims. No token ever enters a retained event; the active authority
// verifier already rejects token echo.
//
// Every invocation is bounded to at most one previous promotion POST and a
// fixed number of reads and CAS attempts: no sleeps, polling, retries, or
// model calls. Remote, control, verification, and CAS uncertainty yields a
// pending result with a fixed safe reason and leaves the durable rollback
// intent unresolved; only the final exact-parent CAS returns rolled_back.

import { defaultRevisionHealthUrl, type DenoDeployClient, type ProductionRouteOwnership } from "./deploy.ts";
import {
  ExecutorAuthorityError,
  parseProviderExecutorIdentity,
  type VerifiedActiveExecutorAuthority,
  verifyActiveProviderExecutorAuthority,
} from "./executor.ts";
import {
  executorEquals,
  isTerminalSentinelProviderPhase,
  parseSentinelProviderStateDocument,
  type SentinelProviderAppName,
  type SentinelProviderAttestationV1,
  type SentinelProviderExecutorV1,
  type SentinelProviderPromotionResultV1,
  type SentinelProviderStateDocumentV1,
  type SentinelProviderTransactionV1,
} from "./provider-state.ts";

const ROUTE_ORGANIZATION = "ubiquity-dao" as const;
const OBSERVATION_CADENCE_MS = 30_000;
const FULL_GIT_SHA = /^[0-9a-f]{40}$/u;
/** Exactly the provider schema's bounded transaction identifier shape. */
const TRANSACTION_ID = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$/u;
/** Exactly canonical Date.toISOString() output: UTC, 3-digit milliseconds. */
const ISO_UTC = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/u;

export type ProviderRecoveryPendingReason =
  | "rollback_not_authorized"
  | "state_conflict"
  | "control_unverified"
  | "ownership_unresolved"
  | "promotion_guard_blocked"
  | "promotion_ambiguous"
  | "state_publication_unresolved"
  | "restoration_unverified";

export type ProviderRecoveryResult = Readonly<{
  status: "pending" | "rolled_back";
  reason: ProviderRecoveryPendingReason | "rollback_completed";
  state_commit_sha: string;
}>;

export type ProviderRecoveryInput = Readonly<{
  app: SentinelProviderAppName;
  transactionId: string;
  expectedCommitSha: string;
  executor: SentinelProviderExecutorV1;
}>;

/** The exact GitHubSentinelProviderState snapshot surface the operation uses. */
export type ProviderRecoveryStateSnapshot = Readonly<{
  document: SentinelProviderStateDocumentV1 | null;
  commit_sha: string;
  tree_sha: string;
  state_ref_exists: boolean;
}>;

/** Structurally the createGitHubSentinelProviderState adapter surface. */
export interface ProviderRecoveryState {
  readSnapshot(): ProviderRecoveryStateSnapshot;
  refresh(): Promise<ProviderRecoveryStateSnapshot>;
  compareAndSet(expectedCommitSha: string, nextDocument: unknown): Promise<boolean>;
}

export type ProviderRecoveryDependencies = Readonly<{
  state: ProviderRecoveryState;
  deno: Pick<DenoDeployClient, "verifyHealthIdentity" | "readProductionRouteOwnership" | "promoteRevision">;
  githubToken: string;
  githubFetch?: typeof fetch;
  now: () => number;
  withPromotionLock: <T>(action: () => Promise<T>) => Promise<T>;
  verifyPreviousControl: (
    app: SentinelProviderAppName,
    previous: SentinelProviderAttestationV1,
  ) => Promise<SentinelProviderAttestationV1 | null>;
  verifyRestoration: (
    app: SentinelProviderAppName,
    previous: SentinelProviderAttestationV1,
  ) => Promise<SentinelProviderAttestationV1 | null>;
  retainEvidence: (event: Readonly<Record<string, unknown>>) => Promise<string>;
}>;

/** Fixed safe error for invalid local input or a malformed durable document. */
export class ProviderRecoveryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ProviderRecoveryError";
  }
}

/** Internal guard rejection carrying the fixed pending reason for the abort. */
class ProviderRecoveryGuardError extends Error {
  readonly reason: ProviderRecoveryPendingReason;
  readonly commitSha: string;

  constructor(reason: ProviderRecoveryPendingReason, commitSha: string) {
    super("provider recovery guard rejected");
    this.name = "ProviderRecoveryGuardError";
    this.reason = reason;
    this.commitSha = commitSha;
  }
}

const pendingResult = (reason: ProviderRecoveryPendingReason, stateCommitSha: string): ProviderRecoveryResult =>
  Object.freeze({ status: "pending", reason, state_commit_sha: stateCommitSha });

const rolledBackResult = (stateCommitSha: string): ProviderRecoveryResult =>
  Object.freeze({ status: "rolled_back", reason: "rollback_completed", state_commit_sha: stateCommitSha });

const validateMillisecondsClock = (value: number, label: string): string => {
  if (!Number.isFinite(value)) throw new ProviderRecoveryError(`${label} clock is not a finite timestamp`);
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) throw new ProviderRecoveryError(`${label} clock is not a valid UTC instant`);
  const normalized = parsed.toISOString();
  if (Date.parse(normalized) !== value) throw new ProviderRecoveryError(`${label} clock is not a valid UTC instant`);
  return normalized;
};

const validateCanonicalTime = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !ISO_UTC.test(value)) {
    throw new ProviderRecoveryError(`${label} is not a canonical UTC timestamp`);
  }
  const parsed = Date.parse(value);
  if (!Number.isFinite(parsed) || new Date(parsed).toISOString() !== value) {
    throw new ProviderRecoveryError(`${label} is not a canonical UTC timestamp`);
  }
  return value;
};

const validateInput = (input: ProviderRecoveryInput): void => {
  if (input.app !== "ai-ubq-fi" && input.app !== "p-ai-ubq-fi") {
    throw new ProviderRecoveryError("provider recovery app is not allowlisted");
  }
  if (typeof input.expectedCommitSha !== "string" || !FULL_GIT_SHA.test(input.expectedCommitSha)) {
    throw new ProviderRecoveryError("provider recovery expected commit SHA is invalid");
  }
  if (typeof input.transactionId !== "string" || !TRANSACTION_ID.test(input.transactionId)) {
    throw new ProviderRecoveryError("provider recovery transaction id is invalid");
  }
  if (parseProviderExecutorIdentity(input.executor) === null) {
    throw new ProviderRecoveryError("provider recovery executor identity is not approved");
  }
};

/**
 * Validates a returned attestation against the existing provider schema by
 * placing it in a parsed document copy as an initialized healthy attestation.
 * Fields, SHA/digest/revision formats, canonical verified_at, and both
 * bounded refs are validated by the parser; the copy is never persisted and
 * never fabricates attestation from health alone.
 */
const parseAttestationShape = (
  attestation: SentinelProviderAttestationV1,
  app: SentinelProviderAppName,
): SentinelProviderAttestationV1 | null => {
  try {
    const parsed = parseSentinelProviderStateDocument({
      schema_version: 1,
      generation: 1,
      applications: [{ app, healthy: attestation, transaction: null }],
    });
    return parsed.applications[0]!.healthy;
  } catch {
    return null;
  }
};

export const recoverProviderTransaction = async (
  input: ProviderRecoveryInput,
  dependencies: ProviderRecoveryDependencies,
): Promise<ProviderRecoveryResult> => {
  validateInput(input);

  // Capture a valid operation start clock before any remote read; every later
  // sample must be valid and never regress.
  let lastClockMs: number | null = null;
  const clock = (): string => {
    const milliseconds = dependencies.now();
    const normalized = validateMillisecondsClock(milliseconds, "provider recovery");
    if (lastClockMs !== null && milliseconds < lastClockMs) {
      throw new ProviderRecoveryError("provider recovery clock regressed");
    }
    lastClockMs = milliseconds;
    return normalized;
  };
  const startIso = clock();

  // Refresh the store and parse the durable document anew.
  const reparseSnapshot = (raw: ProviderRecoveryStateSnapshot): ProviderRecoveryStateSnapshot | null => {
    if (raw.document === null) return null;
    try {
      return Object.freeze({
        document: parseSentinelProviderStateDocument(raw.document),
        commit_sha: raw.commit_sha,
        tree_sha: raw.tree_sha,
        state_ref_exists: raw.state_ref_exists,
      });
    } catch {
      return null;
    }
  };
  let snapshot: ProviderRecoveryStateSnapshot;
  try {
    const raw = await dependencies.state.refresh();
    const reparsed = reparseSnapshot(raw);
    if (reparsed === null || reparsed.document === null) {
      throw new ProviderRecoveryError("provider recovery state snapshot is malformed or unavailable");
    }
    snapshot = reparsed;
  } catch (error) {
    if (error instanceof ProviderRecoveryError) throw error;
    throw new ProviderRecoveryError("provider recovery state snapshot is malformed or unavailable");
  }
  if (snapshot.commit_sha !== input.expectedCommitSha) {
    // A moved state ref is a conflict for fresh reconciliation, never a rebase
    // onto a different transaction.
    return pendingResult("state_conflict", snapshot.commit_sha);
  }
  const document = snapshot.document!;
  const appState = document.applications.find((entry) => entry.app === input.app);
  const transaction = appState?.transaction ?? null;
  if (transaction === null) {
    throw new ProviderRecoveryError("provider recovery transaction is malformed or unavailable");
  }
  if (transaction.id !== input.transactionId) {
    // The selected transaction identity must be exact; a different durable
    // transaction is never rebased onto this invocation.
    return pendingResult("state_conflict", snapshot.commit_sha);
  }
  if (isTerminalSentinelProviderPhase(transaction.phase)) {
    return pendingResult("rollback_not_authorized", snapshot.commit_sha);
  }
  if (!executorEquals(transaction.executor, input.executor)) {
    return pendingResult("state_conflict", snapshot.commit_sha);
  }
  if (
    transaction.decision !== "rollback" || transaction.rollback_intent_at === null ||
    (transaction.phase !== "rollback_pending" && transaction.phase !== "rollback_pending_verification")
  ) {
    return pendingResult("rollback_not_authorized", snapshot.commit_sha);
  }
  const isFirstPostPending = transaction.phase === "rollback_pending";
  const previous = transaction.previous;
  if (!isFirstPostPending && transaction.rollback_result?.kind !== "acknowledged") {
    // The parser already bounds this, but a real 204 is a hard requirement
    // before any restoration.
    return pendingResult("state_conflict", snapshot.commit_sha);
  }

  const refreshState = async (): Promise<ProviderRecoveryStateSnapshot | null> => {
    try {
      return reparseSnapshot(await dependencies.state.refresh());
    } catch {
      return null;
    }
  };

  const exactState = (
    fresh: ProviderRecoveryStateSnapshot,
    expected: Readonly<{
      commit_sha: string;
      generation: number;
      transaction: SentinelProviderTransactionV1;
    }>,
  ): ProviderRecoveryResult | null => {
    if (
      fresh.document === null || fresh.commit_sha !== expected.commit_sha ||
      fresh.document.generation !== expected.generation
    ) {
      return pendingResult("state_conflict", fresh.commit_sha);
    }
    const freshTransaction = fresh.document.applications.find((entry) => entry.app === input.app)
      ?.transaction ?? null;
    if (freshTransaction === null || JSON.stringify(freshTransaction) !== JSON.stringify(expected.transaction)) {
      return pendingResult("state_conflict", fresh.commit_sha);
    }
    return null;
  };

  const activeAuthority = async (): Promise<VerifiedActiveExecutorAuthority | null> => {
    try {
      return await verifyActiveProviderExecutorAuthority({
        token: dependencies.githubToken,
        fetcher: dependencies.githubFetch ?? fetch,
        executor: input.executor,
        // Route the verifier's clock through the operation's monotonic
        // validator so a verifier clock sample can never bypass regression
        // or validity enforcement.
        now: () => Date.parse(clock()),
      });
    } catch (error) {
      if (error instanceof ExecutorAuthorityError) return null;
      throw error;
    }
  };

  const readRoute = async (revisionId: string): Promise<
    Readonly<{
      route: ProductionRouteOwnership;
      observedAt: string;
    }> | null
  > => {
    try {
      const route = await dependencies.deno.readProductionRouteOwnership(input.app, revisionId);
      const observedAt = validateCanonicalTime(route.observedAt, "provider recovery route observation");
      const nowIso = clock();
      if (
        route.app !== input.app || route.revisionId !== revisionId ||
        route.managedHostname !== `${input.app}.${ROUTE_ORGANIZATION}.deno.net` ||
        typeof route.ownsRoute !== "boolean" ||
        observedAt < startIso || observedAt > nowIso
      ) {
        return null;
      }
      return Object.freeze({ route, observedAt });
    } catch {
      return null;
    }
  };

  const routeOwner = (
    candidate: Readonly<{ route: ProductionRouteOwnership; observedAt: string }>,
    previousOwn: Readonly<{ route: ProductionRouteOwnership; observedAt: string }>,
  ): "candidate" | "previous" | null => {
    if (candidate.route.ownsRoute && !previousOwn.route.ownsRoute) return "candidate";
    if (previousOwn.route.ownsRoute && !candidate.route.ownsRoute) return "previous";
    return null;
  };

  const retain = async (event: Readonly<Record<string, unknown>>): Promise<string | null> => {
    try {
      const reference = await dependencies.retainEvidence(event);
      if (typeof reference !== "string" || reference.length === 0 || reference.length > 512) return null;
      return reference;
    } catch {
      return null;
    }
  };

  /**
   * Verifies the exact previous control-plane attestation outside the lock.
   * The previous immutable health is required only for rollback_pending (the
   * pre-restoration POST path); candidate/stable health is never required to
   * authorize restoration. Any null/throw/mismatch returns control_unverified.
   */
  const verifyControl = async (): Promise<SentinelProviderAttestationV1 | null> => {
    try {
      if (isFirstPostPending) {
        const url = defaultRevisionHealthUrl(input.app, previous.revision_id, ROUTE_ORGANIZATION);
        const identities = await dependencies.deno.verifyHealthIdentity(
          [url],
          previous.git_sha,
          previous.revision_id,
        );
        const identity = identities[0];
        if (
          identities.length !== 1 || identity === undefined ||
          identity.url !== url || identity.gitSha !== previous.git_sha ||
          identity.revisionId !== previous.revision_id
        ) {
          return null;
        }
      }
      const returned = await dependencies.verifyPreviousControl(input.app, previous);
      if (returned === null) return null;
      if (
        returned.git_sha !== previous.git_sha || returned.revision_id !== previous.revision_id ||
        returned.configuration_digest !== previous.configuration_digest ||
        returned.validator_sha !== previous.validator_sha || returned.corpus_digest !== previous.corpus_digest
      ) {
        return null;
      }
      const parsed = parseAttestationShape(returned, input.app);
      if (parsed === null) return null;
      const nowIso = clock();
      if (parsed.verified_at < startIso || parsed.verified_at > nowIso) return null;
      return parsed;
    } catch {
      return null;
    }
  };

  const control = await verifyControl();
  if (control === null) {
    return pendingResult("control_unverified", snapshot.commit_sha);
  }

  const validateRestoration = async (
    rollbackResult: SentinelProviderPromotionResultV1,
  ): Promise<SentinelProviderAttestationV1 | null> => {
    try {
      const returned = await dependencies.verifyRestoration(input.app, previous);
      if (returned === null) return null;
      if (
        returned.git_sha !== previous.git_sha || returned.revision_id !== previous.revision_id ||
        returned.configuration_digest !== previous.configuration_digest ||
        returned.validator_sha !== previous.validator_sha || returned.corpus_digest !== previous.corpus_digest
      ) {
        return null;
      }
      const parsed = parseAttestationShape(returned, input.app);
      if (parsed === null) return null;
      const nowIso = clock();
      if (
        parsed.verified_at < startIso || parsed.verified_at < rollbackResult.observed_at ||
        parsed.verified_at > nowIso
      ) {
        return null;
      }
      return parsed;
    } catch {
      return null;
    }
  };

  /**
   * Builds the next document from the EXACT current snapshot being updated
   * (never the initially captured document), so an acknowledgment or ambiguous
   * CAS preserves the previously published route evidence and the final CAS
   * preserves both the refreshed route and the acknowledged rollback result.
   */
  const buildDocument = (
    base: SentinelProviderStateDocumentV1,
    patch: Readonly<{
      phase?: SentinelProviderTransactionV1["phase"];
      route?: SentinelProviderTransactionV1["route"];
      rollback_result?: SentinelProviderTransactionV1["rollback_result"];
      restoration?: SentinelProviderAttestationV1 | null;
      healthy?: SentinelProviderAttestationV1 | null;
    }>,
  ): SentinelProviderStateDocumentV1 => {
    const nextGeneration = base.generation + 1;
    return parseSentinelProviderStateDocument({
      schema_version: 1,
      generation: nextGeneration,
      applications: base.applications.map((entry) => {
        if (entry.app !== input.app) return entry;
        return {
          app: entry.app,
          healthy: patch.healthy === undefined ? entry.healthy : patch.healthy,
          transaction: {
            ...entry.transaction!,
            ...(patch.phase === undefined ? {} : { phase: patch.phase }),
            ...(patch.route === undefined ? {} : { route: patch.route }),
            ...(patch.rollback_result === undefined ? {} : { rollback_result: patch.rollback_result }),
            ...(patch.restoration === undefined ? {} : { restoration: patch.restoration }),
          },
        };
      }),
    });
  };

  const publishAndConfirm = async (
    expectedCommitSha: string,
    nextDocument: SentinelProviderStateDocumentV1,
  ): Promise<ProviderRecoveryStateSnapshot | null> => {
    let published: ProviderRecoveryStateSnapshot;
    try {
      if (!(await dependencies.state.compareAndSet(expectedCommitSha, nextDocument))) return null;
      published = dependencies.state.readSnapshot();
    } catch {
      return null;
    }
    const publishedReparsed = reparseSnapshot(published);
    if (
      publishedReparsed === null || publishedReparsed.document === null ||
      JSON.stringify(publishedReparsed.document) !== JSON.stringify(nextDocument)
    ) {
      return null;
    }
    return publishedReparsed;
  };

  const linkedEvent = (
    event: string,
    expectedCommitSha: string,
    authority: VerifiedActiveExecutorAuthority,
    route?: Readonly<{ route: ProductionRouteOwnership; observedAt: string }> | null,
  ): Readonly<Record<string, unknown>> => ({
    schema_version: 1,
    event,
    app: input.app,
    transaction_id: transaction.id,
    fence_generation: transaction.fence_generation,
    expected_commit_sha: expectedCommitSha,
    executor: input.executor,
    authority: {
      observed_at: authority.observed_at,
      attempt: authority.attempt,
    },
    previous_control: control,
    ...(route === undefined || route === null ? {} : {
      route: {
        app: route.route.app,
        revision_id: route.route.revisionId,
        managed_hostname: route.route.managedHostname,
        owns_route: route.route.ownsRoute,
        observed_at: route.observedAt,
      },
    }),
  });

  // -----------------------------------------------------------------------
  // Locked work: exact refresh, active authority, route ownership, the one
  // guarded promotion (rollback_pending only), and the evidence CAS writes.
  // -----------------------------------------------------------------------
  const locked = await dependencies.withPromotionLock(async (): Promise<
    | { kind: "result"; result: ProviderRecoveryResult }
    | {
      kind: "continue";
      snapshot: ProviderRecoveryStateSnapshot;
      rollbackResult: SentinelProviderPromotionResultV1;
      authority: VerifiedActiveExecutorAuthority;
    }
  > => {
    const lockedSnapshot = await refreshState();
    if (lockedSnapshot === null) {
      return { kind: "result", result: pendingResult("state_conflict", snapshot.commit_sha) };
    }
    const conflict = exactState(lockedSnapshot, {
      commit_sha: snapshot.commit_sha,
      generation: document.generation,
      transaction,
    });
    if (conflict !== null) return { kind: "result", result: conflict };

    const authority = await activeAuthority();
    if (authority === null) {
      return { kind: "result", result: pendingResult("promotion_guard_blocked", lockedSnapshot.commit_sha) };
    }

    const candidateRoute = await readRoute(transaction.candidate.revision_id);
    const previousRoute = await readRoute(previous.revision_id);
    if (candidateRoute === null || previousRoute === null) {
      return { kind: "result", result: pendingResult("ownership_unresolved", lockedSnapshot.commit_sha) };
    }
    const owner = routeOwner(candidateRoute, previousRoute);
    if (owner === null) {
      return { kind: "result", result: pendingResult("ownership_unresolved", lockedSnapshot.commit_sha) };
    }

    if (!isFirstPostPending) {
      // rollback_pending_verification: the promotion was already acknowledged
      // by a real 204 in a prior invocation; never POST again here.
      if (owner !== "previous") {
        return { kind: "result", result: pendingResult("ownership_unresolved", lockedSnapshot.commit_sha) };
      }
      if (transaction.rollback_result === null || transaction.rollback_result.kind !== "acknowledged") {
        return { kind: "result", result: pendingResult("state_conflict", lockedSnapshot.commit_sha) };
      }
      return {
        kind: "continue",
        snapshot: lockedSnapshot,
        rollbackResult: transaction.rollback_result,
        authority,
      };
    }

    // rollback_pending: retain the observed route event, persist the refreshed
    // route evidence through the exact-parent CAS (generation + 1, healthy
    // unchanged at previous), and only then touch Deno.
    const observedRoute = owner === "candidate" ? candidateRoute : previousRoute;
    const routeRef = await retain(linkedEvent(
      "provider_recovery_route_observed",
      lockedSnapshot.commit_sha,
      authority,
      observedRoute,
    ));
    if (routeRef === null) {
      return { kind: "result", result: pendingResult("state_publication_unresolved", lockedSnapshot.commit_sha) };
    }
    const routeDocument = buildDocument(lockedSnapshot.document!, {
      route: {
        revision_id: observedRoute.route.revisionId,
        observed_at: observedRoute.observedAt,
        evidence_ref: routeRef,
      },
    });
    const published = await publishAndConfirm(lockedSnapshot.commit_sha, routeDocument);
    if (published === null) {
      return { kind: "result", result: pendingResult("state_publication_unresolved", lockedSnapshot.commit_sha) };
    }
    const publishedTransaction = published.document!.applications.find((entry) => entry.app === input.app)!
      .transaction!;

    // The final guard runs inside the client after its own target reads and
    // directly before the POST; a rejection prevents the POST.
    let guardCompleted = false;
    let guardAuthority: VerifiedActiveExecutorAuthority | null = null;
    const guard = async (): Promise<void> => {
      const guardedAuthority = await activeAuthority();
      if (guardedAuthority === null) {
        throw new ProviderRecoveryGuardError("promotion_guard_blocked", published.commit_sha);
      }
      const guardSnapshot = await refreshState();
      if (guardSnapshot === null) {
        throw new ProviderRecoveryGuardError("state_conflict", published.commit_sha);
      }
      const guardConflict = exactState(guardSnapshot, {
        commit_sha: published.commit_sha,
        generation: published.document!.generation,
        transaction: publishedTransaction,
      });
      if (guardConflict !== null) {
        throw new ProviderRecoveryGuardError("state_conflict", guardSnapshot.commit_sha);
      }
      const guardedCandidate = await readRoute(transaction.candidate.revision_id);
      const guardedPrevious = await readRoute(previous.revision_id);
      if (
        guardedCandidate === null || guardedPrevious === null || routeOwner(guardedCandidate, guardedPrevious) === null
      ) {
        throw new ProviderRecoveryGuardError("ownership_unresolved", guardSnapshot.commit_sha);
      }
      if (Date.parse(clock()) - Date.parse(control.verified_at) > OBSERVATION_CADENCE_MS) {
        throw new ProviderRecoveryGuardError("control_unverified", guardSnapshot.commit_sha);
      }
      guardAuthority = guardedAuthority;
      guardCompleted = true;
    };

    try {
      await dependencies.deno.promoteRevision(input.app, previous.revision_id, guard);
    } catch (error) {
      if (error instanceof ProviderRecoveryGuardError) {
        return { kind: "result", result: pendingResult(error.reason, error.commitSha) };
      }
      if (!guardCompleted || guardAuthority === null) {
        // A guard/pre-POST check failed without a POST result; never fabricate
        // a promotion result for it.
        return { kind: "result", result: pendingResult("promotion_guard_blocked", published.commit_sha) };
      }
      // POST threw after the guard completed: the outcome is ambiguous. An
      // existing ambiguous result is already retained and may not be replaced;
      // otherwise persist the ambiguous result and keep rollback_pending.
      // The guard is the sole closer that assigns guardAuthority; the null
      // guard above short-circuits every failing path before this point.
      const ambiguousAuthority = guardAuthority!;
      const nowIso = clock();
      if (publishedTransaction.rollback_result === null) {
        const ambiguousRef = await retain({
          ...linkedEvent(
            "provider_recovery_rollback_ambiguous",
            published.commit_sha,
            ambiguousAuthority,
          ),
          http_status: null,
          observed_at: nowIso,
        });
        if (ambiguousRef === null) {
          return { kind: "result", result: pendingResult("state_publication_unresolved", published.commit_sha) };
        }
        const ambiguousDocument = buildDocument(published.document!, {
          rollback_result: {
            kind: "ambiguous",
            http_status: null,
            observed_at: nowIso,
            evidence_ref: ambiguousRef,
          },
        });
        const ambiguousPublished = await publishAndConfirm(published.commit_sha, ambiguousDocument);
        if (ambiguousPublished === null) {
          return { kind: "result", result: pendingResult("state_publication_unresolved", published.commit_sha) };
        }
        return { kind: "result", result: pendingResult("promotion_ambiguous", ambiguousPublished.commit_sha) };
      }
      return { kind: "result", result: pendingResult("promotion_ambiguous", published.commit_sha) };
    }

    const nowIso = clock();
    // A successful return must have executed the guard to completion; a
    // missing guard completion means the POST outcome is not trustworthy.
    if (!guardCompleted || guardAuthority === null) {
      return { kind: "result", result: pendingResult("promotion_guard_blocked", published.commit_sha) };
    }
    const acknowledgedAuthority = guardAuthority!;
    const acknowledgementRef = await retain({
      ...linkedEvent(
        "provider_recovery_rollback_acknowledged",
        published.commit_sha,
        acknowledgedAuthority,
      ),
      http_status: 204,
      observed_at: nowIso,
    });
    if (acknowledgementRef === null) {
      return { kind: "result", result: pendingResult("state_publication_unresolved", published.commit_sha) };
    }
    const acknowledgedResult: SentinelProviderPromotionResultV1 = {
      kind: "acknowledged",
      http_status: 204,
      observed_at: nowIso,
      evidence_ref: acknowledgementRef,
    };
    const acknowledgementDocument = buildDocument(published.document!, {
      phase: "rollback_pending_verification",
      rollback_result: acknowledgedResult,
    });
    const acknowledgementPublished = await publishAndConfirm(published.commit_sha, acknowledgementDocument);
    if (acknowledgementPublished === null) {
      return { kind: "result", result: pendingResult("state_publication_unresolved", published.commit_sha) };
    }
    return {
      kind: "continue",
      snapshot: acknowledgementPublished,
      rollbackResult: acknowledgedResult,
      authority: acknowledgedAuthority,
    };
  });

  if (locked.kind === "result") return locked.result;

  // -----------------------------------------------------------------------
  // Lock released: independent frozen authenticated restoration verification.
  // -----------------------------------------------------------------------
  const restoration = await validateRestoration(locked.rollbackResult);
  if (restoration === null) {
    return pendingResult("restoration_unverified", locked.snapshot.commit_sha);
  }

  // -----------------------------------------------------------------------
  // Completion lock: exact snapshot/fence, active executor, exact previous
  // route, evidence retention, and the single final exact-parent CAS.
  // -----------------------------------------------------------------------
  const completed = await dependencies.withPromotionLock(async (): Promise<ProviderRecoveryResult> => {
    const completionSnapshot = await refreshState();
    if (completionSnapshot === null) {
      return pendingResult("state_conflict", locked.snapshot.commit_sha);
    }
    const completionTransaction = locked.snapshot.document!.applications.find((entry) =>
      entry.app === input.app
    )?.transaction ?? null;
    if (completionTransaction === null) {
      return pendingResult("state_conflict", completionSnapshot.commit_sha);
    }
    const conflict = exactState(completionSnapshot, {
      commit_sha: locked.snapshot.commit_sha,
      generation: locked.snapshot.document!.generation,
      transaction: completionTransaction,
    });
    if (conflict !== null) return conflict;

    const authority = await activeAuthority();
    if (authority === null) {
      return pendingResult("promotion_guard_blocked", completionSnapshot.commit_sha);
    }
    const finalCandidate = await readRoute(transaction.candidate.revision_id);
    const finalRoute = await readRoute(previous.revision_id);
    if (finalCandidate === null || finalRoute === null || routeOwner(finalCandidate, finalRoute) !== "previous") {
      return pendingResult("ownership_unresolved", completionSnapshot.commit_sha);
    }
    // Reject stale evidence older than the fixed 30-second observation
    // cadence for both the restoration acceptance and the final route read.
    if (Date.parse(clock()) - Date.parse(restoration.verified_at) > OBSERVATION_CADENCE_MS) {
      return pendingResult("restoration_unverified", completionSnapshot.commit_sha);
    }
    if (Date.parse(clock()) - Date.parse(finalRoute.observedAt) > OBSERVATION_CADENCE_MS) {
      return pendingResult("ownership_unresolved", completionSnapshot.commit_sha);
    }
    const completionRef = await retain({
      ...linkedEvent(
        "provider_recovery_restoration_verified",
        completionSnapshot.commit_sha,
        authority,
        finalRoute,
      ),
      restoration,
    });
    if (completionRef === null) {
      return pendingResult("state_publication_unresolved", completionSnapshot.commit_sha);
    }
    const rolledDocument = buildDocument(completionSnapshot.document!, {
      phase: "rolled_back",
      route: {
        revision_id: previous.revision_id,
        observed_at: finalRoute.observedAt,
        evidence_ref: completionRef,
      },
      restoration,
      healthy: restoration,
    });
    const finished = await publishAndConfirm(completionSnapshot.commit_sha, rolledDocument);
    if (finished === null) {
      return pendingResult("state_publication_unresolved", completionSnapshot.commit_sha);
    }
    return rolledBackResult(finished.commit_sha);
  });
  return completed;
};
