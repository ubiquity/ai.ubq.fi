import {
  isAuthoritativeBootstrapFailureClass,
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelBootstrapHealthSignal,
  parseSentinelBootstrapProgressObservation,
  parseSentinelBootstrapRollbackIntent,
  parseSentinelFailureConstraint,
  type SentinelBootstrapActivationPointerV1,
  type SentinelBootstrapClassifierEvidenceV1,
  type SentinelBootstrapHealthSignalV1,
  type SentinelBootstrapProgressDecisionV1,
  type SentinelBootstrapProgressObservationV1,
  type SentinelBootstrapRollbackIntentV1,
  type SentinelFailureConstraintV1,
} from "./contracts.ts";
import {
  createDenoKvBootstrapStateStore,
  createRollbackActivation,
  initialSentinelBootstrapActivation,
  selectStableRollbackSha,
  SentinelBootstrapStateConflict,
  type SentinelBootstrapStateStore,
} from "./activation.ts";
import { evaluateSentinelBootstrapHealth, type SentinelBootstrapHealthDecision } from "./health.ts";
import {
  classifierFailureEvidence,
  evaluateSentinelBootstrapProgress,
  resolveSentinelBootstrapProgress,
} from "./progress.ts";
import {
  assertBootstrapActivationFence,
  assertImplementationSelection,
  assertRecoveryDispatchIdentity,
  SENTINEL_BOOTSTRAP_POLICY,
} from "./policy.ts";

const MAX_STATE_ATTEMPTS = 4;

export type SentinelBootstrapRecoveryDispatch = Readonly<{
  repository: string;
  ref: string;
  sha: string;
  previous_sha: string;
  fenced_generation: number;
  active_generation: number;
  constraint: SentinelFailureConstraintV1;
}>;

export type SentinelBootstrapControllerDependencies = Readonly<{
  store: SentinelBootstrapStateStore;
  now?: () => string;
  publishConstraint?: (constraint: SentinelFailureConstraintV1) => Promise<void>;
  dispatchRecovery?: (dispatch: SentinelBootstrapRecoveryDispatch) => Promise<void>;
  /**
   * One injected zero-tool GPT-OSS classifier consulted only when the
   * deterministic verdict is `ambiguous`. Its evidence is advisory; any
   * failure becomes `unknown` and never affects rollback or identity gates.
   */
  classifyAmbiguous?: (observation: SentinelBootstrapProgressObservationV1) => Promise<
    SentinelBootstrapClassifierEvidenceV1
  >;
}>;

export type SentinelBootstrapReconcileInput = Readonly<{
  release: unknown;
  signals: readonly unknown[];
  repository?: string;
  ref?: string;
  implementationModel?: string;
  implementationReasoning?: string;
  expectedFence?: Readonly<{ generation: number; activeSha: string }>;
  /**
   * Canonical per-run progress observations, newest last. Absent means the
   * caller does not evaluate progress; malformed input fails closed to
   * `progress: null` without blocking health/rollback.
   */
  progressObservations?: readonly unknown[];
}>;

export type SentinelBootstrapReconcileOutcome = Readonly<{
  action: "initialized" | "none" | "rolled_back" | "blocked";
  active_sha: string;
  generation: number;
  fenced_generation: number | null;
  stable_sha: string;
  health: SentinelBootstrapHealthDecision;
  constraint_published: boolean;
  recovery_dispatched: boolean;
  reason: string;
  /**
   * Advisory bootstrap progress decision (null when no observations were
   * supplied or they were invalid). The current promotion path does not
   * consume this field. A later authority change requires live classifier
   * acceptance and must preserve the existing exact-SHA/revision gates.
   */
  progress: SentinelBootstrapProgressDecisionV1 | null;
}>;

const nowIso = (): string => new Date().toISOString();

type CompletedRollbackEffects = Readonly<{
  completed: boolean;
  constraintPublished: boolean;
  recoveryDispatched: boolean;
}>;

const completePendingRollbackEffects = async (
  pointer: SentinelBootstrapActivationPointerV1,
  dependencies: SentinelBootstrapControllerDependencies,
): Promise<CompletedRollbackEffects> => {
  const snapshot = await dependencies.store.readRollbackIntent();
  if (snapshot.intent === null) {
    return { completed: false, constraintPublished: false, recoveryDispatched: false };
  }
  if (snapshot.versionstamp === null) throw new Error("Sentinel bootstrap rollback intent is not durable");
  const intent = parseSentinelBootstrapRollbackIntent(snapshot.intent);
  if (intent.target_sha !== pointer.active_sha || intent.active_generation !== pointer.generation) {
    throw new SentinelBootstrapStateConflict("Sentinel bootstrap rollback intent does not match activation");
  }
  await dependencies.store.putConstraintIfAbsent(intent.constraint);
  if (dependencies.publishConstraint !== undefined) await dependencies.publishConstraint(intent.constraint);
  let recoveryDispatched = false;
  if (dependencies.dispatchRecovery !== undefined) {
    const dispatch: SentinelBootstrapRecoveryDispatch = {
      repository: SENTINEL_BOOTSTRAP_POLICY.repository,
      ref: SENTINEL_BOOTSTRAP_POLICY.developmentRef,
      sha: intent.target_sha,
      previous_sha: intent.previous_sha,
      fenced_generation: intent.fenced_generation,
      active_generation: intent.active_generation,
      constraint: intent.constraint,
    };
    assertRecoveryDispatchIdentity(dispatch);
    await dependencies.dispatchRecovery(dispatch);
    recoveryDispatched = true;
  }
  if (!await dependencies.store.clearRollbackIntent(snapshot.versionstamp)) {
    throw new SentinelBootstrapStateConflict("Sentinel bootstrap rollback intent changed before completion");
  }
  return { completed: true, constraintPublished: true, recoveryDispatched };
};

