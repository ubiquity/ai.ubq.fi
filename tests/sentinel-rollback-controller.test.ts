import assert from "node:assert/strict";
import type { HealthIdentity, ProductionHealthAttestation, RollbackTarget } from "../scripts/sentinel/deploy.ts";
import {
  executeProductionRollback,
  type ProductionRollbackDenoClient,
  ProductionRollbackError,
  type ProductionRollbackEvidence,
  type ProductionRollbackInput,
  type ProductionRollbackPromotionInput,
} from "../scripts/sentinel/rollback-controller.ts";

const DEVELOPMENT_SHA = "1".repeat(40);
const PREVIOUS_SHA = "2".repeat(40);
const CANDIDATE_SHA = "3".repeat(40);
const PREVIOUS_REVISION = "previous-revision";
const CANDIDATE_REVISION = "candidate-revision";
const UNRELATED_SHA = "4".repeat(40);
const UNRELATED_REVISION = "unrelated-revision";
const MANAGED_URL = "https://ai-ubq-fi.ubiquity-dao.deno.net/health";
const CUSTOM_URL = "https://ai.ubq.fi/health";
const PREVIEW_URL = "https://p-ai-ubq-fi.ubiquity-dao.deno.net/health";

const snapshot = (
  gitSha = PREVIOUS_SHA,
  revisionId = PREVIOUS_REVISION,
  healthUrls: readonly string[] = [MANAGED_URL, CUSTOM_URL],
): RollbackTarget => ({
  gitSha,
  revisionId,
  healthUrls: [...healthUrls],
  snapshottedAt: "2026-08-21T10:00:00.000Z",
});

const previousTarget = snapshot();

const validInput = (partial: Partial<ProductionRollbackInput> = {}): ProductionRollbackInput => ({
  app: "ai-ubq-fi",
  candidate: { gitSha: CANDIDATE_SHA, revisionId: CANDIDATE_REVISION },
  previous: previousTarget,
  expectedDevelopmentGitSha: DEVELOPMENT_SHA,
  failure: {
    kind: "post_promotion_acceptance_failed",
    cause: "fail_safe_after_production_stage_error",
    observed_at: "2026-08-21T10:30:00.000Z",
  },
  ...partial,
});

class FakeRollbackDenoClient implements ProductionRollbackDenoClient {
  readonly snapshots: RollbackTarget[] = [];
  readonly verifyCalls: Array<{ urls: readonly string[]; gitSha: string; revisionId: string }> = [];
  snapshotFailures = 0;
  verifyFailures = 0;
  cloudflare = false;

  snapshotHealthyProduction(app: string, healthUrls: readonly string[]): Promise<RollbackTarget> {
    if (this.snapshotFailures > 0) {
      this.snapshotFailures -= 1;
      return Promise.reject(new Error("Injected production identity observation failure"));
    }
    if (healthUrls.length === 0 || healthUrls.length > 2) {
      throw new Error(`Unexpected snapshot health URLs: ${healthUrls.join(",")}`);
    }
    const value = this.snapshots.shift();
    if (!value) throw new Error("Missing fake production snapshot");
    if (app !== "ai-ubq-fi" && app !== "p-ai-ubq-fi") {
      throw new Error(`Unexpected snapshot target: ${app}`);
    }
    return Promise.resolve(value);
  }

  verifyHealthIdentity(
    urls: readonly string[],
    gitSha: string,
    revisionId: string,
  ): Promise<readonly HealthIdentity[]> {
    this.verifyCalls.push({ urls, gitSha, revisionId });
    if (this.verifyFailures > 0) {
      this.verifyFailures -= 1;
      return Promise.reject(new Error("Injected managed identity verification failure"));
    }
    return Promise.resolve(urls.map((url) => ({ url, gitSha, revisionId })));
  }

