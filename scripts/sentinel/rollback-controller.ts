import type { HealthIdentity, ProductionHealthAttestation, RollbackTarget } from "./deploy.ts";

/**
 * Objective automatic production rollback controller.
 *
 * Restores exactly the immutable prior Deno revision when a post-merge /
 * post-promotion production acceptance failed for the newly delivered
 * candidate. The controller is deterministic and dependency-injected: it
 * reads no environment variables, defines no flags or secrets, and uses no
 * model. All deployment, promotion, and verification work is performed through
 * the existing primitives:
 *
 * - the exact pre-deploy healthy attestation (`RollbackTarget`) captured
 *   before promotion is the ONLY source of the rollback identity — no revision
 *   is ever selected from list order, creation time, or a "latest" label;
 * - the promotion dependency dispatches the existing serialized
 *   revision-control workflow (concurrency group `ai-ubq-fi-deploy`), which
 *   promotes `POST https://api.deno.com/v2/revisions/<id>/promote` with the
 *   existing Deno organization token and requires HTTP 204, so the rollback
 *   is serialized with every other deployment writer;
 * - the managed health endpoint is verified to return the exact previous full
 *   Git SHA and revision ID in both its body and its response headers
 *   (`verifyHealthIdentity` / `verifyProductionHealthIdentity`);
 * - the custom domain is probed and an identified Cloudflare 403 challenge is
 *   recorded as a warning after the exact managed route passes;
 * - machine-readable rollback evidence is persisted on every path, and the
 *   controller fails closed — rejecting the decision and throwing — whenever
 *   identity or promotion cannot be proven.
 *
 * The controller never rolls back because of Codex review findings: the only
 * accepted failure kinds are post-promotion acceptance failures and the
 * keep/monitoring rollback decision, and any other kind (in particular a
 * review finding) is refused before any network activity.
 */

export type RollbackControllerApp = "ai-ubq-fi" | "p-ai-ubq-fi";

/** Exact failure kinds the controller accepts. Anything else is refused fail-closed. */
export type ProductionRollbackTriggerKind =
  /** Deterministic post-promotion production acceptance failed for the candidate. */
  | "post_promotion_acceptance_failed"
  /** The post-promotion keep/monitoring decision selected rollback. */
  | "post_promotion_monitoring_rollback";

export type ProductionRollbackFailure = Readonly<{
  kind: ProductionRollbackTriggerKind;
  /** Bounded machine-readable cause (stage, error class, or decision label). */
  cause: string;
  /** ISO timestamp the failed acceptance was observed. */
  observed_at: string;
}>;

export type ProductionRollbackInput = Readonly<{
  app: RollbackControllerApp;
  /** The exact newly delivered candidate identity the acceptance expected. */
  candidate: Readonly<{ gitSha: string; revisionId: string }>;
  /** The exact pre-deploy healthy attestation captured before promotion. */
  previous: RollbackTarget;
  /** Exact development tip that must still hold for the serialized promotion. */
  expectedDevelopmentGitSha: string;
  failure: ProductionRollbackFailure;
}>;

export type ProductionRollbackPromotionInput = Readonly<{
  targetGitSha: string;
  targetRevision: string;
  expectedCurrent: RollbackTarget;
  expectedDevelopmentGitSha: string;
}>;

export type ProductionRollbackPromotionResult = Readonly<{ workflowRunId: number | null }>;

export interface ProductionRollbackDenoClient {
  snapshotHealthyProduction(app: string, healthUrls: readonly string[]): Promise<RollbackTarget>;
  verifyHealthIdentity(
    urls: readonly string[],
    expectedGitSha: string,
    expectedRevisionId: string,
  ): Promise<readonly HealthIdentity[]>;
  verifyProductionHealthIdentity(
    managedUrl: string,
    customUrl: string,
    expectedGitSha: string,
    expectedRevisionId: string,
  ): Promise<ProductionHealthAttestation>;
}

export type ProductionRollbackDependencies = Readonly<{
  deno: ProductionRollbackDenoClient;
  /**
   * Serialized promotion through the existing authenticated Deno API path.
   * Must dispatch the revision-control workflow, whose concurrency lock
   * serializes the promotion with other deployment writers.
   */
  promotion: (input: ProductionRollbackPromotionInput) => Promise<ProductionRollbackPromotionResult>;
  /** Persists the machine-readable evidence exactly as produced. */
  persist: (evidence: ProductionRollbackEvidence) => Promise<void>;
  now?: () => number;
}>;

