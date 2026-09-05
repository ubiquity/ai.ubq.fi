import {
  defaultRevisionHealthUrl,
  DenoDeployClient,
  type DenoRevision,
  type HealthIdentity,
  type ProductionHealthAttestation,
  type RollbackTarget,
} from "./deploy.ts";

const GITHUB_API_BASE_URL = "https://api.github.com/";
const GITHUB_API_VERSION = "2026-03-10";
const EXPECTED_REPOSITORY = "ubiquity/ai.ubq.fi";
const ORGANIZATION = "ubiquity-dao";
const PRODUCTION_APP = "ai-ubq-fi";
const PREVIEW_APP = "p-ai-ubq-fi";
const FULL_GIT_SHA = /^[0-9a-f]{40}$/;
const REVISION_ID = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
const CORRELATION_ID = /^[A-Za-z0-9][A-Za-z0-9._:-]{7,127}$/;

export const REVISION_CONTROL_RESULT_PATH = "sentinel-revision-control-result.json";

export type RevisionControlApp = typeof PRODUCTION_APP | typeof PREVIEW_APP;

export interface RevisionControlInput {
  readonly correlationId: string;
  readonly targetApp: RevisionControlApp;
  readonly targetGitSha: string;
  readonly targetRevision: string;
  readonly expectedCurrentGitSha: string;
  readonly expectedCurrentRevision: string;
  readonly expectedDevelopmentGitSha: string;
  readonly workflowGitSha: string;
  readonly workflowRef: "refs/heads/development";
  readonly githubRepository: string;
  readonly githubRunId: number;
}

export interface RevisionControlDenoClient {
  snapshotHealthyProduction(
    app: string,
    healthUrls: readonly string[],
  ): Promise<RollbackTarget>;
  assertRevisionBelongsToApp(app: string, revisionId: string): Promise<DenoRevision>;
  getRevision(revisionId: string): Promise<DenoRevision>;
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
  promoteRevision(app: string, revisionId: string): Promise<void>;
}

export interface RevisionControlDependencies {
  readonly deno: RevisionControlDenoClient;
  readonly githubToken: string;
  readonly githubFetch?: typeof fetch;
  readonly githubApiBaseUrl?: string;
  readonly now?: () => number;
}

export interface RevisionControlHealthIdentity {
  readonly url: string;
  readonly git_sha: string;
  readonly revision: string;
}

export type RevisionControlCustomAttestation =
  | Readonly<{
    kind: "identity";
    identity: RevisionControlHealthIdentity;
  }>
  | Readonly<{
    kind: "cloudflare_challenge";
    url: string;
    status: 403;
    ray: string;
  }>;

export interface RevisionControlResult {
  readonly schema_version: 1;
  readonly status: "promoted";
  readonly correlation_id: string;
  readonly github_run_id: number;
  readonly repository: string;
  readonly expected_development_git_sha: string;
  readonly app: RevisionControlApp;
  readonly previous: Readonly<{
    git_sha: string;
    revision: string;
    snapshotted_at: string;
  }>;
  readonly target: Readonly<{
    git_sha: string;
    revision: string;
    immutable_health_url: string;
  }>;
  readonly stable: Readonly<{
    managed: RevisionControlHealthIdentity;
    custom: RevisionControlCustomAttestation | null;
  }>;
  readonly promoted_at: string;
}

export interface RevisionControlFailureResult {
  readonly schema_version: 1;
  readonly status: "no_change" | "compensated" | "compensation_failed" | "outcome_unknown";
  readonly correlation_id: string;
  readonly github_run_id: number;
  readonly repository: string;
  readonly expected_development_git_sha: string;
  readonly app: RevisionControlApp;
  readonly previous: Readonly<{ git_sha: string; revision: string }>;
  readonly target: Readonly<{ git_sha: string; revision: string }>;
  readonly failed_at: string;
  readonly error_class: string;
}

export class RevisionControlOutcomeError extends Error {
  readonly result: RevisionControlFailureResult;

  constructor(message: string, result: RevisionControlFailureResult, options?: ErrorOptions) {
    super(message, options);
    this.name = "RevisionControlOutcomeError";
    this.result = result;
  }
}

const required = (
  environment: Readonly<Record<string, string | undefined>>,
  name: string,
): string => {
  const value = environment[name]?.trim() ?? "";
  if (value === "") throw new Error(`${name} is required`);
  return value;
};

const requireFullGitSha = (value: string, label: string): string => {
  if (!FULL_GIT_SHA.test(value)) throw new Error(`${label} must be a lowercase, full Git commit SHA`);
  return value;
};