const buildConstraint = (
  decision: SentinelBootstrapHealthDecision,
  now: string,
): SentinelFailureConstraintV1 => {
  const failureClass = decision.failure_class;
  if (
    failureClass === null || decision.failure_fingerprint === null ||
    !isAuthoritativeBootstrapFailureClass(failureClass)
  ) throw new Error("An unhealthy Sentinel bootstrap decision has no authoritative fingerprint");
  const constraint = {
    schema_version: 1,
    fenced_generation: decision.generation,
    failure_class: failureClass,
    failure_fingerprint: decision.failure_fingerprint,
    violated_invariant: "bootstrap_authoritative_failure_must_fence_active_generation",
    evidence_refs: decision.evidence_refs,
    regression_test: "tests/sentinel-bootstrap.test.ts",
    created_at: now,
  } satisfies SentinelFailureConstraintV1;
  return parseSentinelFailureConstraint(constraint);
};

const validateInputIdentity = (input: SentinelBootstrapReconcileInput): void => {
  if (input.repository !== undefined || input.ref !== undefined) {
    assertRecoveryDispatchIdentity({
      repository: input.repository ?? SENTINEL_BOOTSTRAP_POLICY.repository,
      ref: input.ref ?? SENTINEL_BOOTSTRAP_POLICY.developmentRef,
      sha: "0".repeat(40),
    });
  }
  if (input.implementationModel !== undefined || input.implementationReasoning !== undefined) {
    assertImplementationSelection(
      input.implementationModel ?? SENTINEL_BOOTSTRAP_POLICY.implementationModel,
      input.implementationReasoning ?? SENTINEL_BOOTSTRAP_POLICY.implementationReasoning,
    );
  }
};

const activationFromStore = async (
  input: SentinelBootstrapReconcileInput,
  dependencies: SentinelBootstrapControllerDependencies,
): Promise<
  Readonly<{ pointer: SentinelBootstrapActivationPointerV1; versionstamp: string | null; initialized: boolean }>
> => {
  const current = await dependencies.store.readActivation();
  if (current.pointer !== null) {
    return {
      pointer: parseSentinelBootstrapActivationPointer(current.pointer),
      versionstamp: current.versionstamp,
      initialized: false,
    };
  }
  const initial = initialSentinelBootstrapActivation(input.release, dependencies.now?.() ?? nowIso());
  if (await dependencies.store.compareAndSetActivation(null, initial)) {
    const created = await dependencies.store.readActivation();
    if (created.pointer === null) throw new Error("Sentinel bootstrap activation initialization was not durable");
    return {
      pointer: parseSentinelBootstrapActivationPointer(created.pointer),
      versionstamp: created.versionstamp,
      initialized: true,
    };
  }
  const raced = await dependencies.store.readActivation();
  if (raced.pointer === null) throw new Error("Sentinel bootstrap activation initialization conflicted");
  return {
    pointer: parseSentinelBootstrapActivationPointer(raced.pointer),
    versionstamp: raced.versionstamp,
    initialized: false,
  };
};

const outcomeWithoutRollback = (
  action: "initialized" | "none" | "blocked",
  pointer: SentinelBootstrapActivationPointerV1,
  stableSha: string,
  health: SentinelBootstrapHealthDecision,
  reason: string,
  progress: SentinelBootstrapProgressDecisionV1 | null,
): SentinelBootstrapReconcileOutcome => ({
  action,
  active_sha: pointer.active_sha,
  generation: pointer.generation,
  fenced_generation: null,
  stable_sha: stableSha,
  health,
  constraint_published: false,
  recovery_dispatched: false,
  reason,
  progress,
});

/**
 * Evaluates the advisory progress decision. The deterministic verdict is
 * pure; the classifier is called exactly once, only for `ambiguous`, and any
 * failure degrades to `unknown` evidence instead of propagating. Malformed
 * observations fail closed to no progress evidence without blocking the
 * controller's authoritative health/rollback work.
 */