export type ProductionRollbackStatus = "rolled_back" | "already_previous" | "failed";

export type ProductionRollbackClassification =
  | "rollback"
  | "already_previous"
  | "unrelated"
  | "unobservable"
  | "refused";

export type ProductionRollbackManaged = Readonly<{
  url: string;
  git_sha: string;
  revision: string;
}>;

export type ProductionRollbackCustom = Readonly<
  | { kind: "identity"; url: string; git_sha: string; revision: string }
  | { kind: "cloudflare_challenge"; url: string; status: 403; ray: string }
>;

export type ProductionRollbackEvidence = Readonly<{
  schema_version: 1;
  status: ProductionRollbackStatus;
  app: string;
  trigger: ProductionRollbackTriggerKind;
  cause: string;
  failure_observed_at: string;
  candidate: Readonly<{ git_sha: string; revision: string }>;
  previous: Readonly<{
    git_sha: string;
    revision: string;
    health_urls: readonly string[];
    snapshotted_at: string;
  }>;
  /** Exact identity observed on the stable route, or null when unobservable. */
  observed: Readonly<{ git_sha: string; revision: string }> | null;
  classification: ProductionRollbackClassification;
  promotion_workflow_run_id: number | null;
  managed: ProductionRollbackManaged | null;
  custom: ProductionRollbackCustom | null;
  cloudflare_403_warning: boolean;
  cloudflare_ray: string | null;
  completed_at: string;
  error_class: string | null;
  error: string | null;
}>;

export class ProductionRollbackError extends Error {
  readonly evidence: ProductionRollbackEvidence;

  constructor(message: string, evidence: ProductionRollbackEvidence, options?: ErrorOptions) {
    super(message, options);
    this.name = "ProductionRollbackError";
    this.evidence = evidence;
  }
}

const FULL_GIT_SHA = /^[0-9a-f]{40}$/u;
const REVISION_ID = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/u;
const MAX_CAUSE_LENGTH = 512;
const MAX_ERROR_LENGTH = 2_048;
const PRODUCTION_APP = "ai-ubq-fi";
const PREVIEW_APP = "p-ai-ubq-fi";
const ACCEPTED_TRIGGER_KINDS = new Set<ProductionRollbackTriggerKind>([
  "post_promotion_acceptance_failed",
  "post_promotion_monitoring_rollback",
]);
const REFUSED_TRIGGER_KINDS = new Set<string>(["codex_review_finding", "codex_review_findings"]);

const requireFullGitSha = (value: string, label: string): void => {
  if (!FULL_GIT_SHA.test(value)) throw new Error(`${label} must be a lowercase, full Git commit SHA`);
};

const requireRevisionId = (value: string, label: string): string => {
  if (!REVISION_ID.test(value)) throw new Error(`${label} must be a safe Deno revision ID`);
  return value;
};

const validateInput = (input: ProductionRollbackInput): void => {
  if (input.app !== PRODUCTION_APP && input.app !== PREVIEW_APP) {
    throw new Error("Production rollback application is invalid");
  }
  requireFullGitSha(input.candidate.gitSha, "Candidate Git SHA");
  requireRevisionId(input.candidate.revisionId, "Candidate revision");
  requireFullGitSha(input.previous.gitSha, "Previous Git SHA");
  requireRevisionId(input.previous.revisionId, "Previous revision");
  if (
    input.previous.gitSha === input.candidate.gitSha ||
    input.previous.revisionId === input.candidate.revisionId
  ) {
    throw new Error("The pre-deploy rollback attestation does not differ from the delivered candidate");
  }
  if (input.previous.healthUrls.length === 0 || input.previous.healthUrls.length > 2) {
    throw new Error("The pre-deploy rollback attestation must carry one or two exact health URLs");
  }
  for (const url of input.previous.healthUrls) {
    if (!/^https:\/\/[A-Za-z0-9.-]+(?::[0-9]{1,5})?\/[^ ]{0,511}$/u.test(url)) {
      throw new Error("The pre-deploy rollback attestation carries an unsafe health URL");
    }
  }
  if (!Number.isFinite(Date.parse(input.previous.snapshottedAt))) {
    throw new Error("The pre-deploy rollback attestation timestamp is invalid");
  }
  requireFullGitSha(input.expectedDevelopmentGitSha, "Expected development SHA");
  if (REFUSED_TRIGGER_KINDS.has(input.failure.kind)) {
    throw new Error(
      `Production rollback refuses the trigger ${input.failure.kind}: Codex review findings never roll back`,
    );
  }
  if (!ACCEPTED_TRIGGER_KINDS.has(input.failure.kind)) {
    throw new Error(`Production rollback refuses the trigger ${input.failure.kind}`);
  }
  if (
    input.failure.cause.trim().length === 0 || input.failure.cause.length > MAX_CAUSE_LENGTH ||
    !/^[A-Za-z0-9_.:-]+$/u.test(input.failure.cause)
  ) {
    throw new Error("Production rollback failure cause must be a bounded machine-readable label");
  }
  const observedAt = Date.parse(input.failure.observed_at);
  if (!Number.isFinite(observedAt) || observedAt <= 0) {
    throw new Error("Production rollback failure observation time is invalid");
  }
};