const requireRevisionId = (value: string, label: string): string => {
  if (!REVISION_ID.test(value)) throw new Error(`${label} must be a safe Deno revision ID`);
  return value;
};

const parsePositiveInteger = (value: string, label: string): number => {
  if (!/^[1-9][0-9]*$/u.test(value)) throw new Error(`${label} must be a positive integer`);
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed)) throw new Error(`${label} must be a positive integer`);
  return parsed;
};

export const parseRevisionControlEnvironment = (
  environment: Readonly<Record<string, string | undefined>>,
): RevisionControlInput => {
  const correlationId = required(environment, "SENTINEL_CORRELATION_ID");
  if (!CORRELATION_ID.test(correlationId)) {
    throw new Error("SENTINEL_CORRELATION_ID must be a bounded opaque identifier");
  }

  const targetAppValue = required(environment, "SENTINEL_TARGET_APP");
  if (targetAppValue !== PRODUCTION_APP && targetAppValue !== PREVIEW_APP) {
    throw new Error("SENTINEL_TARGET_APP must be ai-ubq-fi or p-ai-ubq-fi");
  }

  const githubRepository = required(environment, "GITHUB_REPOSITORY");
  if (githubRepository !== EXPECTED_REPOSITORY) {
    throw new Error(`GITHUB_REPOSITORY must be ${EXPECTED_REPOSITORY}`);
  }

  const expectedDevelopmentGitSha = requireFullGitSha(
    required(environment, "SENTINEL_EXPECTED_DEVELOPMENT_GIT_SHA"),
    "SENTINEL_EXPECTED_DEVELOPMENT_GIT_SHA",
  );
  const workflowGitSha = requireFullGitSha(
    required(environment, "GITHUB_SHA"),
    "GITHUB_SHA",
  );
  if (workflowGitSha !== expectedDevelopmentGitSha) {
    throw new Error("The revision-control workflow must execute from the expected development commit");
  }
  const workflowRef = required(environment, "GITHUB_REF");
  if (workflowRef !== "refs/heads/development") {
    throw new Error("The revision-control workflow must execute from refs/heads/development");
  }

  const targetRevision = requireRevisionId(
    required(environment, "SENTINEL_TARGET_REVISION"),
    "SENTINEL_TARGET_REVISION",
  );
  const expectedCurrentRevision = requireRevisionId(
    required(environment, "SENTINEL_EXPECTED_CURRENT_REVISION"),
    "SENTINEL_EXPECTED_CURRENT_REVISION",
  );
  if (`${targetAppValue}-${targetRevision}`.length > 63) {
    throw new Error("SENTINEL_TARGET_REVISION is too long for the immutable Deno hostname");
  }
  if (`${targetAppValue}-${expectedCurrentRevision}`.length > 63) {
    throw new Error("SENTINEL_EXPECTED_CURRENT_REVISION is too long for the Deno application");
  }

  return Object.freeze({
    correlationId,
    targetApp: targetAppValue,
    targetGitSha: requireFullGitSha(
      required(environment, "SENTINEL_TARGET_GIT_SHA"),
      "SENTINEL_TARGET_GIT_SHA",
    ),
    targetRevision,
    expectedCurrentGitSha: requireFullGitSha(
      required(environment, "SENTINEL_EXPECTED_CURRENT_GIT_SHA"),
      "SENTINEL_EXPECTED_CURRENT_GIT_SHA",
    ),
    expectedCurrentRevision,
    expectedDevelopmentGitSha,
    workflowGitSha,
    workflowRef,
    githubRepository,
    githubRunId: parsePositiveInteger(required(environment, "GITHUB_RUN_ID"), "GITHUB_RUN_ID"),
  });
};

const record = (value: unknown): Record<string, unknown> | null =>
  value !== null && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : null;

