import { GitHubActionsClient } from "./github.ts";
import {
  getCurrentGitHubIssueJob,
  parseGitHubIssueJobLedger,
  renderGitHubIssueJobLedger,
} from "./issues.ts";
import {
  evaluateIssueCompletionAction,
  type GitHubIssuePullRequestRecord,
  issueEvidenceMarker,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueSelectionReport,
  parseSentinelCycleReport,
  renderIssueDeliveryEvidence,
} from "./issue-delivery.ts";

const API_VERSION = "2022-11-28";
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_REVISION = /^[A-Za-z0-9_-]{1,200}$/u;
const COMPLETED_EVIDENCE_TEXT = "Delivered and verified in production; issue closed as completed.";

type PullRequest = Readonly<{
  number: number;
  state: "open" | "closed";
  mergedAt: string | null;
  headRef: string;
  headSha: string;
  baseRef: string;
}>;

type Comment = Readonly<{ id: number; body: string }>;

type IssueState = Readonly<{
  state: "open" | "closed";
  stateReason: string | null;
}>;

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required for Sentinel issue reconciliation`);
  return value;
};

const readJson = async (path: string): Promise<unknown> => JSON.parse(await Deno.readTextFile(path));

const optionalJson = async (path: string): Promise<unknown | null> => {
  try {
    return await readJson(path);
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return null;
    throw error;
  }
};

const record = (value: unknown): Record<string, unknown> | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;

const githubRequest = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit = {},
): Promise<unknown> => {
  const response = await fetch(`https://api.github.com/repos/${repository}${path}`, {
    ...init,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": API_VERSION,
      ...(init.body === undefined ? {} : { "Content-Type": "application/json" }),
      ...init.headers,
    },
    signal: AbortSignal.timeout(30_000),
  });
  const text = await response.text();
  if (!response.ok) {
    throw new Error(
      `GitHub API ${init.method ?? "GET"} ${path} failed with HTTP ${response.status}: ${text.slice(0, 500)}`,
    );
  }
  return text.length === 0 ? null : JSON.parse(text);
};

const parsePullRequest = (value: unknown): PullRequest => {
  const pull = record(value);
  const head = record(pull?.head);
  const base = record(pull?.base);
  if (
    !pull || !Number.isSafeInteger(pull.number) || (pull.number as number) <= 0 ||
    (pull.state !== "open" && pull.state !== "closed") ||
    (pull.merged_at !== null && typeof pull.merged_at !== "string") || !head || !base ||
    typeof head.ref !== "string" || typeof head.sha !== "string" || !FULL_SHA.test(head.sha) ||
    typeof base.ref !== "string"
  ) {
    throw new Error("GitHub returned an invalid pull request during issue reconciliation");
  }
  return {
    number: pull.number as number,
    state: pull.state,
    mergedAt: pull.merged_at as string | null,
    headRef: head.ref,
    headSha: head.sha,
    baseRef: base.ref,
  };
};

const waitForPullRequestSettlement = async (
  token: string,
  repository: string,
  expected: GitHubIssuePullRequestRecord,
): Promise<PullRequest> => {
  for (let attempt = 0; attempt < 30; attempt++) {
    const pull = parsePullRequest(
      await githubRequest(token, repository, `/pulls/${expected.pull_request_number}`),
    );
    if (
      pull.number !== expected.pull_request_number || pull.headRef !== expected.head_branch ||
      pull.headSha !== expected.head_sha || pull.baseRef !== expected.base_branch
    ) {
      throw new Error("Sentinel issue pull request changed identity before reconciliation");
    }
    if (pull.state === "closed" && pull.mergedAt !== null) return pull;
    if (attempt < 29) await new Promise((resolve) => setTimeout(resolve, 2_000));
  }
  return parsePullRequest(
    await githubRequest(token, repository, `/pulls/${expected.pull_request_number}`),
  );
};

const listComments = async (
  token: string,
  repository: string,
  issueNumber: number,
): Promise<Comment[]> => {
  const comments: Comment[] = [];
  for (let page = 1; page <= 10; page++) {
    const value = await githubRequest(
      token,
      repository,
      `/issues/${issueNumber}/comments?per_page=100&page=${page}`,
    );
    if (!Array.isArray(value)) throw new Error("GitHub issue-comment listing is invalid");
    for (const item of value) {
      const comment = record(item);
      if (
        !comment || !Number.isSafeInteger(comment.id) || (comment.id as number) <= 0 ||
        typeof comment.body !== "string"
      ) {
        throw new Error("GitHub returned an invalid issue comment");
      }
      comments.push({ id: comment.id as number, body: comment.body });
    }
    if (value.length < 100) return comments;
  }
  throw new Error("Sentinel issue-comment lookup exceeded its pagination limit");
};