const evaluateProgress = async (
  input: SentinelBootstrapReconcileInput,
  dependencies: SentinelBootstrapControllerDependencies,
  now: string,
): Promise<SentinelBootstrapProgressDecisionV1 | null> => {
  if (input.progressObservations === undefined) return null;
  try {
    const observations = input.progressObservations.map(parseSentinelBootstrapProgressObservation);
    const decision = evaluateSentinelBootstrapProgress(observations, now);
    if (decision.verdict !== "ambiguous" || dependencies.classifyAmbiguous === undefined) {
      return resolveSentinelBootstrapProgress(decision, null);
    }
    const current = observations[observations.length - 1]!;
    try {
      return resolveSentinelBootstrapProgress(decision, await dependencies.classifyAmbiguous(current));
    } catch {
      return resolveSentinelBootstrapProgress(
        decision,
        classifierFailureEvidence(current, "classifier_injected_call_failed", now),
      );
    }
  } catch {
    // Invalid or missing progress input is fail-closed: no progress evidence,
    // rollback and identity decisions remain unaffected.
    return null;
  }
};

/**
 * Reconcile one bootstrap observation set. This controller has no Git,
 * deployment, merge, or policy-writing capability. Its only mutation is a
 * compare-and-swap of the activation pointer and a deduplicated constraint.
 */
export const reconcileSentinelBootstrap = async (
  input: SentinelBootstrapReconcileInput,
  dependencies: SentinelBootstrapControllerDependencies,
): Promise<SentinelBootstrapReconcileOutcome> => {
  validateInputIdentity(input);
  const release = parseBootstrapReleaseRecord(input.release);
  const signals: readonly SentinelBootstrapHealthSignalV1[] = input.signals.map(parseSentinelBootstrapHealthSignal);
  const stableSha = release.stable_sha;
  const now = dependencies.now?.() ?? nowIso();
  let activation = await activationFromStore(input, dependencies);
  if (activation.pointer.active_sha !== release.stable_sha && activation.pointer.active_sha !== release.candidate_sha) {
    throw new Error("Sentinel bootstrap activation points outside the release record");
  }
  if (input.expectedFence !== undefined) {
    assertBootstrapActivationFence(activation.pointer, input.expectedFence);
  }

  const pendingEffects = await completePendingRollbackEffects(activation.pointer, dependencies);

  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: activation.pointer.generation,
    signals,
  });
  const progress = await evaluateProgress(input, dependencies, now);
  if (!health.rollback) {
    if (pendingEffects.completed) {
      return {
        action: "rolled_back",
        active_sha: activation.pointer.active_sha,
        generation: activation.pointer.generation,
        fenced_generation: activation.pointer.generation - 1,
        stable_sha: stableSha,
        health,
        constraint_published: pendingEffects.constraintPublished,
        recovery_dispatched: pendingEffects.recoveryDispatched,
        reason: "pending_rollback_effects_completed",
        progress,
      };
    }
    return outcomeWithoutRollback(
      activation.initialized ? "initialized" : "none",
      activation.pointer,
      stableSha,
      health,
      activation.initialized ? "activation_pointer_initialized" : health.reason,
      progress,
    );
  }

  const rollbackSha = selectStableRollbackSha(activation.pointer, release);
  if (rollbackSha === null) {
    return outcomeWithoutRollback(
      "blocked",
      activation.pointer,
      stableSha,
      health,
      "active_version_is_already_stable",
      progress,
    );
  }
  const constraint = buildConstraint(health, now);
  let rolledBack: SentinelBootstrapActivationPointerV1 | null = null;
  for (let attempt = 0; attempt < MAX_STATE_ATTEMPTS; attempt += 1) {
    const next = createRollbackActivation(activation.pointer, release, now);
    const intent: SentinelBootstrapRollbackIntentV1 = parseSentinelBootstrapRollbackIntent({
      schema_version: 1,
      previous_sha: activation.pointer.active_sha,
      target_sha: next.active_sha,
      fenced_generation: activation.pointer.generation,
      active_generation: next.generation,
      constraint,
      created_at: now,
    });
    if (await dependencies.store.commitRollback(activation.versionstamp, next, intent)) {
      rolledBack = next;
      break;
    }
    activation = await activationFromStore(input, dependencies);
    if (input.expectedFence !== undefined) assertBootstrapActivationFence(activation.pointer, input.expectedFence);
    if (activation.pointer.active_sha === release.stable_sha) {
      rolledBack = activation.pointer;
      break;
    }
  }
  if (rolledBack === null) {
    throw new SentinelBootstrapStateConflict("Sentinel bootstrap rollback lost its activation fence");
  }

  const effects = await completePendingRollbackEffects(rolledBack, dependencies);
  return {
    action: "rolled_back",
    active_sha: rolledBack.active_sha,
    generation: rolledBack.generation,
    fenced_generation: health.generation,
    stable_sha: stableSha,
    health,
    constraint_published: effects.constraintPublished,
    recovery_dispatched: effects.recoveryDispatched,
    reason: "authoritative_failure_rollback",
    progress,
  };
};

export const reconcileSentinelBootstrapWithKv = (
  input: SentinelBootstrapReconcileInput,
  kv: Deno.Kv,
  options: Omit<SentinelBootstrapControllerDependencies, "store"> = {},
): Promise<SentinelBootstrapReconcileOutcome> =>
  reconcileSentinelBootstrap(input, {
    ...options,
    store: createDenoKvBootstrapStateStore(kv),
  });