const managedEvidence = (identity: HealthIdentity | undefined): ProductionRollbackManaged | null =>
  identity ? Object.freeze({ url: identity.url, git_sha: identity.gitSha, revision: identity.revisionId }) : null;

const customEvidence = (attestation: ProductionHealthAttestation["custom"]): ProductionRollbackCustom =>
  attestation.kind === "identity"
    ? Object.freeze({
      kind: "identity" as const,
      url: attestation.identity.url,
      git_sha: attestation.identity.gitSha,
      revision: attestation.identity.revisionId,
    })
    : Object.freeze({
      kind: "cloudflare_challenge" as const,
      url: attestation.url,
      status: attestation.status,
      ray: attestation.ray,
    });

/**
 * Verifies that the managed endpoint reports the exact previous full Git SHA
 * and revision ID in body and headers, probes the custom endpoint, and applies
 * the existing Cloudflare-403 warning policy (warning, never a rollback
 * failure, once the managed route proves the exact prior identity).
 */
const verifyPreviousIdentity = async (
  deno: ProductionRollbackDenoClient,
  previous: RollbackTarget,
): Promise<
  Readonly<{ managed: ProductionRollbackManaged; custom: ProductionRollbackCustom | null; cloudflare: boolean }>
> => {
  if (previous.healthUrls.length === 2) {
    const attestation = await deno.verifyProductionHealthIdentity(
      previous.healthUrls[0]!,
      previous.healthUrls[1]!,
      previous.gitSha,
      previous.revisionId,
    );
    const managed = managedEvidence(attestation.managed);
    if (managed === null) throw new Error("The managed health identity could not be proven");
    return {
      managed,
      custom: customEvidence(attestation.custom),
      cloudflare: attestation.custom.kind === "cloudflare_challenge",
    };
  }
  const identities = await deno.verifyHealthIdentity(previous.healthUrls, previous.gitSha, previous.revisionId);
  const managed = managedEvidence(identities[0]);
  if (managed === null) throw new Error("The managed health identity could not be proven");
  return { managed, custom: null, cloudflare: false };
};

/** Mirrors the order of the existing health URLs: managed first, custom second. */
const evidenceHealthUrls = (previous: RollbackTarget): readonly string[] => Object.freeze([...previous.healthUrls]);

const failureEvidence = (
  input: ProductionRollbackInput,
  classification: ProductionRollbackClassification,
  observed: Readonly<{ gitSha: string; revisionId: string }> | null,
  promotionWorkflowRunId: number | null,
  error: unknown,
  now: string,
): ProductionRollbackEvidence => ({
  schema_version: 1,
  status: "failed",
  app: input.app,
  trigger: input.failure.kind,
  cause: input.failure.cause,
  failure_observed_at: input.failure.observed_at,
  candidate: Object.freeze({ git_sha: input.candidate.gitSha, revision: input.candidate.revisionId }),
  previous: Object.freeze({
    git_sha: input.previous.gitSha,
    revision: input.previous.revisionId,
    health_urls: evidenceHealthUrls(input.previous),
    snapshotted_at: input.previous.snapshottedAt,
  }),
  observed: observed === null ? null : Object.freeze({ git_sha: observed.gitSha, revision: observed.revisionId }),
  classification,
  promotion_workflow_run_id: promotionWorkflowRunId,
  managed: null,
  custom: null,
  cloudflare_403_warning: false,
  cloudflare_ray: null,
  completed_at: now,
  error_class: error instanceof Error ? error.name : "unknown",
  error: (error instanceof Error ? error.message : String(error)).slice(0, MAX_ERROR_LENGTH),
});