export const readDevelopmentGitSha = async (
  repository: string,
  token: string,
  fetcher: typeof fetch = fetch,
  apiBaseUrl = GITHUB_API_BASE_URL,
): Promise<string> => {
  if (repository !== EXPECTED_REPOSITORY) throw new Error(`GitHub repository must be ${EXPECTED_REPOSITORY}`);
  if (token.trim() === "") throw new Error("GITHUB_TOKEN is required");

  const baseUrl = new URL(apiBaseUrl);
  if (
    baseUrl.protocol !== "https:" || baseUrl.username !== "" || baseUrl.password !== "" ||
    baseUrl.search !== "" || baseUrl.hash !== ""
  ) {
    throw new Error("GitHub API base URL must be a trusted HTTPS origin");
  }
  const url = new URL(
    `repos/${repository}/git/ref/heads/development`,
    baseUrl.href.endsWith("/") ? baseUrl : new URL(`${baseUrl.href}/`),
  );
  const response = await fetcher(url, {
    method: "GET",
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": GITHUB_API_VERSION,
    },
    redirect: "manual",
    signal: AbortSignal.timeout(20_000),
  });
  if (response.status !== 200) {
    await response.body?.cancel().catch(() => undefined);
    throw new Error(`Read GitHub development ref failed with HTTP ${response.status}`);
  }
  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    throw new Error("Read GitHub development ref returned invalid JSON");
  }
  const outer = record(payload);
  const object = record(outer?.object);
  if (
    outer?.ref !== "refs/heads/development" || object?.type !== "commit" ||
    typeof object.sha !== "string" || !FULL_GIT_SHA.test(object.sha)
  ) {
    throw new Error("Read GitHub development ref returned an invalid commit identity");
  }
  return object.sha;
};

const healthUrlsFor = (app: RevisionControlApp): readonly string[] =>
  app === PRODUCTION_APP
    ? [
      "https://ai-ubq-fi.ubiquity-dao.deno.net/health",
      "https://ai.ubq.fi/health",
    ]
    : ["https://p-ai-ubq-fi.ubiquity-dao.deno.net/health"];

const assertExpectedCurrent = (
  snapshot: RollbackTarget,
  input: RevisionControlInput,
): void => {
  if (
    snapshot.gitSha !== input.expectedCurrentGitSha ||
    snapshot.revisionId !== input.expectedCurrentRevision
  ) {
    throw new Error(
      `Stable ${input.targetApp} identity changed before promotion; expected ` +
        `${input.expectedCurrentGitSha}/${input.expectedCurrentRevision}`,
    );
  }
};

const assertRoutedTarget = (revision: DenoRevision, expectedRevision: string): void => {
  if (revision.id !== expectedRevision) {
    throw new Error(`Deno returned the wrong revision for ${expectedRevision}`);
  }
  if (revision.status !== "routed") {
    throw new Error(`Target revision ${expectedRevision} is not routed`);
  }
};

const resultHealthIdentity = (identity: HealthIdentity): RevisionControlHealthIdentity =>
  Object.freeze({
    url: identity.url,
    git_sha: identity.gitSha,
    revision: identity.revisionId,
  });

const resultCustomAttestation = (
  attestation: ProductionHealthAttestation["custom"],
): RevisionControlCustomAttestation =>
  attestation.kind === "identity"
    ? Object.freeze({
      kind: "identity",
      identity: resultHealthIdentity(attestation.identity),
    })
    : Object.freeze({
      kind: "cloudflare_challenge",
      url: attestation.url,
      status: attestation.status,
      ray: attestation.ray,
    });

const verifyStableIdentity = async (
  deno: RevisionControlDenoClient,
  app: RevisionControlApp,
  healthUrls: readonly string[],
  gitSha: string,
  revisionId: string,
): Promise<
  Readonly<{
    managed: RevisionControlHealthIdentity;
    custom: RevisionControlCustomAttestation | null;
  }>
> => {
  if (app === PRODUCTION_APP) {
    const attestation = await deno.verifyProductionHealthIdentity(
      healthUrls[0]!,
      healthUrls[1]!,
      gitSha,
      revisionId,
    );
    return {
      managed: resultHealthIdentity(attestation.managed),
      custom: resultCustomAttestation(attestation.custom),
    };
  }
  const identity = (await deno.verifyHealthIdentity(healthUrls, gitSha, revisionId))[0];
  if (!identity) throw new Error("Preview stable health returned no identity");
  return { managed: resultHealthIdentity(identity), custom: null };
};

const failureResult = (
  input: RevisionControlInput,
  previous: RollbackTarget,
  status: RevisionControlFailureResult["status"],
  now: number,
  error: unknown,
): RevisionControlFailureResult => ({
  schema_version: 1,
  status,
  correlation_id: input.correlationId,
  github_run_id: input.githubRunId,
  repository: input.githubRepository,
  expected_development_git_sha: input.expectedDevelopmentGitSha,
  app: input.targetApp,
  previous: { git_sha: previous.gitSha, revision: previous.revisionId },
  target: { git_sha: input.targetGitSha, revision: input.targetRevision },
  failed_at: new Date(now).toISOString(),
  error_class: error instanceof Error ? error.name : "unknown",
});

