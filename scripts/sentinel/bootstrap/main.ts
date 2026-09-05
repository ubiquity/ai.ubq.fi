import {
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelBootstrapHealthSignal,
  parseSentinelBootstrapProgressObservation,
  type SentinelBootstrapHealthSignalV1,
  type SentinelBootstrapProgressObservationV1,
  type SentinelBootstrapReleaseRecordV1,
} from "./contracts.ts";
import { createGitHubSentinelBootstrapState, type GitHubSentinelBootstrapState } from "./github-store.ts";
import {
  reconcileSentinelBootstrap,
  type SentinelBootstrapControllerDependencies,
  type SentinelBootstrapReconcileInput,
  type SentinelBootstrapReconcileOutcome,
} from "./controller.ts";
import { parseBootstrapEnvironment, SENTINEL_BOOTSTRAP_POLICY } from "./policy.ts";
import { parseAdvisoryRecoveryLedgerSummary, SENTINEL_RECOVERY_LEDGER_PATH } from "./recovery-ledger-summary.ts";

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_REVISION = /^[A-Za-z0-9._-]{1,128}$/u;
const HEALTHY_CANDIDATE_ACCEPTANCE_MS = 60 * 60 * 1_000;
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

const observeManagedRelease = async (fetcher: typeof fetch): Promise<Readonly<{ sha: string; revision: string }>> => {
  const response = await fetcher("https://ai-ubq-fi.ubiquity-dao.deno.net/health", {
    signal: AbortSignal.timeout(20_000),
  });
  const value = await response.json().catch(() => null) as Record<string, unknown> | null;
  const release = value && typeof value.release === "object" && value.release !== null
    ? value.release as Record<string, unknown>
    : null;
  if (
    response.status !== 200 || value?.status !== "available" || typeof release?.git_sha !== "string" ||
    !FULL_SHA.test(release.git_sha) || typeof release.deployment_id !== "string" ||
    !SAFE_REVISION.test(release.deployment_id)
  ) throw new Error("Managed Sentinel health identity is invalid");
  return { sha: release.git_sha, revision: release.deployment_id };
};

export const synchronizeObservedRelease = async (
  state: GitHubSentinelBootstrapState,
  observed: Readonly<{ sha: string; revision: string }>,
  now: string,
): Promise<SentinelBootstrapReleaseRecordV1> => {
  const current = state.readDocument();
  if (current.release === null) {
    const initial = parseBootstrapReleaseRecord({
      schema_version: 1,
      stable_sha: observed.sha,
      candidate_sha: null,
      acceptance_evidence: [`health:${observed.revision}`],
      activated_at: now,
      rollback_reason: null,
      generation: 1,
    });
    await state.replaceRelease(initial);
    return initial;
  }
  // Preserve the release/activation identities that own a durable rollback
  // intent. The controller must finish those side effects before observation
  // can advance or normalize either record.
  if (current.rollback_intent !== null) return current.release;
  if (current.release.candidate_sha === observed.sha) {
    const currentGenerationHasFailure = current.signals.some((signal) =>
      signal.generation === current.release!.generation
    );
    if (
      !currentGenerationHasFailure &&
      Date.parse(now) - Date.parse(current.release.activated_at) >= HEALTHY_CANDIDATE_ACCEPTANCE_MS
    ) {
      const accepted = parseBootstrapReleaseRecord({
        ...current.release,
        stable_sha: observed.sha,
        candidate_sha: null,
        acceptance_evidence: [
          ...current.release.acceptance_evidence,
          `health:${observed.revision}`,
          "bootstrap:healthy-window",
        ].slice(-8),
        activated_at: now,
      });
      await state.replaceRelease(accepted);
      return accepted;
    }
    return current.release;
  }
  if (current.release.stable_sha === observed.sha) {
    if (
      current.release.candidate_sha !== null && current.activation !== null &&
      current.activation.active_sha === current.release.stable_sha &&
      current.activation.generation > current.release.generation
    ) {
      const rolledBack = parseBootstrapReleaseRecord({
        ...current.release,
        candidate_sha: null,
        acceptance_evidence: [
          ...current.release.acceptance_evidence,
          `health:${observed.revision}`,
          "bootstrap:rollback-confirmed",
        ].slice(-8),
        activated_at: now,
        rollback_reason: "authoritative_failure_rollback",
        generation: current.activation.generation,
      });
      await state.replaceRelease(rolledBack);
      return rolledBack;
    }
    return current.release;
  }
  if (current.activation === null) {
    throw new Error("Managed Sentinel release changed before the prior candidate was reconciled");
  }
  const generation = current.activation.generation + 1;
  const supersededCandidate = current.release.candidate_sha;
  const next = parseBootstrapReleaseRecord({
    schema_version: 1,
    stable_sha: current.release.stable_sha,
    candidate_sha: observed.sha,
    acceptance_evidence: [
      `health:${observed.revision}`,
      ...(supersededCandidate === null ? [] : [`bootstrap:superseded:${supersededCandidate}`]),
    ],
    activated_at: now,
    rollback_reason: null,
    generation,
  });
  const activation = parseSentinelBootstrapActivationPointer({
    schema_version: 1,
    active_sha: observed.sha,
    generation,
    fenced_generations: supersededCandidate === null
      ? current.activation.fenced_generations
      : [...current.activation.fenced_generations, current.activation.generation].slice(-64),
    updated_at: now,
    reason: supersededCandidate === null ? "managed_health_observed" : "managed_candidate_superseded",
  });
  await state.replaceRelease(next, activation);
  return next;
};

/**
 * The workflow invokes this entry point from the protected development ref.
 * State, observation, and bounded recovery-dispatch wiring is supplied by the
 * orchestrator through the typed controller function. The entry point cannot
 * merge, deploy, promote, or edit repository history.
 */
export const runProtectedBootstrap = async (
  input: SentinelBootstrapReconcileInput,
  dependencies: SentinelBootstrapControllerDependencies,
): Promise<SentinelBootstrapReconcileOutcome> => {
  const environment = parseBootstrapEnvironment();
  return await reconcileSentinelBootstrap({
    ...input,
    repository: environment.repository,
    ref: environment.ref,
  }, dependencies);
};

if (import.meta.main) {
  const environment = parseBootstrapEnvironment();
  const token = requiredEnvironment("GITHUB_TOKEN");
  const state = await createGitHubSentinelBootstrapState({ token, repository: environment.repository });
  const now = new Date().toISOString();
  const observed = await observeManagedRelease(fetch);
  const release = await synchronizeObservedRelease(state, observed, now);
  const generation = state.readDocument().activation?.generation ?? release.generation;
  await state.appendSignals(
    await collectAuthoritativeStartupSignals(token, environment.repository, observed.sha, generation),
  );
  const progressObservations = await collectBootstrapProgressObservations(
    token,
    environment.repository,
    observed.sha,
    generation,
  );
  const outcome = await runProtectedBootstrap({
    release,
    signals: state.readDocument().signals,
    progressObservations,
  }, {
    store: state.store,
    now: () => now,
    dispatchRecovery: () => dispatchStableRecovery(token, environment.repository),
  });
  // Advisory evidence only: the decision is recorded but never changes the
  // activation/rollback identity or the exact-SHA/revision promotion gates.
  if (outcome.progress !== null) {
    await state.replaceProgress(outcome.progress);
  }
  console.log(JSON.stringify({
    schema_version: 1,
    repository: environment.repository,
    execution_sha: environment.sha,
    run_id: environment.runId,
    ...outcome,
  }));
}