/**
 * Executes the objective automatic rollback. Promotes only the exact immutable
 * prior revision from the pre-deploy attestation, verifies the exact prior
 * identity on the managed (body and headers) and custom routes, persists
 * machine-readable evidence, and throws (fail closed) whenever identity or
 * promotion cannot be proven. Callers must never treat a thrown outcome as a
 * rollback.
 */
export const executeProductionRollback = async (
  input: ProductionRollbackInput,
  dependencies: ProductionRollbackDependencies,
): Promise<ProductionRollbackEvidence> => {
  const now = dependencies.now ?? Date.now;
  let classification: ProductionRollbackClassification = "refused";
  let observed: Readonly<{ gitSha: string; revisionId: string }> | null = null;
  let promotionWorkflowRunId: number | null = null;
  try {
    validateInput(input);
    let observedTarget: RollbackTarget;
    try {
      observedTarget = await dependencies.deno.snapshotHealthyProduction(input.app, input.previous.healthUrls);
    } catch (error) {
      classification = "unobservable";
      throw error;
    }
    observed = Object.freeze({ gitSha: observedTarget.gitSha, revisionId: observedTarget.revisionId });
    const observedIsPrevious = observed.gitSha === input.previous.gitSha &&
      observed.revisionId === input.previous.revisionId;
    const observedIsCandidate = observed.gitSha === input.candidate.gitSha &&
      observed.revisionId === input.candidate.revisionId;
    if (observedIsPrevious) {
      classification = "already_previous";
    } else if (observedIsCandidate) {
      classification = "rollback";
    } else {
      classification = "unrelated";
      throw new Error(
        "Production identity is unrelated to both the delivered candidate and the pre-deploy attestation; " +
          "refusing to promote any revision",
      );
    }
    if (classification === "rollback") {
      const promotion = await dependencies.promotion({
        targetGitSha: input.previous.gitSha,
        // The only allowed promotion target is the exact immutable prior
        // revision from the pre-deploy attestation — never list order, time,
        // or a "latest" label.
        targetRevision: input.previous.revisionId,
        expectedCurrent: observedTarget,
        expectedDevelopmentGitSha: input.expectedDevelopmentGitSha,
      });
      promotionWorkflowRunId = promotion.workflowRunId;
    }
    const verified = await verifyPreviousIdentity(dependencies.deno, input.previous);
    const completedAt = new Date(now()).toISOString();
    const evidence: ProductionRollbackEvidence = {
      schema_version: 1,
      status: classification === "rollback" ? "rolled_back" : "already_previous",
      app: input.app,
      trigger: input.failure.kind,
      cause: input.failure.cause,
      failure_observed_at: input.failure.observed_at,
      candidate: Object.freeze({ git_sha: input.candidate.gitSha, revision: input.candidate.revisionId }),
      previous: Object.freeze({
        git_sha: input.previous.gitSha,
        revision: input.previous.revisionId,
        health_urls: evidenceHealthUrls(input.previous),
        snapshotted_at: input.previous.snapshottedAt,
      }),
      observed: Object.freeze({ git_sha: observed.gitSha, revision: observed.revisionId }),
      classification,
      promotion_workflow_run_id: promotionWorkflowRunId,
      managed: verified.managed,
      custom: verified.custom,
      cloudflare_403_warning: verified.cloudflare,
      cloudflare_ray: verified.cloudflare && verified.custom?.kind === "cloudflare_challenge"
        ? verified.custom.ray
        : null,
      completed_at: completedAt,
      error_class: null,
      error: null,
    };
    await dependencies.persist(evidence);
    return evidence;
  } catch (error) {
    const completedAt = new Date(now()).toISOString();
    const evidence = failureEvidence(input, classification, observed, promotionWorkflowRunId, error, completedAt);
    await dependencies.persist(evidence);
    throw new ProductionRollbackError(
      `Production rollback did not converge: ${error instanceof Error ? error.message : String(error)}`,
      evidence,
      { cause: error },
    );
  }
};