export const executeRevisionControl = async (
  input: RevisionControlInput,
  dependencies: RevisionControlDependencies,
): Promise<RevisionControlResult> => {
  const developmentGitSha = await readDevelopmentGitSha(
    input.githubRepository,
    dependencies.githubToken,
    dependencies.githubFetch,
    dependencies.githubApiBaseUrl,
  );
  if (developmentGitSha !== input.expectedDevelopmentGitSha) {
    throw new Error(
      `Development advanced before promotion; expected ${input.expectedDevelopmentGitSha}, observed ${developmentGitSha}`,
    );
  }

  const healthUrls = healthUrlsFor(input.targetApp);
  const initialSnapshot = await dependencies.deno.snapshotHealthyProduction(input.targetApp, healthUrls);
  assertExpectedCurrent(initialSnapshot, input);

  await dependencies.deno.assertRevisionBelongsToApp(input.targetApp, input.targetRevision);
  assertRoutedTarget(await dependencies.deno.getRevision(input.targetRevision), input.targetRevision);

  const immutableHealthUrl = defaultRevisionHealthUrl(
    input.targetApp,
    input.targetRevision,
    ORGANIZATION,
  );
  await dependencies.deno.verifyHealthIdentity(
    [immutableHealthUrl],
    input.targetGitSha,
    input.targetRevision,
  );

  // Recheck both mutable identities after all slower immutable checks. The
  // shared workflow concurrency lock prevents other repository deployment
  // workflows from entering this section. These checks also stop if an
  // out-of-band deployment changed either identity while this run waited.
  const finalSnapshot = await dependencies.deno.snapshotHealthyProduction(input.targetApp, healthUrls);
  assertExpectedCurrent(finalSnapshot, input);
  await dependencies.deno.assertRevisionBelongsToApp(input.targetApp, input.targetRevision);
  assertRoutedTarget(await dependencies.deno.getRevision(input.targetRevision), input.targetRevision);

  const finalDevelopmentGitSha = await readDevelopmentGitSha(
    input.githubRepository,
    dependencies.githubToken,
    dependencies.githubFetch,
    dependencies.githubApiBaseUrl,
  );
  if (finalDevelopmentGitSha !== input.expectedDevelopmentGitSha) {
    throw new Error(
      `Development advanced immediately before promotion; expected ${input.expectedDevelopmentGitSha}, ` +
        `observed ${finalDevelopmentGitSha}`,
    );
  }

  let stable: Readonly<{
    managed: RevisionControlHealthIdentity;
    custom: RevisionControlCustomAttestation | null;
  }>;
  let promotionAcknowledged = false;
  try {
    await dependencies.deno.promoteRevision(input.targetApp, input.targetRevision);
    promotionAcknowledged = true;
    stable = await verifyStableIdentity(
      dependencies.deno,
      input.targetApp,
      healthUrls,
      input.targetGitSha,
      input.targetRevision,
    );
  } catch (operationError) {
    const failedAt = dependencies.now?.() ?? Date.now();
    let observed: RollbackTarget;
    try {
      observed = await dependencies.deno.snapshotHealthyProduction(input.targetApp, [healthUrls[0]!]);
    } catch (observationError) {
      throw new RevisionControlOutcomeError(
        "The target promotion outcome could not be determined from the managed route",
        failureResult(input, initialSnapshot, "outcome_unknown", failedAt, operationError),
        { cause: new AggregateError([operationError, observationError]) },
      );
    }
    const observedTarget = observed.gitSha === input.targetGitSha &&
      observed.revisionId === input.targetRevision;
    const observedPrevious = observed.gitSha === initialSnapshot.gitSha &&
      observed.revisionId === initialSnapshot.revisionId;
    if (!observedTarget && !observedPrevious) {
      throw new RevisionControlOutcomeError(
        "The managed route reported an unrelated identity after the promotion attempt",
        failureResult(input, initialSnapshot, "outcome_unknown", failedAt, operationError),
        { cause: operationError },
      );
    }

    if (!promotionAcknowledged && observedPrevious) {
      try {
        await verifyStableIdentity(
          dependencies.deno,
          input.targetApp,
          healthUrls,
          initialSnapshot.gitSha,
          initialSnapshot.revisionId,
        );
      } catch (verificationError) {
        throw new RevisionControlOutcomeError(
          "The previous managed identity remained live, but full stable verification failed",
          failureResult(input, initialSnapshot, "outcome_unknown", failedAt, operationError),
          { cause: new AggregateError([operationError, verificationError]) },
        );
      }
      throw new RevisionControlOutcomeError(
        "The target promotion failed before the stable identity changed",
        failureResult(input, initialSnapshot, "no_change", failedAt, operationError),
        { cause: operationError },
      );
    }

    let compensationError: unknown = null;
    try {
      await dependencies.deno.promoteRevision(input.targetApp, initialSnapshot.revisionId);
    } catch (error) {
      compensationError = error;
    }
    try {
      await verifyStableIdentity(
        dependencies.deno,
        input.targetApp,
        healthUrls,
        initialSnapshot.gitSha,
        initialSnapshot.revisionId,
      );
    } catch (compensationVerificationError) {
      throw new RevisionControlOutcomeError(
        "The target promotion failed and compensation did not converge",
        failureResult(input, initialSnapshot, "compensation_failed", failedAt, operationError),
        {
          cause: new AggregateError(
            [operationError, compensationError, compensationVerificationError].filter((value) => value !== null),
          ),
        },
      );
    }
    throw new RevisionControlOutcomeError(
      "The target promotion failed; the previous revision was restored",
      failureResult(input, initialSnapshot, "compensated", failedAt, operationError),
      { cause: operationError },
    );
  }

  const now = dependencies.now?.() ?? Date.now();
  if (!Number.isFinite(now)) throw new Error("Revision-control clock returned an invalid time");
  return Object.freeze({
    schema_version: 1,
    status: "promoted",
    correlation_id: input.correlationId,
    github_run_id: input.githubRunId,
    repository: input.githubRepository,
    expected_development_git_sha: input.expectedDevelopmentGitSha,
    app: input.targetApp,
    previous: Object.freeze({
      git_sha: initialSnapshot.gitSha,
      revision: initialSnapshot.revisionId,
      snapshotted_at: initialSnapshot.snapshottedAt,
    }),
    target: Object.freeze({
      git_sha: input.targetGitSha,
      revision: input.targetRevision,
      immutable_health_url: immutableHealthUrl,
    }),
    stable: Object.freeze(stable),
    promoted_at: new Date(now).toISOString(),
  });
};