const upsertComment = async (
  token: string,
  repository: string,
  issueNumber: number,
  marker: string,
  body: string,
): Promise<void> => {
  const matching = (await listComments(token, repository, issueNumber)).filter((comment) =>
    comment.body.includes(marker)
  );
  if (matching.length > 1) throw new Error("Sentinel evidence has more than one matching GitHub comment");
  if (matching.length === 1) {
    await githubRequest(token, repository, `/issues/comments/${matching[0]!.id}`, {
      method: "PATCH",
      body: JSON.stringify({ body }),
    });
  } else {
    await githubRequest(token, repository, `/issues/${issueNumber}/comments`, {
      method: "POST",
      body: JSON.stringify({ body }),
    });
  }
};

const getIssueState = async (
  token: string,
  repository: string,
  issueNumber: number,
): Promise<IssueState> => {
  const value = record(await githubRequest(token, repository, `/issues/${issueNumber}`));
  if (
    !value || (value.state !== "open" && value.state !== "closed") ||
    (value.state_reason !== null && typeof value.state_reason !== "string")
  ) {
    throw new Error("GitHub returned an invalid issue state");
  }
  return {
    state: value.state,
    stateReason: value.state_reason as string | null,
  };
};

const completionEvidence = async (
  token: string,
  repository: string,
  issueNumber: number,
  marker: string,
): Promise<string | null> => {
  const matching = (await listComments(token, repository, issueNumber)).filter((comment) =>
    comment.body.includes(marker) && comment.body.includes(COMPLETED_EVIDENCE_TEXT)
  );
  if (matching.length > 1) throw new Error("Sentinel completion evidence is duplicated");
  return matching[0]?.body ?? null;
};

const parseDisposition = (value: unknown): "resolved" | "manual_required" | null => {
  if (value === null) return null;
  const disposition = record(value);
  return disposition?.disposition === "resolved" || disposition?.disposition === "manual_required"
    ? disposition.disposition
    : null;
};

const parseOutcome = (value: unknown): Readonly<{
  outcome: "kept" | "rolled_back";
  candidateSha: string;
  candidateRevision: string | null;
}> | null => {
  if (value === null) return null;
  const outcome = record(value);
  if (
    !outcome || (outcome.outcome !== "kept" && outcome.outcome !== "rolled_back") ||
    typeof outcome.candidate_sha !== "string" || !FULL_SHA.test(outcome.candidate_sha) ||
    (outcome.candidate_revision !== null &&
      (typeof outcome.candidate_revision !== "string" || !SAFE_REVISION.test(outcome.candidate_revision)))
  ) {
    return null;
  }
  return {
    outcome: outcome.outcome,
    candidateSha: outcome.candidate_sha,
    candidateRevision: outcome.candidate_revision as string | null,
  };
};

const productionWorkflowEvidence = async (
  reportsDir: string,
): Promise<Readonly<{
  deploymentRunId: number | null;
  promotionRunId: number | null;
  monitoringDecision: "keep" | "rollback" | null;
}>> => {
  const workflow = record(await optionalJson(`${reportsDir}/production-deployment-workflow.json`));
  const decision = record(await optionalJson(`${reportsDir}/production-decision.json`));
  return {
    deploymentRunId: Number.isSafeInteger(workflow?.deployment_workflow_run_id) &&
        (workflow!.deployment_workflow_run_id as number) > 0
      ? workflow!.deployment_workflow_run_id as number
      : null,
    promotionRunId: Number.isSafeInteger(workflow?.promotion_workflow_run_id) &&
        (workflow!.promotion_workflow_run_id as number) > 0
      ? workflow!.promotion_workflow_run_id as number
      : null,
    monitoringDecision: decision?.decision === "keep" || decision?.decision === "rollback"
      ? decision.decision
      : null,
  };
};