  verifyProductionHealthIdentity(
    managedUrl: string,
    customUrl: string,
    gitSha: string,
    revisionId: string,
  ): Promise<ProductionHealthAttestation> {
    this.verifyCalls.push({ urls: [managedUrl, customUrl], gitSha, revisionId });
    if (this.verifyFailures > 0) {
      this.verifyFailures -= 1;
      return Promise.reject(new Error("Injected stable identity verification failure"));
    }
    return Promise.resolve({
      managed: { url: managedUrl, gitSha, revisionId },
      custom: this.cloudflare
        ? { kind: "cloudflare_challenge", url: customUrl, status: 403, ray: "cf-ray-123456" }
        : { kind: "identity", identity: { url: customUrl, gitSha, revisionId } },
    });
  }
}

type ControllerRun = {
  result: ProductionRollbackEvidence | null;
  error: ProductionRollbackError | null;
  promotionCalls: ProductionRollbackPromotionInput[];
  evidence: ProductionRollbackEvidence[];
};

const runController = async (
  deno: FakeRollbackDenoClient,
  input: ProductionRollbackInput,
  partial: Partial<{ promotionFailure: string; persistFailure: string }> = {},
): Promise<ControllerRun> => {
  const promotionCalls: ProductionRollbackPromotionInput[] = [];
  const evidence: ProductionRollbackEvidence[] = [];
  const promotion = (value: ProductionRollbackPromotionInput): Promise<{ workflowRunId: number }> => {
    promotionCalls.push(value);
    if (partial.promotionFailure !== undefined) return Promise.reject(new Error(partial.promotionFailure));
    return Promise.resolve({ workflowRunId: 77 });
  };
  const persist = (value: ProductionRollbackEvidence): Promise<void> => {
    if (partial.persistFailure !== undefined) return Promise.reject(new Error(partial.persistFailure));
    evidence.push(value);
    return Promise.resolve();
  };
  try {
    const result = await executeProductionRollback(input, { deno, promotion, persist });
    return { result, error: null, promotionCalls, evidence };
  } catch (error) {
    return { result: null, error: error as ProductionRollbackError, promotionCalls, evidence };
  }
};

Deno.test("rollback controller promotes only the exact prior revision from the pre-deploy attestation", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshots.push(snapshot(CANDIDATE_SHA, CANDIDATE_REVISION));
  const { result, promotionCalls, evidence } = await runController(deno, validInput());
  assert.ok(result);
  assert.equal(result.error, null);
  assert.deepEqual(promotionCalls.length, 1);
  // The only promotion target is the immutable prior revision captured before promotion.
  assert.deepEqual(promotionCalls[0]!.targetGitSha, PREVIOUS_SHA);
  assert.deepEqual(promotionCalls[0]!.targetRevision, PREVIOUS_REVISION);
  assert.deepEqual(promotionCalls[0]!.expectedCurrent.gitSha, CANDIDATE_SHA);
  assert.deepEqual(promotionCalls[0]!.expectedCurrent.revisionId, CANDIDATE_REVISION);
  assert.deepEqual(promotionCalls[0]!.expectedDevelopmentGitSha, DEVELOPMENT_SHA);
  // Managed and custom endpoints are verified against the exact previous identity.
  assert.deepEqual(deno.verifyCalls.length, 1);
  assert.deepEqual(deno.verifyCalls[0]!.gitSha, PREVIOUS_SHA);
  assert.deepEqual(deno.verifyCalls[0]!.revisionId, PREVIOUS_REVISION);
  assert.deepEqual(result.status, "rolled_back");
  assert.deepEqual(result.classification, "rollback");
  assert.deepEqual(result.promotion_workflow_run_id, 77);
  assert.deepEqual(result.managed?.git_sha, PREVIOUS_SHA);
  assert.deepEqual(result.managed?.revision, PREVIOUS_REVISION);
  assert.deepEqual(result.custom, {
    kind: "identity",
    url: CUSTOM_URL,
    git_sha: PREVIOUS_SHA,
    revision: PREVIOUS_REVISION,
  });
  assert.deepEqual(result.cloudflare_403_warning, false);
  assert.deepEqual(evidence.length, 1);
  assert.deepEqual(evidence[0]!.status, "rolled_back");
});

