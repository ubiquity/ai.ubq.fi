import {
  parseSentinelBootstrapHealthSignal,
  parseSentinelBootstrapProgressObservation,
  type SentinelBootstrapHealthSignalV1,
  type SentinelBootstrapProgressObservationV1,
} from "./contracts.ts";
import { createGitHubSentinelBootstrapState, type GitHubSentinelBootstrapState } from "./github-store.ts";
import {
  reconcileSentinelBootstrap,
  type SentinelBootstrapControllerDependencies,
  type SentinelBootstrapReconcileOutcome,
} from "./controller.ts";
import { initialSentinelBootstrapActivation } from "./activation.ts";
import { type BootstrapEnvironment, parseBootstrapEnvironment, SENTINEL_BOOTSTRAP_POLICY } from "./policy.ts";
import { parseAdvisoryRecoveryLedgerSummary, SENTINEL_RECOVERY_LEDGER_PATH } from "./recovery-ledger-summary.ts";

const AUTHORITATIVE_STARTUP_STEPS = new Set([
  "Require ciphertext-only artifact policy",
  "Select immutable run mode",
]);

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required`);
  return value;
};

const sha256 = async (value: string): Promise<string> =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value))))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");

const githubJson = async (token: string, repository: string, path: string): Promise<Record<string, unknown>> => {
  const response = await fetch(`https://api.github.com/repos/${repository}${path}`, {
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": "2022-11-28",
    },
    signal: AbortSignal.timeout(20_000),
  });
  const value = await response.json().catch(() => null);
  if (!response.ok || typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`Bootstrap GitHub observation failed with HTTP ${response.status}`);
  }
  return value as Record<string, unknown>;
};

const collectAuthoritativeStartupSignals = async (
  token: string,
  repository: string,
  activeSha: string,
  generation: number,
): Promise<readonly SentinelBootstrapHealthSignalV1[]> => {
  const runsValue = await githubJson(
    token,
    repository,
    "/actions/workflows/provider-sentinel.yml/runs?branch=development&status=completed&per_page=10",
  );
  if (!Array.isArray(runsValue.workflow_runs)) throw new Error("Bootstrap workflow-run observation is invalid");
  const signals: SentinelBootstrapHealthSignalV1[] = [];
  for (const value of runsValue.workflow_runs) {
    if (typeof value !== "object" || value === null || Array.isArray(value)) continue;
    const run = value as Record<string, unknown>;
    if (
      run.conclusion !== "failure" || run.head_sha !== activeSha || !Number.isSafeInteger(run.id) ||
      typeof run.run_started_at !== "string" || !Number.isFinite(Date.parse(run.run_started_at))
    ) continue;
    const jobsValue = await githubJson(token, repository, `/actions/runs/${run.id}/jobs?per_page=100`);
    if (!Array.isArray(jobsValue.jobs)) throw new Error("Bootstrap workflow-job observation is invalid");
    for (const jobValue of jobsValue.jobs) {
      if (typeof jobValue !== "object" || jobValue === null || Array.isArray(jobValue)) continue;
      const job = jobValue as Record<string, unknown>;
      if (!Array.isArray(job.steps)) continue;
      for (const stepValue of job.steps) {
        if (typeof stepValue !== "object" || stepValue === null || Array.isArray(stepValue)) continue;
        const step = stepValue as Record<string, unknown>;
        if (
          step.conclusion !== "failure" || typeof step.name !== "string" || !AUTHORITATIVE_STARTUP_STEPS.has(step.name)
        ) {
          continue;
        }
        signals.push(parseSentinelBootstrapHealthSignal({
          schema_version: 1,
          generation,
          failure_class: "workflow_failure",
          failure_fingerprint: await sha256(`workflow_failure\n${activeSha}\n${step.name}`),
          observed_at: run.run_started_at,
          evidence_refs: [`run:${run.id}`],
          observation_id: `run-${run.id}`,
        }));
      }
    }
  }
  return signals;
};

/**
 * Reads the recovery ledger from the sentinel recovery-state ref when it is
 * available. A missing ledger is not an error: ledger/state version and retry
 * state are used only "where available".
 */
