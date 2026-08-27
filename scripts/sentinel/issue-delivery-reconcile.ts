import { GitHubActionsClient, type GitHubIssue } from "./github.ts";
import {
  getCurrentGitHubIssueJob,
  type GitHubIssueJobSource,
  parseGitHubIssueJobLedger,
  renderGitHubIssueJobLedger,
} from "./issues.ts";
import {
  evaluateIssueCompletionAction,
  type GitHubIssuePullRequestRecord,
  type GitHubIssueSelectionReport,
  isContainedDevelopmentComparison,
  isPullRequestMergeRefusalStatus,
  ISSUE_COMPLETION_EVIDENCE_TEXT,
  issueEvidenceMarker,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueSelectionReport,
  parseSentinelCycleReport,
  renderIssueDeliveryEvidence,
} from "./issue-delivery.ts";

const API_VERSION = "2022-11-28";
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_REVISION = /^[A-Za-z0-9_-]{1,200}$/u;

type PullRequest = Readonly<{
  number: number;
  state: "open" | "closed";
  mergedAt: string | null;
  headRef: string;
  headSha: string;
  baseRef: string;
}>;

type Comment = Readonly<{ id: number; body: string; updatedAt: string }>;

type IssueState = Readonly<{
  state: "open" | "closed";
  stateReason: string | null;
}>;

type PullRequestMergeSource = "sentinel_merge_api" | "development_content" | "already_merged" | null;
const MAX_COMMENT_TIMESTAMP_PROPAGATION_MS = 5_000;

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

