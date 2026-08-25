import {
  type GitHubIssuePullRequestRecord,
  isIssueDeliveryFailSafeRevert,
  issuePullRequestMarker,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueSelectionReport,
  parseGitPushUpdates,
  parseSentinelCycleReport,
  renderIssuePullRequestBody,
  selectDevelopmentPush,
} from "./issue-delivery.ts";

const API_VERSION = "2022-11-28";
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;

type PullRequest = Readonly<{
  number: number;
  htmlUrl: string;
  state: "open" | "closed";
  mergedAt: string | null;
  body: string;
  headRef: string;
  headSha: string;
  baseRef: string;
}>;

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required for Sentinel issue PR delivery`);
  return value;
};

const readJson = async (path: string): Promise<unknown> => JSON.parse(await Deno.readTextFile(path));

const git = async (args: readonly string[]): Promise<string> => {
  const executable = Deno.env.get("SENTINEL_REAL_GIT")?.trim() || "git";
  const result = await new Deno.Command(executable, {
    args: [...args],
    stdout: "piped",
    stderr: "piped",
  }).output();
  if (!result.success) {
    const stderr = new TextDecoder().decode(result.stderr).trim();
    throw new Error(
      `Git command failed: ${executable} ${args.join(" ")}${stderr ? `: ${stderr.slice(0, 500)}` : ""}`,
    );
  }
  return new TextDecoder().decode(result.stdout).trim();
};

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
  const pull = typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
  const head = pull?.head && typeof pull.head === "object" && !Array.isArray(pull.head)
    ? pull.head as Record<string, unknown>
    : null;
  const base = pull?.base && typeof pull.base === "object" && !Array.isArray(pull.base)
    ? pull.base as Record<string, unknown>
    : null;
  if (
    !pull || !Number.isSafeInteger(pull.number) || (pull.number as number) <= 0 ||
    typeof pull.html_url !== "string" || !/^https:\/\/github\.com\//u.test(pull.html_url) ||
    (pull.state !== "open" && pull.state !== "closed") ||
    (pull.merged_at !== null && typeof pull.merged_at !== "string") ||
    (pull.body !== null && typeof pull.body !== "string") || !head || !base ||
    typeof head.ref !== "string" || !SAFE_BRANCH.test(head.ref) ||
    typeof head.sha !== "string" || !FULL_SHA.test(head.sha) ||
    typeof base.ref !== "string" || !SAFE_BRANCH.test(base.ref)
  ) {
    throw new Error("GitHub returned an invalid pull request");
  }
  return {
    number: pull.number as number,
    htmlUrl: pull.html_url,
    state: pull.state,
    mergedAt: pull.merged_at as string | null,
    body: (pull.body as string | null) ?? "",
    headRef: head.ref,
    headSha: head.sha,
    baseRef: base.ref,
  };
};

const listPullRequests = async (token: string, repository: string): Promise<PullRequest[]> => {
  const pulls: PullRequest[] = [];
  for (let page = 1; page <= 10; page++) {
    const value = await githubRequest(
      token,
      repository,
      `/pulls?state=all&per_page=100&page=${page}&sort=created&direction=desc`,
    );
    if (!Array.isArray(value)) throw new Error("GitHub pull-request listing is invalid");
    pulls.push(...value.map(parsePullRequest));
    if (value.length < 100) return pulls;
  }
  throw new Error("Sentinel pull-request lookup exceeded its pagination limit");
};

const ensureRemoteCandidateBranch = async (branch: string, candidateSha: string): Promise<void> => {
  const remote = await git(["ls-remote", "--heads", "origin", `refs/heads/${branch}`]);
  const remoteSha = remote.length === 0 ? null : remote.split(/\s+/u)[0] ?? null;
  if (remoteSha === candidateSha) return;
  if (remoteSha !== null && !FULL_SHA.test(remoteSha)) {
    throw new Error("Remote Sentinel candidate branch has an invalid SHA");
  }
  await git(["push", "--no-verify", "origin", `HEAD:refs/heads/${branch}`]);
  const confirmed = await git(["ls-remote", "--heads", "origin", `refs/heads/${branch}`]);
  if ((confirmed.split(/\s+/u)[0] ?? null) !== candidateSha) {
    throw new Error("Sentinel candidate branch did not reach the expected remote SHA");
  }
};

const reportNames = async (reportsDir: string, pattern: RegExp): Promise<string[]> => {
  const names: string[] = [];
  for await (const entry of Deno.readDir(reportsDir)) {
    if (entry.isFile && pattern.test(entry.name)) names.push(entry.name);
  }
  return names.sort();
};

const optionalPreviewEvidence = async (
  reportsDir: string,
): Promise<Readonly<{ revision: string | null; workflowRunId: number | null }>> => {
  const names = await reportNames(reportsDir, /^preview-deployment-round-[1-3]\.json$/u);
  const latest = names.at(-1);
  if (!latest) return { revision: null, workflowRunId: null };
  const value = await readJson(`${reportsDir}/${latest}`);
  const report = typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
  return {
    revision: typeof report?.revision === "string" && /^[A-Za-z0-9_-]{1,200}$/u.test(report.revision)
      ? report.revision
      : null,
    workflowRunId: Number.isSafeInteger(report?.workflow_run_id) && (report!.workflow_run_id as number) > 0
      ? report!.workflow_run_id as number
      : null,
  };
};

export const matchingIssueDeliveryPullRequests = (
  pulls: readonly PullRequest[],
  marker: string,
  candidateSha: string,
  candidateBranch: string,
): PullRequest[] =>
  pulls.filter((pull) =>
    pull.body.includes(marker) && pull.headSha === candidateSha && pull.headRef === candidateBranch &&
    pull.baseRef === "development"
  );

export const ensureIssuePullRequestForDevelopmentPush = async (input: Readonly<{
  repositoryRoot: string;
  prePushInput: string;
  token: string;
  repository: string;
  workflowRunId: string;
  serverUrl: string;
}>): Promise<GitHubIssuePullRequestRecord | null> => {
  const update = selectDevelopmentPush(parseGitPushUpdates(input.prePushInput));
  if (!update) return null;
  const reportsDir = `${input.repositoryRoot}/.sentinel/reports`;
  let selectionValue: unknown;
  try {
    selectionValue = await readJson(`${reportsDir}/github-issue-selection.json`);
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return null;
    throw error;
  }
  const selection = parseGitHubIssueSelectionReport(selectionValue);
  const cycle = parseSentinelCycleReport(await readJson(`${reportsDir}/cycle.json`));
  if (!cycle.temporary_branch || !cycle.temporary_branch.startsWith("sentinel/candidate-")) {
    throw new Error("Sentinel issue delivery has no valid candidate branch");
  }
  if (cycle.candidate_sha !== update.localSha) {
    const pullValue = await readJson(`${reportsDir}/github-issue-pull-request.json`);
    const pullRecord = parseGitHubIssuePullRequestRecord(pullValue);
    const parentSha = await git(["rev-parse", `${update.localSha}^`]);
    if (
      !isIssueDeliveryFailSafeRevert({
        selection,
        cycle,
        pullRequest: pullRecord,
        pushedSha: update.localSha,
        parentSha,
      })
    ) {
      throw new Error("Sentinel development push does not match the issue candidate or its fail-safe revert");
    }
    return pullRecord;
  }
  await ensureRemoteCandidateBranch(cycle.temporary_branch, update.localSha);

  const marker = issuePullRequestMarker(selection);
  const existing = matchingIssueDeliveryPullRequests(
    await listPullRequests(input.token, input.repository),
    marker,
    update.localSha,
    cycle.temporary_branch,
  );
  if (existing.length > 1) {
    throw new Error("More than one pull request exists for the concrete Sentinel issue delivery attempt");
  }

  const preview = await optionalPreviewEvidence(reportsDir);
  const workflowRunUrl = `${input.serverUrl}/${input.repository}/actions/runs/${input.workflowRunId}`;
  const body = renderIssuePullRequestBody({
    repository: input.repository,
    selection,
    cycle,
    candidateSha: update.localSha,
    workflowRunUrl,
    validationReports: await reportNames(reportsDir, /^validation-(?:round-[1-3]|manual-github-issue)\.json$/u),
    nativeReviewReports: await reportNames(reportsDir, /^native-review-round-[1-3]\.json$/u),
    replayReports: await reportNames(reportsDir, /^replay-round-[1-3]\.json$/u),
    previewRevision: preview.revision,
    previewWorkflowRunId: preview.workflowRunId,
  });

  let pull: PullRequest;
  let reused = false;
  if (existing.length === 1) {
    pull = existing[0]!;
    reused = true;
    if (pull.state !== "open" || pull.mergedAt !== null) {
      throw new Error("Existing Sentinel delivery-attempt pull request is no longer open");
    }
    pull = parsePullRequest(
      await githubRequest(input.token, input.repository, `/pulls/${pull.number}`, {
        method: "PATCH",
        body: JSON.stringify({ body }),
      }),
    );
  } else {
    pull = parsePullRequest(
      await githubRequest(input.token, input.repository, "/pulls", {
        method: "POST",
        body: JSON.stringify({
          title: `fix(issue #${selection.issue_number}): Provider Sentinel deliverable`,
          body,
          head: cycle.temporary_branch,
          base: "development",
          draft: false,
          maintainer_can_modify: false,
        }),
      }),
    );
  }
  if (
    pull.state !== "open" || pull.mergedAt !== null || pull.headRef !== cycle.temporary_branch ||
    pull.headSha !== update.localSha || pull.baseRef !== "development" || !pull.body.includes(marker)
  ) {
    throw new Error("Sentinel issue pull request failed its post-creation identity check");
  }

  const record: GitHubIssuePullRequestRecord = {
    schema_version: 1,
    issue_number: selection.issue_number,
    fingerprint: selection.fingerprint,
    pull_request_number: pull.number,
    pull_request_url: pull.htmlUrl,
    head_branch: pull.headRef,
    head_sha: pull.headSha,
    base_branch: "development",
    marker,
    reused,
  };
  await Deno.writeTextFile(
    `${reportsDir}/github-issue-pull-request.json`,
    `${JSON.stringify(record, null, 2)}\n`,
    { mode: 0o600 },
  );
  console.log(`[sentinel] issue_pull_request=#${pull.number} issue=#${selection.issue_number} reused=${reused}`);
  return record;
};

if (import.meta.main) {
  const repositoryRoot = await Deno.realPath(requiredEnvironment("GITHUB_WORKSPACE"));
  const prePushInput = await new Response(Deno.stdin.readable).text();
  await ensureIssuePullRequestForDevelopmentPush({
    repositoryRoot,
    prePushInput,
    token: requiredEnvironment("GITHUB_TOKEN"),
    repository: requiredEnvironment("GITHUB_REPOSITORY"),
    workflowRunId: requiredEnvironment("GITHUB_RUN_ID"),
    serverUrl: Deno.env.get("GITHUB_SERVER_URL")?.trim() || "https://github.com",
  });
}