export const runRevisionControlCli = async (): Promise<void> => {
  const environment = Object.fromEntries([
    "SENTINEL_CORRELATION_ID",
    "SENTINEL_TARGET_APP",
    "SENTINEL_TARGET_GIT_SHA",
    "SENTINEL_TARGET_REVISION",
    "SENTINEL_EXPECTED_CURRENT_GIT_SHA",
    "SENTINEL_EXPECTED_CURRENT_REVISION",
    "SENTINEL_EXPECTED_DEVELOPMENT_GIT_SHA",
    "GITHUB_REPOSITORY",
    "GITHUB_RUN_ID",
    "GITHUB_SHA",
    "GITHUB_REF",
    "DENO_DEPLOY_TOKEN",
    "GITHUB_TOKEN",
  ].map((name) => [name, Deno.env.get(name)]));
  const input = parseRevisionControlEnvironment(environment);
  const denoToken = required(environment, "DENO_DEPLOY_TOKEN");
  const githubToken = required(environment, "GITHUB_TOKEN");
  try {
    const result = await executeRevisionControl(input, {
      deno: new DenoDeployClient({ token: denoToken, organization: ORGANIZATION }),
      githubToken,
    });
    await Deno.writeTextFile(
      REVISION_CONTROL_RESULT_PATH,
      `${JSON.stringify(result, null, 2)}\n`,
      { createNew: true, mode: 0o600 },
    );
    console.log(`Promoted and verified ${result.app} revision ${result.target.revision}.`);
  } catch (error) {
    if (error instanceof RevisionControlOutcomeError) {
      await Deno.writeTextFile(
        REVISION_CONTROL_RESULT_PATH,
        `${JSON.stringify(error.result, null, 2)}\n`,
        { createNew: true, mode: 0o600 },
      );
    }
    throw error;
  }
};

if (import.meta.main) {
  await runRevisionControlCli();
}

// Protected Provider Sentinel recovery operation (provider owner rollback).
// Re-exported here so the pinned executor module graph reaches it; it is not
// yet invoked by the revision-control CLI.
export { ProviderRecoveryError, recoverProviderTransaction } from "./provider-recovery.ts";
export type {
  ProviderRecoveryDependencies,
  ProviderRecoveryInput,
  ProviderRecoveryPendingReason,
  ProviderRecoveryResult,
  ProviderRecoveryState,
  ProviderRecoveryStateSnapshot,
} from "./provider-recovery.ts";
