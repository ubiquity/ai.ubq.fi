import {
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelBootstrapHealthSignal,
  type SentinelBootstrapHealthSignalV1,
  type SentinelBootstrapReleaseRecordV1,
} from "./contracts.ts";
import { createGitHubSentinelBootstrapState, type GitHubSentinelBootstrapState } from "./github-store.ts";
import {
  reconcileSentinelBootstrap,
  type SentinelBootstrapControllerDependencies,
  type SentinelBootstrapReconcileInput,
  type SentinelBootstrapReconcileOutcome,
} from "./controller.ts";
import { parseBootstrapEnvironment } from "./policy.ts";

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

const synchronizeObservedRelease = async (
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
  if (current.release.candidate_sha !== null || current.activation === null) {
    throw new Error("Managed Sentinel release changed before the prior candidate was reconciled");
  }
  const generation = current.activation.generation + 1;
  const next = parseBootstrapReleaseRecord({
    schema_version: 1,
    stable_sha: current.release.stable_sha,
    candidate_sha: observed.sha,
    acceptance_evidence: [`health:${observed.revision}`],
    activated_at: now,
    rollback_reason: null,
    generation,
  });
  const activation = parseSentinelBootstrapActivationPointer({
    schema_version: 1,
    active_sha: observed.sha,
    generation,
    fenced_generations: current.activation.fenced_generations,
    updated_at: now,
    reason: "managed_health_observed",
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
  const outcome = await runProtectedBootstrap({
    release,
    signals: state.readDocument().signals,
  }, {
    store: state.store,
    now: () => now,
    dispatchRecovery: () => dispatchStableRecovery(token, environment.repository),
  });
  console.log(JSON.stringify({
    schema_version: 1,
    repository: environment.repository,
    execution_sha: environment.sha,
    run_id: environment.runId,
    ...outcome,
  }));
}