const closeIssue = async (token: string, repository: string, issueNumber: number): Promise<void> => {
  const value = record(
    await githubRequest(token, repository, `/issues/${issueNumber}`, {
      method: "PATCH",
      body: JSON.stringify({ state: "closed", state_reason: "completed" }),
    }),
  );
  if (value?.state !== "closed" || value.state_reason !== "completed") {
    throw new Error("GitHub issue did not close as completed");
  }
};

const bytesToBase64 = (bytes: Uint8Array): string => {
  let value = "";
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    value += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(value);
};

const removeRolledBackLedgerEntry = async (
  token: string,
  repository: string,
  issueNumber: number,
  fingerprint: string,
): Promise<void> => {
  const path = "/contents/docs/sentinel-issue-jobs.md?ref=development";
  const value = record(await githubRequest(token, repository, path));
  if (
    !value || typeof value.sha !== "string" || !FULL_SHA.test(value.sha) ||
    typeof value.content !== "string" || value.encoding !== "base64"
  ) {
    throw new Error("GitHub returned an invalid Sentinel issue ledger blob");
  }
  const normalized = value.content.replaceAll("\n", "");
  const markdown = new TextDecoder("utf-8", { fatal: true }).decode(
    Uint8Array.from(atob(normalized), (character) => character.charCodeAt(0)),
  );
  const entries = parseGitHubIssueJobLedger(markdown);
  const retained = entries.filter((entry) =>
    !(entry.number === issueNumber && entry.fingerprint === fingerprint)
  );
  if (retained.length === entries.length) return;
  const content = bytesToBase64(new TextEncoder().encode(renderGitHubIssueJobLedger(retained)));
  await githubRequest(token, repository, "/contents/docs/sentinel-issue-jobs.md", {
    method: "PUT",
    body: JSON.stringify({
      message: `chore(sentinel): retry rolled-back issue #${issueNumber}`,
      content,
      sha: value.sha,
      branch: "development",
    }),
  });
};

const writeReconciliationReport = async (
  reportsDir: string,
  input: Readonly<{
    issueNumber: number;
    fingerprint: string;
    pullRequestNumber: number;
    pullRequestMerged: boolean;
    action: string;
    issueSnapshotMatches: boolean;
    durableCompletionEvidenceReused: boolean;
  }>,
): Promise<void> => {
  await Deno.writeTextFile(
    `${reportsDir}/github-issue-reconciliation.json`,
    `${JSON.stringify({
      schema_version: 1,
      issue_number: input.issueNumber,
      fingerprint: input.fingerprint,
      pull_request_number: input.pullRequestNumber,
      pull_request_merged: input.pullRequestMerged,
      action: input.action,
      issue_snapshot_matches: input.issueSnapshotMatches,
      durable_completion_evidence_reused: input.durableCompletionEvidenceReused,
    }, null, 2)}\n`,
    { mode: 0o600 },
  );
};