const fetchRecoveryLedger = async (token: string, repository: string): Promise<unknown | null> => {
  const response = await fetch(
    `https://api.github.com/repos/${repository}/contents/${SENTINEL_RECOVERY_LEDGER_PATH}?ref=sentinel/recovery-state`,
    {
      headers: {
        Accept: "application/vnd.github+json",
        Authorization: `Bearer ${token}`,
        "X-GitHub-Api-Version": "2022-11-28",
      },
      signal: AbortSignal.timeout(20_000),
    },
  );
  if (response.status === 404) return null;
  const value = await response.json().catch(() => null) as Record<string, unknown> | null;
  if (!response.ok || typeof value?.content !== "string") {
    throw new Error(`Bootstrap recovery ledger observation failed with HTTP ${response.status}`);
  }
  try {
    return JSON.parse(
      new TextDecoder("utf-8", { fatal: true }).decode(
        Uint8Array.from(atob(value.content.replaceAll("\n", "")), (character) => character.charCodeAt(0)),
      ),
    );
  } catch {
    throw new Error("Bootstrap recovery ledger is invalid");
  }
};

const normalizeProgressMilestone = (stepName: string): string => {
  const segment = stepName
    .replace(/[^A-Za-z0-9._-]+/gu, "-")
    .replace(/^-+|-+$/gu, "")
    .toLowerCase()
    .slice(0, 128);
  return `step:${segment.length > 0 ? segment : "unknown-step"}`;
};

/**
 * Builds canonical per-run progress observations from the completed
 * provider-sentinel runs of the observed revision plus the recovery ledger
 * (when available). Every field comes from immutable facts; run identity
 * lives in `run_id`/`source`, not in the durable state key.
 */
const collectBootstrapProgressObservations = async (
  token: string,
  repository: string,
  activeSha: string,
  generation: number,
): Promise<readonly SentinelBootstrapProgressObservationV1[]> => {
  const runsValue = await githubJson(
    token,
    repository,
    "/actions/workflows/provider-sentinel.yml/runs?branch=development&status=completed&per_page=10",
  );
  if (!Array.isArray(runsValue.workflow_runs)) throw new Error("Bootstrap workflow-run observation is invalid");
  const ledgerValue = await fetchRecoveryLedger(token, repository).catch(() => null);
  const summary = ledgerValue === null ? null : parseAdvisoryRecoveryLedgerSummary(ledgerValue);
  const ledgerVersion = summary === null ? null : summary.max_state_version;
  const retryState = summary === null ? null : summary.retry_state;
  const runs = runsValue.workflow_runs
    .filter((value): value is Record<string, unknown> =>
      typeof value === "object" && value !== null && !Array.isArray(value) &&
      (value.conclusion === "success" || value.conclusion === "failure") &&
      value.head_sha === activeSha && Number.isSafeInteger(value.id) &&
      typeof value.run_started_at === "string" && Number.isFinite(Date.parse(value.run_started_at))
    )
    .sort((left, right) => Date.parse(String(left.run_started_at)) - Date.parse(String(right.run_started_at)))
    .slice(-8);
  const observations: SentinelBootstrapProgressObservationV1[] = [];
  for (const run of runs) {
    let milestone = "run:completed";
    let failureFingerprint: string | null = null;
    if (run.conclusion === "failure") {
      const failedStep = await collectFirstFailedStep(token, repository, run.id as number);
      milestone = normalizeProgressMilestone(failedStep ?? "unknown-step");
      failureFingerprint = await sha256(`workflow_failure\n${activeSha}\n${failedStep ?? "unknown"}`);
    }
    observations.push(parseSentinelBootstrapProgressObservation({
      schema_version: 1,
      run_id: `run:${run.id}`,
      // Bounded reference: the workflow basename, not the leading-dot path.
      source: SENTINEL_BOOTSTRAP_POLICY.evolvingWorkflow.split("/").pop() ?? "provider-sentinel.yml",
      generation,
      phase: run.conclusion === "failure" ? "failed" : "completed",
      milestone,
      failure_fingerprint: failureFingerprint,
      git_sha: run.head_sha === null ? null : run.head_sha,
      ledger_version: ledgerVersion,
      retry_state: retryState,
      verification_evidence: run.conclusion === "failure" ? null : "verification:run-complete",
    }));
  }
  return observations;
};

const collectFirstFailedStep = async (
  token: string,
  repository: string,
  runId: number,
): Promise<string | null> => {
  const jobsValue = await githubJson(token, repository, `/actions/runs/${runId}/jobs?per_page=100`);
  if (!Array.isArray(jobsValue.jobs)) throw new Error("Bootstrap workflow-job observation is invalid");
  for (const jobValue of jobsValue.jobs) {
    if (typeof jobValue !== "object" || jobValue === null || Array.isArray(jobValue)) continue;
    const job = jobValue as Record<string, unknown>;
    if (!Array.isArray(job.steps)) continue;
    for (const stepValue of job.steps) {
      if (typeof stepValue !== "object" || stepValue === null || Array.isArray(stepValue)) continue;
      const step = stepValue as Record<string, unknown>;
      if (step.conclusion !== "failure" || typeof step.name !== "string") continue;
      return step.name;
    }
  }
  return null;
};