const githubRequestRaw = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit = {},
  fetcher: typeof fetch = fetch,
): Promise<Readonly<{ status: number; payload: unknown }>> => {
  const response = await fetcher(`https://api.github.com/repos/${repository}${path}`, {
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
  let payload: unknown = null;
  if (text.length > 0) {
    try {
      payload = JSON.parse(text);
    } catch {
      payload = null;
    }
  }
  return { status: response.status, payload };
};

const githubRequest = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit = {},
  fetcher: typeof fetch = fetch,
): Promise<unknown> => {
  const { status, payload } = await githubRequestRaw(token, repository, path, init, fetcher);
  if (status < 200 || status >= 300) {
    const formatted = typeof payload === "string" ? payload : JSON.stringify(payload);
    throw new Error(
      `GitHub API ${init.method ?? "GET"} ${path} failed with HTTP ${status}: ${
        formatted === "null" ? "" : formatted.slice(0, 500)
      }`,
    );
  }
  return payload;
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

const fetchIssuePullRequest = async (
  token: string,
  repository: string,
  expected: GitHubIssuePullRequestRecord,
  fetcher: typeof fetch,
): Promise<PullRequest> => {
  const pull = parsePullRequest(
    await githubRequest(token, repository, `/pulls/${expected.pull_request_number}`, {}, fetcher),
  );
  if (
    pull.number !== expected.pull_request_number || pull.headRef !== expected.head_branch ||
    pull.headSha !== expected.head_sha || pull.baseRef !== expected.base_branch
  ) {
    throw new Error("Sentinel issue pull request changed identity before reconciliation");
  }
  return pull;
};

export const mergeDeliveryPullRequest = async (
  token: string,
  repository: string,
  expected: GitHubIssuePullRequestRecord,
  fetcher: typeof fetch = fetch,
): Promise<Readonly<{ source: "sentinel_merge_api" | "development_content" | "already_merged" }>> => {
  const pull = await fetchIssuePullRequest(token, repository, expected, fetcher);
  if (pull.state === "closed" && pull.mergedAt !== null) return { source: "already_merged" };
  const response = await githubRequestRaw(token, repository, `/pulls/${expected.pull_request_number}/merge`, {
    method: "PUT",
    body: JSON.stringify({
      merge_method: "merge",
      commit_title: `merge: Provider Sentinel deliverable for #${expected.issue_number}`,
      sha: expected.head_sha,
    }),
  }, fetcher);
  const mergedPayload = record(response.payload);
  if (response.status === 200 && mergedPayload?.merged === true) return { source: "sentinel_merge_api" };
  if (isPullRequestMergeRefusalStatus(response.status)) {
    // A refusal can race with a force-push or other head update after the
    // initial identity check. Revalidate before treating the recorded head as
    // already delivered through development.
    await fetchIssuePullRequest(token, repository, expected, fetcher);
    // The candidate commits were pushed directly to development, so the head
    // may already be contained in the base. GitHub can refuse the API merge in
    // that state (or under branch protection); the comparison decides whether
    // the delivery is genuinely already integrated.
    const comparison = record(
      await githubRequest(token, repository, `/compare/development...${expected.head_sha}`, {}, fetcher),
    );
    const compareStatus = typeof comparison?.status === "string" ? comparison.status : null;
    if (compareStatus !== null && isContainedDevelopmentComparison(compareStatus)) {
      return { source: "development_content" };
    }
  }
  throw new Error(
    `Sentinel merge of pull request #${expected.pull_request_number} failed with HTTP ${response.status}`,
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
        typeof comment.body !== "string" || typeof comment.updated_at !== "string" ||
        !Number.isFinite(Date.parse(comment.updated_at))
      ) {
        throw new Error("GitHub returned an invalid issue comment");
      }
      comments.push({ id: comment.id as number, body: comment.body, updatedAt: comment.updated_at });
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
): Promise<Comment | null> => {
  const matching = (await listComments(token, repository, issueNumber)).filter((comment) =>
    comment.body.includes(marker) && comment.body.includes(ISSUE_COMPLETION_EVIDENCE_TEXT)
  );
  if (matching.length > 1) throw new Error("Sentinel completion evidence is duplicated");
  return matching[0] ?? null;
};

export const completionEvidenceSnapshotMatches = async (
  source: GitHubIssueJobSource,
  repository: string,
  selection: GitHubIssueSelectionReport,
  issue: GitHubIssue,
  evidenceUpdatedAt: string,
): Promise<boolean> => {
  const evidenceUpdatedMs = Date.parse(evidenceUpdatedAt);
  const issueUpdatedMs = Date.parse(issue.updatedAt);
  if (
    issue.state !== "open" || issue.number !== selection.issue_number || issue.comments !== 1 ||
    !Number.isFinite(evidenceUpdatedMs) || !Number.isFinite(issueUpdatedMs) ||
    issueUpdatedMs < evidenceUpdatedMs || issueUpdatedMs - evidenceUpdatedMs > MAX_COMMENT_TIMESTAMP_PROPAGATION_MS
  ) return false;
  const normalizedIssue: GitHubIssue = {
    ...issue,
    comments: 0,
    updatedAt: selection.updated_at,
  };
  const normalizedSource: GitHubIssueJobSource = {
    listOpenIssues: () => source.listOpenIssues(),
    getIssue: (issueNumber) =>
      issueNumber === selection.issue_number ? Promise.resolve(normalizedIssue) : source.getIssue(issueNumber),
    getIssueRelations: (issueNumber) => source.getIssueRelations(issueNumber),
    getRepositoryPermission: (username) => source.getRepositoryPermission(username),
  };
  const current = await getCurrentGitHubIssueJob(
    normalizedSource,
    repository,
    selection.issue_number,
  );
  return current !== null && current.issueId === selection.issue_id &&
    current.fingerprint === selection.fingerprint && current.bodySha256 === selection.body_sha256 &&
    current.updatedAt === selection.updated_at;
};

const parseDisposition = (value: unknown): "resolved" | "manual_required" | null => {
  if (value === null) return null;
  const disposition = record(value);
  return disposition?.disposition === "resolved" || disposition?.disposition === "manual_required"
    ? disposition.disposition
    : null;
};

const parseOutcome = (value: unknown):
  | Readonly<{
    outcome: "kept" | "rolled_back";
    candidateSha: string;
    candidateRevision: string | null;
  }>
  | null => {
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
): Promise<
  Readonly<{
    deploymentRunId: number | null;
    promotionRunId: number | null;
    monitoringDecision: "keep" | "rollback" | null;
  }>
> => {
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
    monitoringDecision: decision?.decision === "keep" || decision?.decision === "rollback" ? decision.decision : null,
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
  const retained = entries.filter((entry) => !(entry.number === issueNumber && entry.fingerprint === fingerprint));
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
    mergeSource: PullRequestMergeSource;
    action: string;
    issueSnapshotMatches: boolean;
    durableCompletionEvidenceReused: boolean;
  }>,
): Promise<void> => {
  await Deno.writeTextFile(
    `${reportsDir}/github-issue-reconciliation.json`,
    `${
      JSON.stringify(
        {
          schema_version: 1,
          issue_number: input.issueNumber,
          fingerprint: input.fingerprint,
          pull_request_number: input.pullRequestNumber,
          pull_request_merged: input.pullRequestMerged,
          merge_source: input.mergeSource,
          action: input.action,
          issue_snapshot_matches: input.issueSnapshotMatches,
          durable_completion_evidence_reused: input.durableCompletionEvidenceReused,
        },
        null,
        2,
      )
    }\n`,
    { mode: 0o600 },
  );
};

export const reconcileGitHubIssueDelivery = async (
  input: Readonly<{
    repositoryRoot: string;
    token: string;
    repository: string;
    workflowRunId: string;
    serverUrl: string;
    workflowFailed: boolean;
  }>,
): Promise<void> => {
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
    const source = new GitHubActionsClient({ repository: input.repository, token: input.token });
    let issue = await source.getIssue(selection.issue_number);
    const requireMatchingOpenSnapshot = async (): Promise<void> => {
      if (
        !await completionEvidenceSnapshotMatches(
          source,
          input.repository,
          selection,
          issue,
          durableEvidence.updatedAt,
        )
      ) {
        throw new Error("Sentinel completion evidence no longer matches the open issue snapshot");
      }
    };
    if (issue.state === "open") await requireMatchingOpenSnapshot();
    const merge = await mergeDeliveryPullRequest(input.token, input.repository, pullRecord);
    // The merge can take long enough for the issue body, metadata, comments,
    // or relationships to change. Fetch and validate the full snapshot again
    // immediately before the irreversible close.
    issue = await source.getIssue(selection.issue_number);
    if (issue.state === "open") {
      await requireMatchingOpenSnapshot();
      await closeIssue(input.token, input.repository, selection.issue_number);
    } else {
      const issueState = await getIssueState(
        input.token,
        input.repository,
        selection.issue_number,
      );
      if (issueState.stateReason !== "completed") {
        throw new Error("Sentinel completion evidence exists on an issue closed for a different reason");
      }
    }
    await upsertComment(
      input.token,
      input.repository,
      pullRecord.pull_request_number,
      marker,
      durableEvidence.body,
    );
    await writeReconciliationReport(reportsDir, {
      issueNumber: selection.issue_number,
      fingerprint: selection.fingerprint,
      pullRequestNumber: pullRecord.pull_request_number,
      pullRequestMerged: true,
      mergeSource: merge.source,
      action: "close_completed",
      issueSnapshotMatches: true,
      durableCompletionEvidenceReused: true,
    });
    return;
  }

  const disposition = parseDisposition(await optionalJson(`${reportsDir}/github-issue-disposition.json`));
  const outcome = parseOutcome(await optionalJson(`${reportsDir}/github-issue-production-outcome.json`));
  if (outcome && outcome.candidateSha !== pullRecord.head_sha) {
    throw new Error("Sentinel production outcome does not match the issue pull-request head");
  }

  let pullMerged = false;
  let mergeSource: PullRequestMergeSource = null;
  if (!input.workflowFailed && disposition === "resolved" && outcome?.outcome === "kept") {
    try {
      const merge = await mergeDeliveryPullRequest(input.token, input.repository, pullRecord);
      pullMerged = true;
      mergeSource = merge.source;
    } catch (error) {
      // A kept delivery whose pull request cannot be merged stays open with
      // evidence; the snapshot-based recheck below leaves the issue open too.
      console.warn(`[sentinel] issue_pull_request_merge=#${pullRecord.pull_request_number} failed`, error);
    }
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
    mergeSource,
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