Deno.test("rollback controller verifies the already-live prior revision without promoting", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshots.push(snapshot());
  const { result, promotionCalls, evidence } = await runController(deno, validInput());
  assert.ok(result);
  assert.deepEqual(promotionCalls.length, 0);
  assert.deepEqual(deno.verifyCalls.length, 1);
  assert.deepEqual(deno.verifyCalls[0]!.gitSha, PREVIOUS_SHA);
  assert.deepEqual(deno.verifyCalls[0]!.revisionId, PREVIOUS_REVISION);
  assert.deepEqual(result.status, "already_previous");
  assert.deepEqual(result.classification, "already_previous");
  assert.deepEqual(result.promotion_workflow_run_id, null);
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller refuses an unrelated production identity and promotes nothing", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshots.push(snapshot(UNRELATED_SHA, UNRELATED_REVISION));
  const { error, promotionCalls, evidence } = await runController(deno, validInput());
  assert.ok(error);
  assert.match(error.message, /unrelated/u);
  assert.deepEqual(promotionCalls.length, 0);
  assert.deepEqual(deno.verifyCalls.length, 0);
  assert.deepEqual(error.evidence.status, "failed");
  assert.deepEqual(error.evidence.classification, "unrelated");
  assert.deepEqual(error.evidence.observed, { git_sha: UNRELATED_SHA, revision: UNRELATED_REVISION });
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller fails closed when promotion cannot be proven", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshots.push(snapshot(CANDIDATE_SHA, CANDIDATE_REVISION));
  const { error, promotionCalls, evidence } = await runController(deno, validInput(), {
    promotionFailure: "Promotion workflow failed",
  });
  assert.ok(error);
  assert.deepEqual(promotionCalls.length, 1);
  assert.deepEqual(promotionCalls[0]!.targetRevision, PREVIOUS_REVISION);
  assert.deepEqual(deno.verifyCalls.length, 0);
  assert.deepEqual(error.evidence.status, "failed");
  assert.deepEqual(error.evidence.classification, "rollback");
  assert.deepEqual(error.evidence.promotion_workflow_run_id, null);
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller fails closed when the restored identity cannot be verified", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshots.push(snapshot(CANDIDATE_SHA, CANDIDATE_REVISION));
  deno.verifyFailures = 1;
  const { error, promotionCalls, evidence } = await runController(deno, validInput());
  assert.ok(error);
  assert.deepEqual(promotionCalls.length, 1);
  assert.deepEqual(deno.verifyCalls.length, 1);
  assert.deepEqual(error.evidence.status, "failed");
  assert.deepEqual(error.evidence.managed, null);
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller records a Cloudflare 403 challenge as a warning and still converges", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshots.push(snapshot(CANDIDATE_SHA, CANDIDATE_REVISION));
  deno.cloudflare = true;
  const { result, promotionCalls, evidence } = await runController(deno, validInput());
  assert.ok(result);
  assert.deepEqual(promotionCalls.length, 1);
  assert.deepEqual(promotionCalls[0]!.targetRevision, PREVIOUS_REVISION);
  assert.deepEqual(result.status, "rolled_back");
  assert.deepEqual(result.cloudflare_403_warning, true);
  assert.deepEqual(result.cloudflare_ray, "cf-ray-123456");
  assert.deepEqual(result.custom, {
    kind: "cloudflare_challenge",
    url: CUSTOM_URL,
    status: 403,
    ray: "cf-ray-123456",
  });
  // The managed route is still proven with the exact previous identity.
  assert.deepEqual(result.managed?.git_sha, PREVIOUS_SHA);
  assert.deepEqual(evidence.length, 1);
  assert.deepEqual(evidence[0]!.cloudflare_403_warning, true);
});

