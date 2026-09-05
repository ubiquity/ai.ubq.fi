import { assertBootstrapReference, assertFullGitSha, type SentinelBootstrapActivationPointerV1 } from "./contracts.ts";

const FULL_RUN_ID = /^[1-9][0-9]*$/u;
const WORKFLOW_REF = /^refs\/heads\/development$/u;

/**
 * This policy is intentionally defined in the bootstrap trust domain. It is
 * not read from repository content, workflow inputs, model output, or a
 * candidate branch.
 */
export const SENTINEL_BOOTSTRAP_POLICY = Object.freeze({
  schemaVersion: 1,
  repository: "ubiquity/ai.ubq.fi",
  developmentRef: "refs/heads/development",
  bootstrapWorkflow: ".github/workflows/provider-sentinel-bootstrap.yml",
  evolvingWorkflow: ".github/workflows/provider-sentinel.yml",
  implementationModel: "gpt-5.6-luna",
  implementationReasoning: "max",
  repeatedFailureThreshold: 3,
  maximumFencedGenerations: 64,
  maximumEvidenceReferences: 8,
});

export type BootstrapEnvironment = Readonly<{
  repository: string;
  ref: string;
  sha: string;
  runId: number;
  workflowRef: string;
}>;

type EnvironmentReader = Readonly<{ get(name: string): string | undefined }>;

const environment: EnvironmentReader = {
  get(name) {
    try {
      return Deno.env.get(name);
    } catch {
      return undefined;
    }
  },
};

export const parseBootstrapEnvironment = (
  reader: EnvironmentReader = environment,
): BootstrapEnvironment => {
  const repository = reader.get("GITHUB_REPOSITORY")?.trim() ?? "";
  const ref = reader.get("GITHUB_REF")?.trim() ?? "";
  const sha = reader.get("GITHUB_SHA")?.trim() ?? "";
  const runIdText = reader.get("GITHUB_RUN_ID")?.trim() ?? "";
  const workflowRef = reader.get("GITHUB_WORKFLOW_REF")?.trim() ?? "";
  if (repository !== SENTINEL_BOOTSTRAP_POLICY.repository) {
    throw new Error("Sentinel bootstrap repository identity is invalid");
  }
  if (!WORKFLOW_REF.test(ref)) throw new Error("Sentinel bootstrap must run from development");
  assertFullGitSha(sha, "Sentinel bootstrap execution SHA");
  if (!FULL_RUN_ID.test(runIdText)) throw new Error("Sentinel bootstrap run ID is invalid");
  const runId = Number(runIdText);
  if (!Number.isSafeInteger(runId) || runId <= 0) throw new Error("Sentinel bootstrap run ID is invalid");
  const expectedWorkflowRef =
    `${SENTINEL_BOOTSTRAP_POLICY.repository}/${SENTINEL_BOOTSTRAP_POLICY.bootstrapWorkflow}@${ref}`;
  if (workflowRef !== expectedWorkflowRef) throw new Error("Sentinel bootstrap workflow ref is not immutable");
  return { repository, ref, sha, runId, workflowRef };
};

export const assertImplementationSelection = (model: string, reasoning: string): void => {
  if (
    model !== SENTINEL_BOOTSTRAP_POLICY.implementationModel ||
    reasoning !== SENTINEL_BOOTSTRAP_POLICY.implementationReasoning
  ) throw new Error("Sentinel implementation selection violates the owner-controlled bootstrap policy");
};

export const SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH = "docs/sentinel-provider-executor-evidence" as const;

export const isBootstrapProtectedPath = (path: string): boolean => {
  if (
    path.length === 0 || path.startsWith("/") || path.includes("\\") || path.includes("..") ||
    path.split("/").some((segment) => segment === "" || segment === ".")
  ) return true;
  return path === SENTINEL_BOOTSTRAP_POLICY.bootstrapWorkflow ||
    path === ".github/workflows/sentinel-revision-control.yml" ||
    path === "docs/sentinel-provider-state.json" ||
    path === "docs/sentinel-bootstrap-state.json" ||
    path === "scripts/sentinel/bootstrap" ||
    path.startsWith("scripts/sentinel/bootstrap/") ||
    path === SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH ||
    path.startsWith(`${SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH}/`);
};

export const assertNoBootstrapMutation = (changedPaths: readonly string[]): void => {
  for (const path of changedPaths) {
    if (isBootstrapProtectedPath(path)) {
      throw new Error(`Evolving Sentinel cannot modify bootstrap path: ${path}`);
    }
  }
};

export const assertBootstrapActivationFence = (
  pointer: SentinelBootstrapActivationPointerV1,
  expected: Readonly<{ generation: number; activeSha: string }>,
): void => {
  if (pointer.generation !== expected.generation || pointer.active_sha !== expected.activeSha) {
    throw new Error("Sentinel bootstrap activation fence is stale");
  }
};

export const assertRecoveryDispatchIdentity = (
  input: Readonly<{ repository: string; ref: string; sha: string }>,
): void => {
  if (input.repository !== SENTINEL_BOOTSTRAP_POLICY.repository) {
    throw new Error("Sentinel recovery dispatch repository identity is invalid");
  }
  if (input.ref !== SENTINEL_BOOTSTRAP_POLICY.developmentRef) {
    throw new Error("Sentinel recovery dispatch ref is invalid");
  }
  assertFullGitSha(input.sha, "Sentinel recovery dispatch SHA");
};

export const assertProtectedRecoveryReference = (value: string, label: string): string =>
  assertBootstrapReference(value, label);