export const reconcileGitHubIssueDelivery = async (input: Readonly<{
  repositoryRoot: string;
  token: string;
  repository: string;
  workflowRunId: string;
  serverUrl: string;
  workflowFailed: boolean;
}>): Promise<void> => {
  const reportsDir = `${input.repositoryRoot}/.sentinel/reports`;
  const selectionValue = await optionalJson(`${reportsDir}/github-issue-selection.json`);
  if (selectionValue === null) return;
  const selection = parseGitHubIssueSelectionReport(selectionValue);
  const cycle = parseSentinelCycleReport(await readJson(`${reportsDir}/cycle.json`));
  const pullValue = await optionalJson(`${reportsDir}/github-issue-pull-request.json`);
  if (pullValue === null) {
    if (input.workflowFailed) return;
    throw new Error("A completed Sentinel issue cycle has no pull-request delivery record");
  }
  const pullRecord = parseGitHubIssuePullRequestRecord(pullValue);
  if (
    pullRecord.issue_number !== selection.issue_number ||
    pullRecord.fingerprint !== selection.fingerprint
  ) {
    throw new Error("Sentinel issue pull-request record does not match the selected issue snapshot");
  }

  const marker = issueEvidenceMarker(selection);
  const durableEvidence = await completionEvidence(
    input.token,
    input.repository,
    selection.issue_number,
    marker,
  );
  if (durableEvidence !== null) {
    const issueState = await getIssueState(
      input.token,
      input.repository,
      selection.issue_number,
    );
    if (issueState.state === "open") {
      await closeIssue(input.token, input.repository, selection.issue_number);
    } else if (issueState.stateReason !== "completed") {
      throw new Error("Sentinel completion evidence exists on an issue closed for a different reason");
    }
    await upsertComment(
      input.token,
      input.repository,
      pullRecord.pull_request_number,
      marker,
      durableEvidence,
    );
    await writeReconciliationReport(reportsDir, {
      issueNumber: selection.issue_number,
      fingerprint: selection.fingerprint,
      pullRequestNumber: pullRecord.pull_request_number,
      pullRequestMerged: true,
      action: "close_completed",
      issueSnapshotMatches: true,
      durableCompletionEvidenceReused: true,
    });
    return;
  }

  const pull = await waitForPullRequestSettlement(input.token, input.repository, pullRecord);
  const pullMerged = pull.state === "closed" && pull.mergedAt !== null;
  const disposition = parseDisposition(await optionalJson(`${reportsDir}/github-issue-disposition.json`));
  const outcome = parseOutcome(await optionalJson(`${reportsDir}/github-issue-production-outcome.json`));
  if (outcome && outcome.candidateSha !== pullRecord.head_sha) {
    throw new Error("Sentinel production outcome does not match the issue pull-request head");
  }

  let issueSnapshotMatches = false;
  if (
    !input.workflowFailed && disposition === "resolved" && outcome?.outcome === "kept" &&
    pullMerged
  ) {
    const current = await getCurrentGitHubIssueJob(
      new GitHubActionsClient({ repository: input.repository, token: input.token }),
      input.repository,
      selection.issue_number,
    );
    issueSnapshotMatches = current !== null && current.issueId === selection.issue_id &&
      current.fingerprint === selection.fingerprint && current.bodySha256 === selection.body_sha256 &&
      current.updatedAt === selection.updated_at;
  }
  const action = evaluateIssueCompletionAction({
    hasSelection: true,
    workflowFailed: input.workflowFailed,
    disposition,
    outcome: outcome?.outcome ?? null,
    pullRequestMerged: pullMerged,
    issueSnapshotMatches,
  });
  const workflowEvidence = await productionWorkflowEvidence(reportsDir);
  const workflowRunUrl = `${input.serverUrl}/${input.repository}/actions/runs/${input.workflowRunId}`;
  const evidence = renderIssueDeliveryEvidence({
    repository: input.repository,
    selection,
    pullRequest: pullRecord,
    workflowRunUrl,
    action,
    cycleStatus: cycle.status,
    candidateSha: pullRecord.head_sha,
    productionRevision: outcome?.candidateRevision ?? null,
    deploymentWorkflowRunId: workflowEvidence.deploymentRunId,
    promotionWorkflowRunId: workflowEvidence.promotionRunId,
    monitoringDecision: workflowEvidence.monitoringDecision,
  });

  await upsertComment(
    input.token,
    input.repository,
    pullRecord.pull_request_number,
    marker,
    evidence,
  );
  if (action === "close_completed") {
    // The issue evidence is the durable retry checkpoint. Persist it before the irreversible close.
    await upsertComment(
      input.token,
      input.repository,
      selection.issue_number,
      marker,
      evidence,
    );
    await closeIssue(input.token, input.repository, selection.issue_number);
  } else if (action === "leave_open_rolled_back") {
    await removeRolledBackLedgerEntry(
      input.token,
      input.repository,
      selection.issue_number,
      selection.fingerprint,
    );
  }

  await writeReconciliationReport(reportsDir, {
    issueNumber: selection.issue_number,
    fingerprint: selection.fingerprint,
    pullRequestNumber: pullRecord.pull_request_number,
    pullRequestMerged: pullMerged,
    action,
    issueSnapshotMatches,
    durableCompletionEvidenceReused: false,
  });
};

if (import.meta.main) {
  const repositoryRoot = Deno.cwd();
  const workflowOutcome = Deno.env.get("SENTINEL_WORKFLOW_OUTCOME")?.trim();
  await reconcileGitHubIssueDelivery({
    repositoryRoot,
    token: requiredEnvironment("GITHUB_TOKEN"),
    repository: requiredEnvironment("GITHUB_REPOSITORY"),
    workflowRunId: requiredEnvironment("GITHUB_RUN_ID"),
    serverUrl: Deno.env.get("GITHUB_SERVER_URL")?.trim() || "https://github.com",
    workflowFailed: workflowOutcome === "failure" || workflowOutcome === "cancelled" ||
      workflowOutcome === "timed_out",
  });
}