Deno.test("rollback controller never acts on Codex review findings alone", async () => {
  const deno = new FakeRollbackDenoClient();
  const codexTrigger = validInput({
    failure: {
      kind: "codex_review_finding" as unknown as ProductionRollbackInput["failure"]["kind"],
      cause: "codex_review_finding",
      observed_at: "2026-08-21T10:30:00.000Z",
    },
  });
  const { error, promotionCalls, evidence } = await runController(deno, codexTrigger);
  assert.ok(error);
  assert.match(error.message, /Codex review findings never roll back/u);
  assert.deepEqual(promotionCalls.length, 0);
  assert.deepEqual(deno.snapshots.length, 0); // never even observed production
  assert.deepEqual(deno.verifyCalls.length, 0);
  assert.deepEqual(error.evidence.status, "failed");
  assert.deepEqual(error.evidence.classification, "refused");
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller refuses an attestation that does not differ from the candidate", async () => {
  const deno = new FakeRollbackDenoClient();
  const { error, promotionCalls, evidence } = await runController(
    deno,
    validInput({ previous: snapshot(CANDIDATE_SHA, CANDIDATE_REVISION) }),
  );
  assert.ok(error);
  assert.match(error.message, /does not differ/u);
  assert.deepEqual(promotionCalls.length, 0);
  assert.deepEqual(deno.snapshots.length, 0);
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller fails closed when production identity is unobservable", async () => {
  const deno = new FakeRollbackDenoClient();
  deno.snapshotFailures = 1;
  const { error, promotionCalls, evidence } = await runController(deno, validInput());
  assert.ok(error);
  assert.deepEqual(promotionCalls.length, 0);
  assert.deepEqual(error.evidence.status, "failed");
  assert.deepEqual(error.evidence.classification, "unobservable");
  assert.deepEqual(error.evidence.observed, null);
  assert.deepEqual(evidence.length, 1);
});

Deno.test("rollback controller validation rejects unknown triggers and malformed identity", async () => {
  const deno = new FakeRollbackDenoClient();
  const unknown = await runController(
    deno,
    validInput({
      failure: {
        kind: "random_event" as unknown as ProductionRollbackInput["failure"]["kind"],
        cause: "random_event",
        observed_at: "2026-08-21T10:30:00.000Z",
      },
    }),
  );
  assert.ok(unknown.error);
  assert.match(unknown.error.message, /refuses the trigger/u);
  assert.deepEqual(unknown.promotionCalls.length, 0);
  assert.deepEqual(deno.snapshots.length, 0);

  const badCause = await runController(
    deno,
    validInput({
      failure: {
        kind: "post_promotion_acceptance_failed",
        cause: "not a machine label",
        observed_at: "2026-08-21T10:30:00.000Z",
      },
    }),
  );
  assert.ok(badCause.error);
  assert.match(badCause.error.message, /machine-readable label/u);

  const badSha = await runController(
    deno,
    validInput({ candidate: { gitSha: "not-a-sha", revisionId: CANDIDATE_REVISION } }),
  );
  assert.ok(badSha.error);
  assert.match(badSha.error.message, /Candidate Git SHA/u);
});

Deno.test("rollback controller supports the managed-only preview path", async () => {
  const deno = new FakeRollbackDenoClient();
  const previewPrevious = snapshot(PREVIOUS_SHA, PREVIOUS_REVISION, [PREVIEW_URL]);
  deno.snapshots.push({ ...previewPrevious, gitSha: CANDIDATE_SHA, revisionId: CANDIDATE_REVISION });
  const { result, promotionCalls, evidence } = await runController(
    deno,
    validInput({ app: "p-ai-ubq-fi", previous: previewPrevious }),
  );
  assert.ok(result);
  assert.deepEqual(promotionCalls.length, 1);
  assert.deepEqual(promotionCalls[0]!.targetRevision, PREVIOUS_REVISION);
  // Managed-only verification is used; no custom attestation exists.
  assert.deepEqual(deno.verifyCalls.length, 1);
  assert.deepEqual(deno.verifyCalls[0]!.urls, [PREVIEW_URL]);
  assert.deepEqual(result.status, "rolled_back");
  assert.deepEqual(result.custom, null);
  assert.deepEqual(result.cloudflare_403_warning, false);
  assert.deepEqual(evidence.length, 1);
});