const dispatchStableRecovery = async (token: string, repository: string): Promise<void> => {
  const response = await fetch(
    `https://api.github.com/repos/${repository}/actions/workflows/provider-sentinel.yml/dispatches`,
    {
      method: "POST",
      headers: {
        Accept: "application/vnd.github+json",
        Authorization: `Bearer ${token}`,
        "Content-Type": "application/json",
        "X-GitHub-Api-Version": "2022-11-28",
      },
      body: JSON.stringify({ ref: "development", inputs: { sentinel_mode: "hourly" } }),
      signal: AbortSignal.timeout(20_000),
    },
  );
  if (response.status !== 204) {
    await response.body?.cancel().catch(() => undefined);
    throw new Error(`Bootstrap recovery dispatch failed with HTTP ${response.status}`);
  }
};

export type SentinelBootstrapCycleInput = Readonly<{
  environment: BootstrapEnvironment;
  state: GitHubSentinelBootstrapState;
  adapters: Readonly<{
    now: () => string;
    collectStartupSignals: (
      activeSha: string,
      generation: number,
    ) => Promise<readonly SentinelBootstrapHealthSignalV1[]>;
    collectProgressObservations: (
      activeSha: string,
      generation: number,
    ) => Promise<readonly SentinelBootstrapProgressObservationV1[]>;
    dispatchRecovery: NonNullable<SentinelBootstrapControllerDependencies["dispatchRecovery"]>;
  }>;
}>;

export type SentinelBootstrapCycleResult = Readonly<
  | { status: "uninitialized"; reason: "durable_release_missing" }
  | { status: "reconciled"; outcome: SentinelBootstrapReconcileOutcome }
>;

/**
 * One state-read-to-reconcile bootstrap cycle. The workflow invokes this
 * entry point from the frozen protected bootstrap package, never from the
 * evolving provider sources or a surrounding config. Controller identity is
 * selected exclusively from the durable release/activation record: provider
 * /health and elapsed healthy time can never initialize, supersede, or
 * accept a controller, and startup/progress/recovery work without any
 * provider health request. State, observation, and bounded recovery-dispatch
 * wiring is supplied by the orchestrator through the typed adapters. The
 * entry point cannot merge application changes or deploy/promote releases.
 */
export const runBootstrapCycle = async (
  input: SentinelBootstrapCycleInput,
): Promise<SentinelBootstrapCycleResult> => {
  const document = input.state.readDocument();
  if (document.release === null) {
    return { status: "uninitialized", reason: "durable_release_missing" };
  }
  const now = input.adapters.now();
  const selected = document.activation ?? initialSentinelBootstrapActivation(document.release, now);
  await input.state.appendSignals(
    await input.adapters.collectStartupSignals(selected.active_sha, selected.generation),
  );
  const progressObservations = await input.adapters.collectProgressObservations(
    selected.active_sha,
    selected.generation,
  );
  const outcome = await reconcileSentinelBootstrap({
    release: document.release,
    signals: input.state.readDocument().signals,
    progressObservations,
    repository: input.environment.repository,
    ref: input.environment.ref,
    expectedFence: { generation: selected.generation, activeSha: selected.active_sha },
  }, {
    store: input.state.store,
    now: () => now,
    dispatchRecovery: input.adapters.dispatchRecovery,
  });
  // Advisory evidence only: the decision is recorded but never changes the
  // activation/rollback identity or the exact-SHA/revision promotion gates.
  if (outcome.progress !== null) {
    await input.state.replaceProgress(outcome.progress);
  }
  return { status: "reconciled", outcome };
};

if (import.meta.main) {
  const environment = parseBootstrapEnvironment();
  const token = requiredEnvironment("GITHUB_TOKEN");
  const state = await createGitHubSentinelBootstrapState({ token, repository: environment.repository });
  const result = await runBootstrapCycle({
    environment,
    state,
    adapters: {
      now: () => new Date().toISOString(),
      collectStartupSignals: (activeSha, generation) =>
        collectAuthoritativeStartupSignals(token, environment.repository, activeSha, generation),
      collectProgressObservations: (activeSha, generation) =>
        collectBootstrapProgressObservations(token, environment.repository, activeSha, generation),
      dispatchRecovery: () => dispatchStableRecovery(token, environment.repository),
    },
  });
  if (result.status === "uninitialized") {
    Deno.exitCode = 1;
  }
  console.log(JSON.stringify({
    schema_version: 1,
    repository: environment.repository,
    execution_sha: environment.sha,
    run_id: environment.runId,
    ...(result.status === "reconciled" ? result.outcome : { status: result.status, reason: result.reason }),
  }));
}
