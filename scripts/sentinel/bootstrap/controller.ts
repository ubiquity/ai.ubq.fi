import {
  isAuthoritativeBootstrapFailureClass,
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelBootstrapHealthSignal,
  parseSentinelFailureConstraint,
  type SentinelBootstrapActivationPointerV1,
  type SentinelBootstrapHealthSignalV1,
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
}>;

export type SentinelBootstrapReconcileInput = Readonly<{
  release: unknown;
  signals: readonly unknown[];
  repository?: string;
  ref?: string;
  implementationModel?: string;
  implementationReasoning?: string;
  expectedFence?: Readonly<{ generation: number; activeSha: string }>;
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
}>;

const nowIso = (): string => new Date().toISOString();

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
});

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
  let activation = await activationFromStore(input, dependencies);
  if (activation.pointer.active_sha !== release.stable_sha && activation.pointer.active_sha !== release.candidate_sha) {
    throw new Error("Sentinel bootstrap activation points outside the release record");
  }
  if (input.expectedFence !== undefined) {
    assertBootstrapActivationFence(activation.pointer, input.expectedFence);
  }

  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: activation.pointer.generation,
    signals,
  });
  if (!health.rollback) {
    return outcomeWithoutRollback(
      activation.initialized ? "initialized" : "none",
      activation.pointer,
      stableSha,
      health,
      activation.initialized ? "activation_pointer_initialized" : health.reason,
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
    );
  }
  const now = dependencies.now?.() ?? nowIso();
  const previousActiveSha = activation.pointer.active_sha;
  let rolledBack: SentinelBootstrapActivationPointerV1 | null = null;
  let rollbackCommitted = false;
  for (let attempt = 0; attempt < MAX_STATE_ATTEMPTS; attempt += 1) {
    const next = createRollbackActivation(activation.pointer, release, now);
    if (await dependencies.store.compareAndSetActivation(activation.versionstamp, next)) {
      rolledBack = next;
      rollbackCommitted = true;
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

  const constraint = buildConstraint(health, now);
  const constraintCreated = await dependencies.store.putConstraintIfAbsent(constraint);
  if (constraintCreated && dependencies.publishConstraint !== undefined) {
    await dependencies.publishConstraint(constraint);
  }

  let recoveryDispatched = false;
  if (rollbackCommitted && dependencies.dispatchRecovery !== undefined) {
    const dispatch: SentinelBootstrapRecoveryDispatch = {
      repository: SENTINEL_BOOTSTRAP_POLICY.repository,
      ref: SENTINEL_BOOTSTRAP_POLICY.developmentRef,
      sha: rolledBack.active_sha,
      previous_sha: previousActiveSha,
      fenced_generation: health.generation,
      active_generation: rolledBack.generation,
      constraint,
    };
    assertRecoveryDispatchIdentity(dispatch);
    await dependencies.dispatchRecovery(dispatch);
    recoveryDispatched = true;
  }
  return {
    action: "rolled_back",
    active_sha: rolledBack.active_sha,
    generation: rolledBack.generation,
    fenced_generation: health.generation,
    stable_sha: stableSha,
    health,
    constraint_published: constraintCreated,
    recovery_dispatched: recoveryDispatched,
    reason: "authoritative_failure_rollback",
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
