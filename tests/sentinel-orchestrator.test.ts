import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import issueJobLedger from "../docs/sentinel-issue-jobs.md" with { type: "text" };
import {
  isAutonomousMode,
  isSentinelProtectedImplementationPath,
  SENTINEL_POLICY,
} from "../scripts/sentinel/policy.ts";
import {
  agentCheckoutPath,
  assertRetainedReplayArtifactBudget,
  assertTriageMatchesMatrixPlan,
  bindMatrixConvergenceWork,
  candidateRevertDiffArguments,
  candidateShaForReview,
  createSentinelCandidateRecoveryRecord,
  deduplicateRetainedReplayCaptures,
  durableProductionDecision,
  evaluateReviewBacklogImplementation,
  evaluateRollbackPreflight,
  evaluateSelectedIssueImplementation,
  evaluateSentinelTriageGate,
  failedCycleBranchDisposition,
  finalizeSentinelCandidate,
  GITHUB_ISSUE_IMPLEMENTATION_CONTINUATION_MS,
  IMPLEMENTATION_CONTINUATION_MS,
  IMPLEMENTATION_INITIAL_MS,
  implementationFailureDisposition,
  implementationPrompt,
  isObserveOnlyMode,
  isStablePlanningBlocker,
  loadPreparedConvergenceRecoveryRecord,
  MAX_MATCHING_REPLAY_ARCHIVE_BYTES,
  MAX_MATCHING_REPLAY_ARTIFACTS,
  MONITOR_AGENT_MS,
  parseIncidentStartMs,
  parseMode,
  parseMonitorDecision,
  parseSentinelDeploymentAttestation,
  persistPlanningOutcome,
  prepareImplementationFailureRetry,
  previewCompletionForDecision,
  replayIndexArtifactMayMatch,
  replayIndexArtifactName,
  requireIssueLedgerOnlyChangedPaths,
  requireResolvedReviewBacklogImplementation,
  requireResolvedSelectedIssueImplementation,
  requiresReplayEvaluation,
  resolveCycleAnchorMs,
  RetryCheckpointResumeError,
  retryCheckpointResumeFailureDisposition,
  reuseMatrixPreparedRecoveryRecord,
  reviewBacklogEntriesMatch,
  runImplementationStageWithContinuation,
  runObserveCycle,
  runWithSingleTimeoutContinuation,
  selectSentinelWork,
  sentinelDeploymentInputs,
  sentinelEvidenceArtifactName,
  sentinelRevisionControlInputs,
  type SentinelWorkSelection,
  shouldDeferHourlyBacklogWork,
  terminalTemporaryCandidateBranches,
  TRIAGE_INCIDENT_MS,
  triageExpectedMaximumRuntimeMs,
  triagePrompt,
  validationRepairPrompt,
  verifyMatrixConvergenceAdvance,
  withStageHeartbeat,
  zeroUnselectedReplayBodies,
} from "../scripts/sentinel/main.ts";
import {
  buildMatrixPlan,
  canonicalMatrixJson,
  matrixCellReportDigest,
  matrixCycleReportDigest,
  type MatrixCycleReportV1,
  type MatrixPlanV1,
  parseMatrixPlanV1,
} from "../scripts/sentinel/matrix.ts";
import { encryptSentinelArtifact } from "../scripts/sentinel/artifact-crypto.ts";
import { validateMatrixIssueDeliveryMerge } from "../scripts/sentinel/issue-delivery-reconcile.ts";
import { CodexInvocationError } from "../scripts/sentinel/codex.ts";
import type {
  GitHubIssue,
  GitHubIssueComment,
  GitHubIssueRelations,
  GitHubRepositoryPermission,
} from "../scripts/sentinel/github.ts";
import {
  applyGitHubIssueJobDisposition,
  blockingIssueReviewFindings,
  createGitHubIssueJob,
  evaluateGitHubIssueJobImplementation,
  getCurrentGitHubIssueJob,
  GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS,
  type GitHubIssueJob,
  githubIssueJobMatchesHint,
  githubIssueJobsMatch,
  type GitHubIssueJobSource,
  githubIssueJobTriageReport,
  issueJobFindingId,
  issueReviewBacklogFindings,
  parseGitHubIssueJobHint,
  parseGitHubIssueJobLedger,
  renderGitHubIssueJobHint,
  renderGitHubIssueJobLedger,
  requireResolvedGitHubIssueJobImplementation,
  selectNextGitHubIssueJob,
  selectNextGitHubIssueJobSelection,
  SentinelChangedFilesMismatchError,
} from "../scripts/sentinel/issues.ts";
import type { GitHubIssueSelectionReport } from "../scripts/sentinel/issue-delivery.ts";
import { parseSentinelRecoveryRecord, type SentinelRecoveryRecordV1 } from "../scripts/sentinel/recovery.ts";
import {
  emptySentinelRecoveryLedger,
  parseSentinelRecoveryLedger,
  type SentinelRecoveryEligibilityContext,
  sentinelRecoveryIdentityKey,
  type SentinelRecoveryLedgerV1,
} from "../scripts/sentinel/recovery-ledger.ts";
import type { SentinelRecoveryLedgerSnapshot } from "../scripts/sentinel/recovery-github-store.ts";
import {
  inspectSse,
  isInferenceOnlyReplayEndpoint,
  replayOneCase,
  selectCurrentAndMatchingRegressionCases,
} from "../scripts/sentinel/replay.ts";
import {
  applyReviewBacklogImplementationDisposition,
  blockingReviewFindings,
  canStartReviewRound,
  findReviewBacklogEntry,
  mergeReviewBacklog,
  nativeReviewParseInput,
  parseNativeReview,
  parseReviewBacklog,
  parseStructuredNativeReview,
  renderReviewBacklog,
  type ReviewBacklogEntry,
  reviewBacklogTriageReport,
  selectNextReviewBacklogEntry,
} from "../scripts/sentinel/review.ts";
import {
  assertActionableFindingsResolved,
  assertCompleteFindingDispositions,
  IMPLEMENTATION_OUTPUT_SCHEMA,
  type ImplementationReport,
  isTriageReport,
  MONITOR_OUTPUT_SCHEMA,
  type ReplayCase,
  type ReplayResult,
  TRIAGE_OUTPUT_SCHEMA,
  type TriageReport,
} from "../scripts/sentinel/types.ts";
import {
  applySentinelRetryPolicyToRecovery,
  classifySentinelFailure,
  computeSentinelRetryBackoffMs,
  createFreshSentinelRecoveryRecord,
  createSentinelRetryAttempt,
  evaluateSentinelRetryPolicy,
  SENTINEL_RETRY_CIRCUIT_THRESHOLD,
  SENTINEL_RETRY_MAX_DELAY_MS,
  stableSentinelFailureFingerprint,
} from "../scripts/sentinel/retry.ts";
import {
  computeSentinelInterval,
  deduplicateEvents,
  eventDedupeKey,
  HOURLY_OVERLAP_MS,
  HOURLY_WINDOW_MS,
  INCIDENT_WINDOW_MS,
  OBSERVE_WINDOW_MS,
} from "../scripts/sentinel/windows.ts";
import {
  assertProtectedFilesUnchanged,
  CANDIDATE_DENO_CHECK_ARGS,
  type CandidateValidationFailure,
  hashProtectedFiles,
} from "../scripts/sentinel/validation.ts";
import type { ExportedSentinelReplayCapture } from "../src/sentinel_replay_capture.ts";

const now = Date.parse("2026-08-21T06:00:00.000Z");

const recoverySelectionContext = (
  ledger: SentinelRecoveryLedgerV1 = emptySentinelRecoveryLedger(),
  continuationRecord: SentinelRecoveryRecordV1 | null = null,
  now = "2026-08-28T06:00:00.000Z",
): SentinelRecoveryEligibilityContext => ({
  repository: "ubiquity/ai.ubq.fi",
  ledger,
  now,
  continuation_record: continuationRecord,
});

const matrixVerifierGitEnvironment = {
  GIT_CONFIG_GLOBAL: "/dev/null",
  GIT_CONFIG_NOSYSTEM: "1",
  GIT_CONFIG_SYSTEM: "/dev/null",
  GIT_TERMINAL_PROMPT: "0",
};

const matrixVerifierPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run", command: "git" }),
]);
const matrixVerifierTestsIgnored = matrixVerifierPermissions.some((permission) => permission.state !== "granted");

const matrixVerifierGit = async (cwd: string, args: readonly string[]): Promise<string> => {
  const output = await new Deno.Command("git", {
    args: [...args],
    cwd,
    env: matrixVerifierGitEnvironment,
    stdout: "piped",
    stderr: "piped",
  }).output();
  const decoder = new TextDecoder();
  if (!output.success) {
    throw new Error(`git ${args.join(" ")} failed: ${decoder.decode(output.stderr).trim()}`);
  }
  return decoder.decode(output.stdout).trim();
};

const createMatrixVerifierRepository = async (): Promise<Readonly<{ repo: string; baseSha: string }>> => {
  const repo = await Deno.makeTempDir({ prefix: "sentinel-matrix-convergence-" });
  await matrixVerifierGit(repo, ["init", "-b", "development"]);
  await matrixVerifierGit(repo, ["config", "user.name", "Sentinel Matrix Fixture"]);
  await matrixVerifierGit(repo, ["config", "user.email", "sentinel-matrix-fixture@example.invalid"]);
  await Deno.writeTextFile(`${repo}/README.md`, "matrix verifier base\n");
  await matrixVerifierGit(repo, ["add", "--", "README.md"]);
  await matrixVerifierGit(repo, ["commit", "--no-gpg-sign", "-m", "matrix verifier base"]);
  return { repo, baseSha: await matrixVerifierGit(repo, ["rev-parse", "HEAD"]) };
};

const commitMatrixVerifierFixture = async (
  repo: string,
  message: string,
  allowEmpty = false,
): Promise<string> => {
  await matrixVerifierGit(repo, ["add", "--all"]);
  await matrixVerifierGit(repo, ["commit", "--no-gpg-sign", ...(allowEmpty ? ["--allow-empty"] : []), "-m", message]);
  return await matrixVerifierGit(repo, ["rev-parse", "HEAD"]);
};

const rollingReviewResultFixture = (
  prNumber: number,
  headSha: string,
  overrides: Readonly<Record<string, unknown>> = {},
): Record<string, unknown> => ({
  schema_version: 1,
  request_id: `${prNumber}-${headSha}`,
  pr_number: prNumber,
  pr_url: `https://github.com/ubiquity/ai.ubq.fi/pull/${prNumber}`,
  head_sha: headSha,
  base_sha: "b".repeat(40),
  head_branch: `sentinel/candidate-${prNumber}`,
  status: "completed",
  reviewed_at: "2026-08-31T00:00:00.000Z",
  parse_status: "no_findings",
  raw_review_text: "",
  review_stderr: "",
  structured_review: null,
  findings: [],
  failure: null,
  ...overrides,
});

const matrixVerifierResultPath = (prNumber: number, headSha: string): string =>
  `docs/sentinel-review-results/${prNumber}-${headSha}.json`;

const writeMatrixVerifierResult = async (
  repo: string,
  prNumber: number,
  headSha: string,
  value: unknown,
  fileName = matrixVerifierResultPath(prNumber, headSha),
): Promise<void> => {
  await Deno.mkdir(`${repo}/docs/sentinel-review-results`, { recursive: true });
  await Deno.writeTextFile(`${repo}/${fileName}`, typeof value === "string" ? value : JSON.stringify(value));
};

type MatrixVerifierAdvance = Readonly<{
  plannedBaseSha: string;
  currentDevelopmentSha: string;
}>;

type MatrixVerifierNegativeCase = Readonly<{
  name: string;
  prepare: (fixture: Readonly<{ repo: string; baseSha: string }>) => Promise<MatrixVerifierAdvance>;
  error: RegExp;
}>;

Deno.test("triage reads the repository root while monitoring reads the accepted candidate checkout", () => {
  const root = "/runner/work/repository";
  const candidate = "/runner/work/repository/.sentinel/candidate-worktree";
  assert.equal(agentCheckoutPath("triage", root, candidate), root);
  assert.equal(agentCheckoutPath("implementation", root, candidate), candidate);
  assert.equal(agentCheckoutPath("monitoring", root, candidate), candidate);
});

Deno.test("incident triage is bounded and unauthenticated model probes are classified by ownership", () => {
  assert.equal(TRIAGE_INCIDENT_MS, 6 * 60_000);
  assert.equal(triageExpectedMaximumRuntimeMs("incident"), TRIAGE_INCIDENT_MS);
  assert.equal(triageExpectedMaximumRuntimeMs("preview"), TRIAGE_INCIDENT_MS);
  assert.equal(triageExpectedMaximumRuntimeMs("hourly"), undefined);
  assert.equal(triageExpectedMaximumRuntimeMs("observe"), undefined);

  const prompt = triagePrompt(
    computeSentinelInterval("incident", now),
    { path: "/tmp/raw-logs.jsonl", byte_count: 123, sha256: "a".repeat(64) },
    [],
  );
  assert.match(prompt, /Expected client rejections are not gateway defects/);
  assert.match(prompt, /Report a repeated evidence-backed external caller misconfiguration as actionable false/);
  assert.match(prompt, /GET \/v1\/models response with 401 invalid_api_key is expected gateway behavior/);
  assert.match(prompt, /not repository-actionable without evidence of a repository-owned caller/);
  assert.match(prompt, /public model catalog is GET \/uos\/models\/catalog/);
});

Deno.test("automatic Codex triage runs only for durable incidents", () => {
  assert.deepEqual(evaluateSentinelTriageGate("hourly", 0), {
    required: false,
    reason: "hourly_archive_only",
  });
  assert.deepEqual(evaluateSentinelTriageGate("hourly", 3), {
    required: false,
    reason: "hourly_archive_only",
  });
  assert.deepEqual(evaluateSentinelTriageGate("incident", 0), {
    required: true,
    reason: "incident_signal",
  });
  assert.deepEqual(evaluateSentinelTriageGate("preview", 0), {
    required: false,
    reason: "preview_no_failure_capture",
  });
  assert.deepEqual(evaluateSentinelTriageGate("preview", 1), {
    required: true,
    reason: "preview_failure_capture",
  });
  assert.deepEqual(evaluateSentinelTriageGate("observe", 0), {
    required: true,
    reason: "explicit_observation",
  });
  assert.throws(() => evaluateSentinelTriageGate("hourly", -1), /capture count/);
});

Deno.test("preview completion restores only a candidate accepted by monitoring", () => {
  assert.deepEqual(previewCompletionForDecision("keep"), {
    restoreCandidate: true,
    status: "preview_complete",
    branchDisposition: "retained_pending_supervised_acceptance",
  });
  assert.deepEqual(previewCompletionForDecision("rollback"), {
    restoreCandidate: false,
    status: "preview_rolled_back",
    branchDisposition: "remote_retained_rejected_by_monitor",
  });
});

Deno.test("post-validation atomic retry failures remain reconcilable", () => {
  assert.equal(
    failedCycleBranchDisposition({
      stage: "validated_retry_pending_atomic_push",
      branch_disposition: "remote_retained_issue_retry_pending",
      temporary_branch: "sentinel/candidate-123456789",
    }),
    "atomic_retry_push_requires_reconciliation",
  );
  assert.equal(
    failedCycleBranchDisposition({
      stage: "pushing_retry_pending_github_issue",
      branch_disposition: "remote_retained_issue_retry_pending",
      temporary_branch: "sentinel/candidate-123456789",
    }),
    "remote_retained_after_failed_cycle",
  );
  assert.equal(
    failedCycleBranchDisposition({
      stage: "verifying_retry_pending_atomic_push",
      branch_disposition: "atomic_retry_push_accepted_unverified",
      temporary_branch: "sentinel/candidate-123456789",
    }),
    "atomic_retry_push_requires_reconciliation",
  );
  assert.equal(
    failedCycleBranchDisposition({
      stage: "pushing_retry_pending_github_issue",
      branch_disposition: "remote_retained_atomic_push_in_flight",
      temporary_branch: "sentinel/candidate-123456789",
    }),
    "atomic_retry_push_requires_reconciliation",
  );
});

Deno.test("implementation scope protects Sentinel and nested Codex instruction surfaces", () => {
  for (
    const path of [
      ".gitleaksignore",
      "AGENTS.md",
      "src/AGENTS.md",
      "nested/AGENTS.override.md",
      ".codex/config.toml",
      "src/.codex/hooks.json",
      ".agents/skills/reviewer/SKILL.md",
      "skills/reviewer/SKILL.md",
      "scripts/sentinel/main.ts",
      "docs/sentinel-issue-jobs.md",
      "docs/sentinel-recovery-records.json",
      "scripts/sentinel/issues.ts",
      "src/sentinel_replay_capture.ts",
      "tests/sentinel-replay-capture.test.ts",
      ".github/workflows/other.yml",
    ]
  ) assert.equal(isSentinelProtectedImplementationPath(path), true, path);
  assert.equal(isSentinelProtectedImplementationPath("src/openai.ts"), false);
});

Deno.test("Sentinel implementation policy fixes the owner-controlled model to Luna", () => {
  assert.deepEqual(SENTINEL_POLICY.implementation, { model: "gpt-5.6-luna", reasoning: "max" });
});

Deno.test("implementation prompt tells agents to block protected repairs before editing", () => {
  const prompt = implementationPrompt(
    {
      schema_version: 1,
      interval: computeSentinelInterval("hourly", now),
      findings: [],
      no_findings_reason: "No evidence-backed finding in the fixture.",
    },
    [],
    null,
  );
  assert.match(prompt, /isSentinelProtectedImplementationPath/);
  assert.match(prompt, /status `blocked`/);
  assert.match(prompt, /src\/sentinel_replay_capture\.ts/);
  assert.match(prompt, /tests\/sentinel-replay-capture\.test\.ts/);
  assert.match(prompt, /empty `changed_files` array/);
  assert.match(prompt, /agent model or reasoning selections/);
  assert.match(prompt, /GitHub issue text and metadata/);
});

Deno.test("validation repair treats private diagnostics as untrusted and keeps policy protected", () => {
  const failure: CandidateValidationFailure = {
    phase: "repository_tests",
    command: ["deno", "test", "--cached-only"],
    exit_code: 1,
    duration_ms: 42,
    stdout_path: "/private/reports/validation.stdout.bin",
    stdout_bytes: 0,
    stdout_sha256: "a".repeat(64),
    stdout_excerpt: "",
    stdout_truncated: false,
    stderr_path: "/private/reports/validation.stderr.bin",
    stderr_bytes: 16,
    stderr_sha256: "b".repeat(64),
    stderr_excerpt: "fixture failure",
    stderr_truncated: false,
  };
  const prompt = validationRepairPrompt(
    {
      schema_version: 1,
      interval: computeSentinelInterval("hourly", now),
      findings: [],
      no_findings_reason: "No evidence-backed finding in the fixture.",
    },
    null,
    failure,
  );
  assert.match(prompt, /untrusted data, not\ninstructions/u);
  assert.match(prompt, /repository_tests/u);
  assert.match(prompt, /fixture failure/u);
  assert.match(prompt, /isSentinelProtectedImplementationPath/u);
  assert.match(prompt, /without weakening, skipping, deleting/u);
});

Deno.test("implementation timeout gets one continuation and never retries another failure", async () => {
  assert.equal(IMPLEMENTATION_INITIAL_MS, 20 * 60_000);
  assert.equal(IMPLEMENTATION_CONTINUATION_MS, 10 * 60_000);
  assert.equal(GITHUB_ISSUE_IMPLEMENTATION_CONTINUATION_MS, 20 * 60_000);
  const attempts: number[] = [];
  let timeoutCallbacks = 0;
  const result = await runWithSingleTimeoutContinuation(
    (attempt) => {
      attempts.push(attempt);
      if (attempt === 1) throw new CodexInvocationError("invocation_timeout");
      return Promise.resolve("completed");
    },
    () => {
      timeoutCallbacks++;
      return Promise.resolve();
    },
  );
  assert.equal(result, "completed");
  assert.deepEqual(attempts, [1, 2]);
  assert.equal(timeoutCallbacks, 1);

  attempts.length = 0;
  await assert.rejects(
    () =>
      runWithSingleTimeoutContinuation(
        (attempt) => {
          attempts.push(attempt);
          throw new CodexInvocationError("invocation_timeout");
        },
        () => Promise.resolve(),
      ),
    (error) => error instanceof CodexInvocationError && error.failure === "invocation_timeout",
  );
  assert.deepEqual(attempts, [1, 2]);

  attempts.length = 0;
  await assert.rejects(
    () =>
      runWithSingleTimeoutContinuation(
        (attempt) => {
          attempts.push(attempt);
          throw new CodexInvocationError("command_failed");
        },
        () => Promise.resolve(),
      ),
    (error) => error instanceof CodexInvocationError && error.failure === "command_failed",
  );
  assert.deepEqual(attempts, [1]);

  const timeoutBudgets: number[] = [];
  await runImplementationStageWithContinuation({
    basePrompt: "Implement the selected GitHub issue.",
    initialTimeoutMs: IMPLEMENTATION_INITIAL_MS,
    continuationTimeoutMs: GITHUB_ISSUE_IMPLEMENTATION_CONTINUATION_MS,
    invoke: ({ attempt, timeoutMs }) => {
      timeoutBudgets.push(timeoutMs);
      if (attempt === 1) throw new CodexInvocationError("invocation_timeout");
      return Promise.resolve("completed");
    },
    onTimeout: () => Promise.resolve(),
  });
  assert.deepEqual(timeoutBudgets, [20 * 60_000, 20 * 60_000]);
});

Deno.test("stage heartbeat emits safe progress and always cancels its timer", async () => {
  let scheduled: (() => void) | null = null;
  const cleared: number[] = [];
  const messages: string[] = [];
  let nowMs = 1_000;
  const result = await withStageHeartbeat(
    "implementing",
    () => {
      nowMs = 62_000;
      scheduled!();
      return Promise.resolve("done");
    },
    {
      intervalMs: 60_000,
      now: () => nowMs,
      log: (message) => messages.push(message),
      setInterval: (callback, intervalMs) => {
        assert.equal(intervalMs, 60_000);
        scheduled = callback;
        return 17;
      },
      clearInterval: (timer) => {
        assert.equal(typeof timer, "number");
        cleared.push(timer as number);
      },
    },
  );
  assert.equal(result, "done");
  assert.deepEqual(messages, ["[sentinel] stage=implementing status=running elapsed_seconds=61"]);
  assert.deepEqual(cleared, [17]);

  await assert.rejects(() =>
    withStageHeartbeat("triage", () => Promise.reject(new Error("failed")), {
      setInterval: () => 23,
      clearInterval: (timer) => {
        assert.equal(typeof timer, "number");
        cleared.push(timer as number);
      },
    })
  );
  assert.deepEqual(cleared, [17, 23]);
});

Deno.test("sentinel capacity failures use bounded source-specific dispositions", () => {
  const capacityFailures = ["accounts_unavailable", "invocation_timeout", "command_failed", "runtime_failure"];
  const integrityFailure = new CodexInvocationError("secret_in_output", { exitCode: 42 });
  for (const failure of capacityFailures) {
    const capacityError = new CodexInvocationError(failure as never, { exitCode: 1 });
    assert.equal(implementationFailureDisposition("github_issue", capacityError), "retry_pending");
    assert.equal(implementationFailureDisposition("review_backlog", capacityError), "manual_required");
  }
  assert.equal(implementationFailureDisposition("github_issue", integrityFailure), "crash");
  assert.equal(implementationFailureDisposition("review_backlog", integrityFailure), "crash");
  assert.equal(implementationFailureDisposition("triage", new CodexInvocationError("accounts_unavailable")), "crash");
  assert.equal(implementationFailureDisposition("triage", new Error("plain failure")), "crash");
  assert.equal(implementationFailureDisposition("github_issue", new Error("plain failure")), "crash");
  const capacityError = new CodexInvocationError("accounts_unavailable");
  assert.equal(implementationFailureDisposition("github_issue", capacityError), "retry_pending");
  assert.equal(
    retryCheckpointResumeFailureDisposition(
      new RetryCheckpointResumeError("retry_pending", "temporary Git transport failure"),
    ),
    "retry_pending",
  );
  assert.equal(
    retryCheckpointResumeFailureDisposition(
      new RetryCheckpointResumeError("manual_required", "checkpoint identity mismatch"),
    ),
    "manual_required",
  );
  assert.equal(retryCheckpointResumeFailureDisposition(new Error("unexpected failure")), "crash");
});

Deno.test("non-runtime issue commits reject post-validation file noise", () => {
  assert.doesNotThrow(() => requireIssueLedgerOnlyChangedPaths([SENTINEL_POLICY.paths.issueJobLedger]));
  assert.throws(
    () => requireIssueLedgerOnlyChangedPaths([SENTINEL_POLICY.paths.issueJobLedger, "deno.json"]),
    /must change only the trusted issue-job ledger/,
  );
  assert.throws(
    () => requireIssueLedgerOnlyChangedPaths([]),
    /must change only the trusted issue-job ledger/,
  );
});

Deno.test("terminal Sentinel cleanup removes a superseded retry checkpoint", () => {
  const checkpoint = {
    branch: "sentinel/candidate-123456789-1",
    sha: "b".repeat(40),
    baseSha: "a".repeat(40),
  };
  assert.deepEqual(
    terminalTemporaryCandidateBranches("sentinel/candidate-123456790-1", checkpoint),
    [checkpoint.branch, "sentinel/candidate-123456790-1"],
  );
  assert.deepEqual(
    terminalTemporaryCandidateBranches(checkpoint.branch, checkpoint),
    [checkpoint.branch],
  );
  assert.deepEqual(
    terminalTemporaryCandidateBranches("sentinel/candidate-123456790-1", null),
    ["sentinel/candidate-123456790-1"],
  );
});

Deno.test("retryable issue failure preserves, discards, cools down, and advances the queue", async () => {
  assert.equal(GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS, 6 * 60 * 60_000);
  const firstIssue = sentinelGitHubIssue();
  const secondIssue = sentinelGitHubIssue({
    id: 10_114,
    nodeId: "I_kwDOIssue114",
    number: 114,
    title: "Implement the next eligible issue",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/114",
    createdAt: "2026-08-23T19:06:04Z",
    updatedAt: "2026-08-23T19:07:28Z",
  });
  const source = githubIssueSource([firstIssue, secondIssue]);
  const emptyLedger = renderGitHubIssueJobLedger([]);
  const failedAt = new Date("2026-08-23T20:00:00Z");
  const selected = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    emptyLedger,
    recoverySelectionContext(),
    failedAt,
  );
  assert.ok(selected);
  const events: string[] = [];
  const failure = new CodexInvocationError("invocation_timeout");
  assert.equal(
    await prepareImplementationFailureRetry(
      "github_issue",
      failure,
      () => {
        events.push("preserve");
        return Promise.resolve();
      },
      () => {
        events.push("discard");
        return Promise.resolve();
      },
    ),
    "retry_pending",
  );
  assert.deepEqual(events, ["preserve", "discard"]);
  events.length = 0;
  await assert.rejects(
    () =>
      prepareImplementationFailureRetry(
        "github_issue",
        failure,
        () => {
          events.push("preserve");
          return Promise.reject(new Error("snapshot failed"));
        },
        () => {
          events.push("discard");
          return Promise.resolve();
        },
      ),
    /snapshot failed/,
  );
  assert.deepEqual(events, ["preserve"]);
  const checkpoint = {
    branch: "sentinel/candidate-123456789",
    sha: "b".repeat(40),
    baseSha: "a".repeat(40),
  };
  const pendingLedger = applyGitHubIssueJobDisposition(
    emptyLedger,
    selected,
    checkpoint.baseSha,
    failedAt,
    "retry_pending",
    checkpoint,
  );
  assert.deepEqual(parseGitHubIssueJobLedger(pendingLedger)[0]?.checkpoint, checkpoint);
  assert.equal(
    (
      await selectNextGitHubIssueJob(
        source,
        "ubiquity/ai.ubq.fi",
        pendingLedger,
        recoverySelectionContext(),
        new Date(failedAt.getTime() + 60 * 60_000),
      )
    )?.number,
    114,
  );
  const dueAt = new Date(failedAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS);
  const dueSelection = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    pendingLedger,
    recoverySelectionContext(),
    dueAt,
  );
  assert.equal(dueSelection?.job?.fingerprint, selected.fingerprint);
  assert.deepEqual(dueSelection?.checkpoint, checkpoint);
  const checkpointHint = parseGitHubIssueJobHint(renderGitHubIssueJobHint(selected, checkpoint));
  assert.equal(githubIssueJobMatchesHint(checkpointHint, selected, checkpoint), true);
  assert.equal(githubIssueJobMatchesHint(checkpointHint, selected, null), false);
  const issueWithLaterInertComment = sentinelGitHubIssue({
    comments: firstIssue.comments + 1,
    updatedAt: "2026-08-23T21:00:00Z",
  });
  const normalizedSelection = await selectNextGitHubIssueJobSelection(
    githubIssueSource([issueWithLaterInertComment, secondIssue]),
    "ubiquity/ai.ubq.fi",
    pendingLedger,
    recoverySelectionContext(),
    dueAt,
  );
  assert.equal(normalizedSelection?.job?.number, selected.number);
  assert.notEqual(normalizedSelection?.job?.fingerprint, selected.fingerprint);
  assert.deepEqual(normalizedSelection?.checkpoint, checkpoint);
  const normalizedHint = parseGitHubIssueJobHint(
    renderGitHubIssueJobHint(normalizedSelection!.job!, normalizedSelection!.checkpoint),
  );
  assert.equal(githubIssueJobMatchesHint(normalizedHint, normalizedSelection!.job!, checkpoint), true);
  const normalizedRetryAt = new Date(dueAt.getTime() + 1_000);
  const normalizedRetryLedger = applyGitHubIssueJobDisposition(
    pendingLedger,
    normalizedSelection!.job!,
    checkpoint.baseSha,
    normalizedRetryAt,
    "retry_pending",
    checkpoint,
  );
  const normalizedRetryEntries = parseGitHubIssueJobLedger(normalizedRetryLedger);
  assert.equal(normalizedRetryEntries[0]?.disposition, "checkpoint_retained");
  assert.deepEqual(normalizedRetryEntries[0]?.checkpoint, checkpoint);
  assert.deepEqual(
    normalizedRetryEntries.find((entry) => entry.disposition === "retry_pending")?.checkpoint,
    checkpoint,
  );
  const normalizedRetrySelection = await selectNextGitHubIssueJobSelection(
    githubIssueSource([issueWithLaterInertComment, secondIssue]),
    "ubiquity/ai.ubq.fi",
    normalizedRetryLedger,
    recoverySelectionContext(),
    new Date(normalizedRetryAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
  );
  assert.equal(normalizedRetrySelection?.job?.fingerprint, normalizedSelection!.job!.fingerprint);
  assert.deepEqual(normalizedRetrySelection?.checkpoint, checkpoint);
  const issueWithSecondLaterInertComment = sentinelGitHubIssue({
    comments: firstIssue.comments + 2,
    updatedAt: "2026-08-23T22:00:00Z",
  });
  const twiceNormalizedSelection = await selectNextGitHubIssueJobSelection(
    githubIssueSource([issueWithSecondLaterInertComment, secondIssue]),
    "ubiquity/ai.ubq.fi",
    normalizedRetryLedger,
    recoverySelectionContext(),
    new Date(normalizedRetryAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
  );
  assert.equal(twiceNormalizedSelection?.job?.number, selected.number);
  assert.notEqual(twiceNormalizedSelection?.job?.fingerprint, normalizedSelection!.job!.fingerprint);
  assert.deepEqual(twiceNormalizedSelection?.checkpoint, checkpoint);
  const changedIssue = sentinelGitHubIssue({
    title: "Changed issue snapshot with separate retry work",
    updatedAt: "2026-08-24T01:00:00Z",
  });
  const changedJob = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    changedIssue,
    noIssueRelations,
  );
  assert.ok(changedJob);
  const terminalLedger = applyGitHubIssueJobDisposition(
    emptyLedger,
    selected,
    "1".repeat(40),
    failedAt,
    "resolved",
  );
  const changedCheckpoint = {
    branch: "sentinel/candidate-123456791-1",
    sha: "3".repeat(40),
    baseSha: "2".repeat(40),
  };
  const terminalAndRetryLedger = applyGitHubIssueJobDisposition(
    terminalLedger,
    changedJob,
    changedCheckpoint.baseSha,
    new Date("2026-08-24T02:00:00Z"),
    "retry_pending",
    changedCheckpoint,
  );
  const returnedTerminalSnapshot = sentinelGitHubIssue({
    comments: firstIssue.comments + 1,
    updatedAt: "2026-08-24T03:00:00Z",
  });
  const returnedSource = githubIssueSource([returnedTerminalSnapshot, secondIssue]);
  const afterChangedRetryCooldown = new Date("2026-08-24T08:00:00Z");
  assert.equal(
    (
      await selectNextGitHubIssueJobSelection(
        returnedSource,
        "ubiquity/ai.ubq.fi",
        terminalLedger,
        recoverySelectionContext(),
        afterChangedRetryCooldown,
      )
    )?.job?.number,
    secondIssue.number,
  );
  assert.equal(
    (
      await selectNextGitHubIssueJobSelection(
        returnedSource,
        "ubiquity/ai.ubq.fi",
        terminalAndRetryLedger,
        recoverySelectionContext(),
        afterChangedRetryCooldown,
      )
    )?.job?.number,
    secondIssue.number,
  );
  const nextCheckpoint = {
    branch: "sentinel/candidate-123456790-2",
    sha: "d".repeat(40),
    baseSha: "c".repeat(40),
  };
  const retriedLedger = applyGitHubIssueJobDisposition(
    pendingLedger,
    selected,
    nextCheckpoint.baseSha,
    dueAt,
    "retry_pending",
    nextCheckpoint,
  );
  assert.equal(parseGitHubIssueJobLedger(retriedLedger).length, 1);
  assert.deepEqual(parseGitHubIssueJobLedger(retriedLedger)[0]?.checkpoint, nextCheckpoint);
  const resolvedLedger = applyGitHubIssueJobDisposition(
    retriedLedger,
    selected,
    "e".repeat(40),
    new Date(failedAt.getTime() + 2 * GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
    "resolved",
  );
  assert.equal(parseGitHubIssueJobLedger(resolvedLedger)[0]?.checkpoint, null);
  const commentNormalizedJob = {
    ...selected,
    fingerprint: "d".repeat(64),
    comments: selected.comments + 1,
    updatedAt: "2026-08-23T21:00:00Z",
  };
  const commentNormalizedLedger = applyGitHubIssueJobDisposition(
    pendingLedger,
    commentNormalizedJob,
    "d".repeat(40),
    new Date(failedAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
    "resolved",
  );
  const normalizedEntries = parseGitHubIssueJobLedger(commentNormalizedLedger);
  assert.equal(normalizedEntries.length, 2);
  assert.equal(normalizedEntries[0]?.disposition, "checkpoint_retained");
  assert.deepEqual(normalizedEntries[0]?.checkpoint, checkpoint);
  assert.equal(normalizedEntries[1]?.fingerprint, commentNormalizedJob.fingerprint);
  assert.equal(normalizedEntries[1]?.disposition, "resolved");
  const normalizedResolvedLedger = applyGitHubIssueJobDisposition(
    normalizedRetryLedger,
    normalizedSelection!.job!,
    "e".repeat(40),
    new Date(normalizedRetryAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
    "resolved",
  );
  const rolledBackLedger = renderGitHubIssueJobLedger(
    parseGitHubIssueJobLedger(normalizedResolvedLedger).filter((entry) =>
      entry.fingerprint !== normalizedSelection!.job!.fingerprint
    ),
  );
  assert.equal(
    (
      await selectNextGitHubIssueJobSelection(
        githubIssueSource([issueWithLaterInertComment, secondIssue]),
        "ubiquity/ai.ubq.fi",
        rolledBackLedger,
        recoverySelectionContext(),
        new Date(normalizedRetryAt.getTime() + 2 * GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
      )
    )?.job?.fingerprint,
    normalizedSelection!.job!.fingerprint,
  );
  const genuineManualLedger = renderGitHubIssueJobLedger(
    parseGitHubIssueJobLedger(rolledBackLedger).map((entry) => ({ ...entry, disposition: "manual_required" })),
  );
  assert.equal(
    (
      await selectNextGitHubIssueJobSelection(
        githubIssueSource([issueWithLaterInertComment, secondIssue]),
        "ubiquity/ai.ubq.fi",
        genuineManualLedger,
        recoverySelectionContext(),
        new Date(normalizedRetryAt.getTime() + 2 * GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
      )
    )?.job?.number,
    secondIssue.number,
  );
});

Deno.test("native review exhaustion retains one manual checkpoint and advances later GitHub work", async () => {
  const exhaustedIssue = sentinelGitHubIssue({
    id: 10_136,
    nodeId: "I_kwDOIssue136",
    number: 136,
    title: "Automate bounded owner backlog maintenance",
    body: ownerBacklogIssueBody(["src/paid_fallback_ledger.ts"]),
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/136",
    labels: [],
    createdAt: "2026-08-27T19:06:03Z",
    updatedAt: "2026-08-27T19:07:27Z",
  });
  const laterIssue = sentinelGitHubIssue({
    id: 10_137,
    nodeId: "I_kwDOIssue137",
    number: 137,
    title: "Later eligible owner backlog maintenance",
    body: ownerBacklogIssueBody(["src/quota_projection.ts"]),
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/137",
    labels: [],
    createdAt: "2026-08-27T19:06:04Z",
    updatedAt: "2026-08-27T19:07:28Z",
  });
  const source = githubIssueSource([exhaustedIssue, laterIssue]);
  const retryAt = new Date("2026-08-28T00:00:00Z");
  const retryCheckpoint = {
    branch: "sentinel/candidate-33177664067-1",
    sha: "b".repeat(40),
    baseSha: "a".repeat(40),
  };
  const retrySelection = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
    retryAt,
  );
  assert.equal(retrySelection?.job?.number, exhaustedIssue.number);
  const dueRetryLedger = applyGitHubIssueJobDisposition(
    renderGitHubIssueJobLedger([]),
    retrySelection!.job!,
    retryCheckpoint.baseSha,
    retryAt,
    "retry_pending",
    retryCheckpoint,
  );
  const manualAt = new Date(retryAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS);
  const dueRetrySelection = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    dueRetryLedger,
    recoverySelectionContext(),
    manualAt,
  );
  assert.equal(dueRetrySelection?.job?.number, exhaustedIssue.number);
  assert.deepEqual(dueRetrySelection?.checkpoint, retryCheckpoint);

  const manualCheckpoint = {
    branch: "sentinel/candidate-33177664067-2",
    sha: "c".repeat(40),
    baseSha: retryCheckpoint.baseSha,
  };
  const manualLedger = applyGitHubIssueJobDisposition(
    dueRetryLedger,
    dueRetrySelection!.job!,
    manualCheckpoint.baseSha,
    manualAt,
    "manual_required",
    manualCheckpoint,
  );
  const exhaustedEntries = parseGitHubIssueJobLedger(manualLedger).filter((entry) =>
    entry.number === exhaustedIssue.number && entry.fingerprint === (dueRetrySelection!.job)!.fingerprint
  );
  assert.equal(exhaustedEntries.length, 1);
  assert.equal(exhaustedEntries[0]?.disposition, "manual_required");
  assert.deepEqual(exhaustedEntries[0]?.checkpoint, manualCheckpoint);
  assert.equal(exhaustedEntries.filter((entry) => entry.disposition === "retry_pending").length, 0);
  assert.throws(
    () =>
      applyGitHubIssueJobDisposition(
        manualLedger,
        dueRetrySelection!.job!,
        manualCheckpoint.baseSha,
        manualAt,
        "manual_required",
        manualCheckpoint,
      ),
    /already has a terminal/,
  );

  const laterSelection = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    manualLedger,
    recoverySelectionContext(),
    new Date(manualAt.getTime() + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS),
  );
  assert.equal(laterSelection?.job?.number, laterIssue.number);
  assert.equal(laterSelection?.checkpoint, null);
});

const recoveryRecordFixture = (overrides: Readonly<Record<string, unknown>> = {}): SentinelRecoveryRecordV1 =>
  parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: "10113",
      source_revision: "a".repeat(64),
      candidate_generation: 1,
    },
    run_id: "run-1",
    attempt: 1,
    lease_token: "lease-1",
    base_sha: "b".repeat(40),
    phase: "workspace_dirty",
    disposition: "active",
    state_version: 2,
    created_at: "2026-08-28T18:00:00.000Z",
    updated_at: "2026-08-28T18:01:00.000Z",
    candidate_branch: null,
    candidate_sha: null,
    changed_files: ["src/paid_fallback_ledger.ts"],
    tree_sha: null,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: [],
    artifact_digests: [],
    reason: null,
    next_action: "publish checkpoint",
    predecessor: null,
    ...overrides,
  });

const planningIdentity = (
  job: GitHubIssueJob,
  generation = 1,
): {
  repository: string;
  source_kind: "github_issue";
  source_id: string;
  source_revision: string;
  candidate_generation: number;
} => ({
  repository: "ubiquity/ai.ubq.fi",
  source_kind: "github_issue",
  source_id: String(job.issueId),
  source_revision: job.fingerprint,
  candidate_generation: generation,
});

const planningJobFixture = async (): Promise<GitHubIssueJob> => {
  const job = await createGitHubIssueJob("ubiquity/ai.ubq.fi", sentinelGitHubIssue(), noIssueRelations);
  assert.ok(job);
  return job;
};

const planningLedgerSnapshot = (ledger: SentinelRecoveryLedgerV1): SentinelRecoveryLedgerSnapshot => ({
  ledger,
  commit_sha: "d".repeat(40),
  tree_sha: "e".repeat(40),
  state_ref_exists: true,
});

const planningPersistenceStore = (
  initial: SentinelRecoveryLedgerV1 = emptySentinelRecoveryLedger(),
): Readonly<{
  store: Readonly<{
    read(): Promise<SentinelRecoveryLedgerSnapshot>;
    write(
      snapshot: SentinelRecoveryLedgerSnapshot,
      ledger: SentinelRecoveryLedgerV1,
      message: string,
    ): Promise<SentinelRecoveryLedgerSnapshot>;
  }>;
  writes: ReadonlyArray<Readonly<{ ledger: SentinelRecoveryLedgerV1; message: string }>>;
}> => {
  let current = initial;
  const writes: { ledger: SentinelRecoveryLedgerV1; message: string }[] = [];
  return {
    writes,
    store: {
      read: () => Promise.resolve(planningLedgerSnapshot(current)),
      write: (_snapshot, ledger, message) => {
        writes.push({ ledger, message });
        current = ledger;
        return Promise.resolve(planningLedgerSnapshot(ledger));
      },
    },
  };
};

Deno.test("issue137 circuit reopen via generated generations cannot restart the unchanged source", async () => {
  const issue137 = sentinelGitHubIssue({
    id: 10_137,
    nodeId: "I_kwDOIssue137",
    number: 137,
    title: "Blocked unchanged source",
    body: ownerBacklogIssueBody(["src/quota_projection.ts"]),
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/137",
    labels: [],
    createdAt: "2026-08-27T19:06:03Z",
    updatedAt: "2026-08-27T19:07:27Z",
  });
  const laterIssue = sentinelGitHubIssue({
    id: 10_138,
    nodeId: "I_kwDOIssue138",
    number: 138,
    title: "Eligible later work advances past the blocked source",
    body: ownerBacklogIssueBody(["src/paid_fallback_ledger.ts"]),
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/138",
    labels: [],
    createdAt: "2026-08-27T19:06:04Z",
    updatedAt: "2026-08-27T19:07:28Z",
  });
  const source = githubIssueSource([issue137, laterIssue]);
  const selection = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
    new Date("2026-08-28T00:00:00Z"),
  );
  const fingerprint = (selection!.job)!.fingerprint;
  const revision = fingerprint;
  // Generation 1 opened the circuit (manual_required), generations 2-6 were
  // rejected on the same unchanged fingerprint, generation 7 is active.
  const records = Array.from({ length: 7 }, (_, index) => {
    const generation = index + 1;
    return recoveryRecordFixture({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "10137",
        source_revision: revision,
        candidate_generation: generation,
      },
      run_id: `run-${generation}`,
      attempt: generation,
      lease_token: `lease-${generation}`,
      phase: generation === 1 ? "manual_required" : generation === 7 ? "claimed" : "rejected",
      disposition: generation === 1 ? "manual_required" : generation === 7 ? "active" : "rejected",
      state_version: generation * 10,
      updated_at: new Date(Date.parse("2026-08-28T18:00:00.000Z") + generation * 1_000).toISOString(),
      reason: generation === 1 ? "Three identical Sentinel failure fingerprints opened the circuit breaker." : null,
      next_action: null,
    });
  });
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records });
  const skipped = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(ledger),
    new Date("2026-09-06T14:10:00Z"),
  );
  // The terminal circuit decision keeps the first item unavailable while the
  // later eligible item advances — no starvation and no circuit bypass.
  assert.equal(skipped?.job?.number, laterIssue.number);
  assert.notEqual(skipped?.job?.fingerprint, fingerprint);
  // The active generation 7 on the same fingerprint also stays unavailable to
  // a competing run even with a due-looking future retry because it is in
  // flight, and a malformed snapshot can never reopen it.
  await assert.rejects(
    () =>
      selectNextGitHubIssueJobSelection(
        source,
        "ubiquity/ai.ubq.fi",
        renderGitHubIssueJobLedger([]),
        recoverySelectionContext({
          ...emptySentinelRecoveryLedger(),
          records: [{ ...records[6]!, phase: "no_such_phase" }],
        } as unknown as SentinelRecoveryLedgerV1),
        new Date("2026-09-06T14:10:00Z"),
      ),
    /record is invalid|ledger is invalid/u,
  );
  // A malformed snapshot fails closed even when no candidate reaches the
  // detail loop: the recovery context is validated before the loop itself.
  await assert.rejects(
    () =>
      selectNextGitHubIssueJobSelection(
        githubIssueSource([]),
        "ubiquity/ai.ubq.fi",
        renderGitHubIssueJobLedger([]),
        recoverySelectionContext({
          ...emptySentinelRecoveryLedger(),
          records: [{ ...records[6]!, phase: "no_such_phase" }],
        } as unknown as SentinelRecoveryLedgerV1),
        new Date("2026-09-06T14:10:00Z"),
      ),
    /record is invalid|ledger is invalid/u,
  );
  assert.throws(
    () =>
      selectNextReviewBacklogEntry(
        renderReviewBacklog([]),
        recoverySelectionContext({
          ...emptySentinelRecoveryLedger(),
          records: [{ ...records[6]!, phase: "no_such_phase" }],
        } as unknown as SentinelRecoveryLedgerV1),
      ),
    /record is invalid|ledger is invalid/u,
  );
});

Deno.test("an unavailable first option never starves later eligible work and convergence continues its own claim", async () => {
  const firstIssue = sentinelGitHubIssue();
  const secondIssue = sentinelGitHubIssue({
    id: 10_114,
    nodeId: "I_kwDOIssue114",
    number: 114,
    title: "Implement the next eligible issue",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/114",
    createdAt: "2026-08-23T19:06:04Z",
    updatedAt: "2026-08-23T19:07:28Z",
  });
  const source = githubIssueSource([firstIssue, secondIssue]);
  const selection = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
    new Date("2026-08-23T20:00:00Z"),
  );
  const firstJob = selection!.job!;
  const terminal = recoveryRecordFixture({
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: String(firstIssue.id),
      source_revision: firstJob.fingerprint,
      candidate_generation: 1,
    },
    phase: "manual_required",
    disposition: "manual_required",
    state_version: 9,
    updated_at: "2026-08-28T18:05:00.000Z",
    reason: "Owner review is required before another Sentinel attempt.",
    next_action: null,
  });
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [terminal] });
  const skippedFirst = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(ledger),
    new Date("2026-08-24T09:00:00Z"),
  );
  assert.equal(skippedFirst?.job?.number, secondIssue.number);
  // The convergence continuation of the run's own exact record is selected;
  // its own active claimed record must not be mistaken for a fresh claim even
  // though it is not due for a retry.
  const claimed = recoveryRecordFixture({
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: String(firstIssue.id),
      source_revision: firstJob.fingerprint,
      candidate_generation: 2,
    },
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
    run_id: "prepare-run",
    lease_token: "prepare-lease",
    attempt: 2,
    updated_at: "2026-08-28T18:20:00.000Z",
    reason: "The selected work item is durably claimed before implementation.",
    next_action: "Start the Luna implementation stage.",
  });
  const activeLedger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [claimed] });
  const continued = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(activeLedger, claimed),
    new Date("2026-08-28T18:30:00Z"),
  );
  assert.equal(continued?.job?.number, firstIssue.number);
  // A different run has no continuation identity: the active claim is
  // unavailable and later work wins instead of starving.
  const competing = await selectNextGitHubIssueJobSelection(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(activeLedger),
    new Date("2026-08-28T18:30:00Z"),
  );
  assert.equal(competing?.job?.number, secondIssue.number);
});

const recoveryFilePermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
]);
const recoveryFileTestsIgnored = recoveryFilePermissions.some(
  (permission) => permission.state !== "granted",
);

Deno.test({
  name: "zero-cell matrix convergence does not require a prepared recovery record",
  ignore: recoveryFileTestsIgnored,
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-convergence-recovery-" });
    try {
      // A no-actionable-work plan intentionally has no recovery record; the fast
      // path must not demand the file.
      assert.equal(await loadPreparedConvergenceRecoveryRecord(`${root}/reports`), null);
      // The exact prepared record is loaded for cell-bearing convergence.
      const prepared = recoveryRecordFixture({
        identity: {
          repository: "ubiquity/ai.ubq.fi",
          source_kind: "github_issue",
          source_id: "10113",
          source_revision: "a".repeat(64),
          candidate_generation: 1,
        },
        phase: "claimed",
        state_version: 1,
        candidate_branch: null,
        candidate_sha: null,
      });
      await Deno.mkdir(`${root}/reports`, { recursive: true });
      await Deno.writeTextFile(`${root}/reports/recovery-record-v1.json`, `${JSON.stringify(prepared)}\n`);
      assert.deepEqual(await loadPreparedConvergenceRecoveryRecord(`${root}/reports`), prepared);
      // A malformed prepared record fails closed.
      await Deno.writeTextFile(`${root}/reports/recovery-record-v1.json`, `{"schema_version": 1}\n`);
      await assert.rejects(loadPreparedConvergenceRecoveryRecord(`${root}/reports`), /record is invalid|phase and/u);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test("sentinel schedule windows overlap hourly and incident runs", () => {
  const hourly = computeSentinelInterval("hourly", now);
  const delayedHourly = computeSentinelInterval("hourly", now + 70 * 60_000);
  const incident = computeSentinelInterval("incident", now);
  const nextIncident = computeSentinelInterval("incident", now + 5 * 60_000);
  const observe = computeSentinelInterval("observe", now);
  const preview = computeSentinelInterval("preview", now);
  assert.equal(HOURLY_OVERLAP_MS, 20 * 60_000);
  assert.equal(hourly.duration_ms, HOURLY_WINDOW_MS);
  assert.equal(hourly.start, "2026-08-21T04:40:00.000Z");
  assert.ok(Date.parse(delayedHourly.start) <= Date.parse(hourly.end));
  assert.equal(incident.duration_ms, INCIDENT_WINDOW_MS);
  assert.equal(incident.start, "2026-08-21T05:40:00.000Z");
  assert.equal(Date.parse(incident.end) - Date.parse(nextIncident.start), 15 * 60_000);
  assert.equal(observe.duration_ms, OBSERVE_WINDOW_MS);
  assert.equal(observe.start, "2026-08-21T03:55:00.000Z");
  assert.deepEqual(preview, incident);
});

Deno.test("durable incident windows include the first failure within replay retention", () => {
  const firstFailure = now - 2 * 60 * 60_000;
  const interval = computeSentinelInterval("incident", now, firstFailure);
  assert.equal(interval.start, new Date(firstFailure).toISOString());
  assert.equal(interval.duration_ms, 2 * 60 * 60_000);
  assert.equal(parseIncidentStartMs("incident", String(firstFailure)), firstFailure);
  assert.throws(() => parseIncidentStartMs("incident", undefined), /positive integer/);
  assert.throws(() => parseIncidentStartMs("hourly", String(firstFailure)), /Only incident mode/);
  assert.throws(() => computeSentinelInterval("incident", now, now - 49 * 60 * 60_000), /retained interval/);
  assert.throws(() => computeSentinelInterval("hourly", now, firstFailure), /Only incident mode/);
});

Deno.test("deployment monitoring keeps 30-second identity probes and reports every five minutes", () => {
  assert.equal(SENTINEL_POLICY.monitorPollMs, 30_000);
  assert.equal(SENTINEL_POLICY.monitorCheckpointMs, 5 * 60_000);
  assert.equal(SENTINEL_POLICY.monitorCheckpointMs % SENTINEL_POLICY.monitorPollMs, 0);
  assert.equal(SENTINEL_POLICY.monitorDurationMs % SENTINEL_POLICY.monitorCheckpointMs, 0);
  assert.equal(MONITOR_AGENT_MS, 5 * 60_000);
});

Deno.test("observe mode is triage-only and never enables autonomous repair", () => {
  assert.equal(parseMode(["--mode", "observe"]), "observe");
  assert.equal(isObserveOnlyMode("observe"), true);
  assert.equal(isObserveOnlyMode("incident"), false);
  assert.equal(isAutonomousMode("observe"), false);
  assert.throws(() => parseMode(["--mode", "unknown"]), /hourly\|incident\|observe\|preview/);
});

Deno.test("every structured-output property declares an explicit JSON Schema type", () => {
  const visit = (value: unknown, path: string): void => {
    if (typeof value !== "object" || value === null || Array.isArray(value)) return;
    const schema = value as Record<string, unknown>;
    if (typeof schema.properties === "object" && schema.properties !== null && !Array.isArray(schema.properties)) {
      for (const [name, property] of Object.entries(schema.properties)) {
        assert.equal(
          typeof property === "object" && property !== null && !Array.isArray(property) && "type" in property,
          true,
          `${path}.properties.${name} must declare type`,
        );
        visit(property, `${path}.properties.${name}`);
      }
    }
    if (schema.items !== undefined) visit(schema.items, `${path}.items`);
  };

  visit(TRIAGE_OUTPUT_SCHEMA, "triage");
  visit(IMPLEMENTATION_OUTPUT_SCHEMA, "implementation");
  visit(MONITOR_OUTPUT_SCHEMA, "monitor");
});

Deno.test({
  name: "matrix convergence accepts a bounded linear review-result-only advance",
  ignore: matrixVerifierTestsIgnored,
  async fn() {
    const fixture = await createMatrixVerifierRepository();
    try {
      const firstHeadSha = "a".repeat(40);
      await writeMatrixVerifierResult(
        fixture.repo,
        801,
        firstHeadSha,
        rollingReviewResultFixture(801, firstHeadSha),
      );
      await commitMatrixVerifierFixture(fixture.repo, "first rolling review result");

      const secondHeadSha = "c".repeat(40);
      await writeMatrixVerifierResult(
        fixture.repo,
        802,
        secondHeadSha,
        rollingReviewResultFixture(802, secondHeadSha),
      );
      const currentDevelopmentSha = await commitMatrixVerifierFixture(fixture.repo, "second rolling review result");

      await assert.doesNotReject(
        verifyMatrixConvergenceAdvance(fixture.repo, fixture.baseSha, currentDevelopmentSha),
      );
      assert.equal(await matrixVerifierGit(fixture.repo, ["rev-parse", "HEAD"]), currentDevelopmentSha);
    } finally {
      await Deno.remove(fixture.repo, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix convergence rejects unsafe history and rolling-result advances",
  ignore: matrixVerifierTestsIgnored,
  async fn() {
    const cases: readonly MatrixVerifierNegativeCase[] = [
      {
        name: "empty advance",
        prepare: ({ baseSha }) => Promise.resolve({ plannedBaseSha: baseSha, currentDevelopmentSha: baseSha }),
        error: /advance is empty/u,
      },
      {
        name: "non-ancestor planned base",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(repo, 810, headSha, rollingReviewResultFixture(810, headSha));
          const descendantSha = await commitMatrixVerifierFixture(repo, "descendant");
          return { plannedBaseSha: descendantSha, currentDevelopmentSha: baseSha };
        },
        error: /not an ancestor/u,
      },
      {
        name: "more than eight commits",
        prepare: async ({ repo, baseSha }) => {
          let currentDevelopmentSha = baseSha;
          for (let index = 0; index < 9; index++) {
            const headSha = index.toString(16).repeat(40);
            await writeMatrixVerifierResult(
              repo,
              820 + index,
              headSha,
              rollingReviewResultFixture(820 + index, headSha),
            );
            currentDevelopmentSha = await commitMatrixVerifierFixture(repo, `rolling result ${index}`);
          }
          return { plannedBaseSha: baseSha, currentDevelopmentSha };
        },
        error: /allowed commit count/u,
      },
      {
        name: "merge commit",
        prepare: async ({ repo, baseSha }) => {
          await matrixVerifierGit(repo, ["switch", "-c", "side"]).then(() => undefined);
          const sideHeadSha = "a".repeat(40);
          await writeMatrixVerifierResult(repo, 830, sideHeadSha, rollingReviewResultFixture(830, sideHeadSha));
          await commitMatrixVerifierFixture(repo, "side rolling result");
          await matrixVerifierGit(repo, ["switch", "development"]);
          const mainHeadSha = "c".repeat(40);
          await writeMatrixVerifierResult(repo, 831, mainHeadSha, rollingReviewResultFixture(831, mainHeadSha));
          await commitMatrixVerifierFixture(repo, "main rolling result");
          await matrixVerifierGit(repo, ["merge", "--no-ff", "side", "-m", "merge rolling result branches"]);
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await matrixVerifierGit(repo, ["rev-parse", "HEAD"]),
          };
        },
        error: /not (?:a single-parent commit|on the planned linear path)/u,
      },
      {
        name: "delete",
        prepare: async ({ repo }) => {
          const headSha = "a".repeat(40);
          const path = matrixVerifierResultPath(840, headSha);
          await writeMatrixVerifierResult(repo, 840, headSha, rollingReviewResultFixture(840, headSha));
          const plannedBaseSha = await commitMatrixVerifierFixture(repo, "existing rolling result");
          await matrixVerifierGit(repo, ["rm", "--", path]);
          return {
            plannedBaseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "delete rolling result", false),
          };
        },
        error: /not a single-file add/u,
      },
      {
        name: "rename",
        prepare: async ({ repo }) => {
          const oldHeadSha = "a".repeat(40);
          const newHeadSha = "c".repeat(40);
          const oldPath = matrixVerifierResultPath(850, oldHeadSha);
          const newPath = matrixVerifierResultPath(851, newHeadSha);
          await writeMatrixVerifierResult(repo, 850, oldHeadSha, rollingReviewResultFixture(850, oldHeadSha));
          const plannedBaseSha = await commitMatrixVerifierFixture(repo, "existing result before rename");
          await matrixVerifierGit(repo, ["mv", "--", oldPath, newPath]);
          return {
            plannedBaseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "rename rolling result"),
          };
        },
        error: /changes multiple paths/u,
      },
      {
        name: "symlink result",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          const path = matrixVerifierResultPath(855, headSha);
          await Deno.mkdir(`${repo}/docs/sentinel-review-results`, { recursive: true });
          await Deno.symlink("not-a-regular-result", `${repo}/${path}`);
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "symlink rolling result"),
          };
        },
        error: /regular 100644 Git blob/u,
      },
      {
        name: "configured external diff cannot hide mixed paths",
        prepare: async ({ repo, baseSha }) => {
          await matrixVerifierGit(repo, ["config", "diff.external", "/bin/false"]);
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(repo, 856, headSha, rollingReviewResultFixture(856, headSha));
          await Deno.writeTextFile(`${repo}/docs/external-diff-unrelated.md`, "unrelated\n");
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "external diff mixed paths"),
          };
        },
        error: /changes multiple paths/u,
      },
      {
        name: "configured textconv cannot hide mixed paths",
        prepare: async ({ repo }) => {
          const headSha = "a".repeat(40);
          await Deno.writeTextFile(`${repo}/.gitattributes`, "README.md diff=sentinel-verifier-textconv\n");
          await matrixVerifierGit(repo, ["config", "diff.sentinel-verifier-textconv.textconv", "/bin/false"]);
          const plannedBaseSha = await commitMatrixVerifierFixture(repo, "configure verifier textconv");
          await writeMatrixVerifierResult(repo, 857, headSha, rollingReviewResultFixture(857, headSha));
          await Deno.writeTextFile(`${repo}/README.md`, "changed by textconv fixture\n");
          return {
            plannedBaseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "textconv mixed paths"),
          };
        },
        error: /changes multiple paths/u,
      },
      {
        name: "mixed result and unrelated paths",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(repo, 860, headSha, rollingReviewResultFixture(860, headSha));
          await Deno.writeTextFile(`${repo}/docs/mixed-path.md`, "unrelated\n");
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "mixed result and unrelated path"),
          };
        },
        error: /changes multiple paths/u,
      },
      {
        name: "two result files in one commit",
        prepare: async ({ repo, baseSha }) => {
          const firstHeadSha = "a".repeat(40);
          const secondHeadSha = "c".repeat(40);
          await writeMatrixVerifierResult(repo, 870, firstHeadSha, rollingReviewResultFixture(870, firstHeadSha));
          await writeMatrixVerifierResult(repo, 871, secondHeadSha, rollingReviewResultFixture(871, secondHeadSha));
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "two rolling results"),
          };
        },
        error: /changes multiple paths/u,
      },
      {
        name: "unrelated path",
        prepare: async ({ repo, baseSha }) => {
          await Deno.mkdir(`${repo}/src`, { recursive: true });
          await Deno.writeTextFile(`${repo}/src/unrelated.ts`, "export const unrelated = true;\n");
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "unrelated path"),
          };
        },
        error: /outside the review results/u,
      },
      {
        name: "near-match result directory",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await Deno.mkdir(`${repo}/docs/sentinel-review-results-bak`, { recursive: true });
          await Deno.writeTextFile(
            `${repo}/docs/sentinel-review-results-bak/880-${headSha}.json`,
            JSON.stringify(rollingReviewResultFixture(880, headSha)),
          );
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "near-match result directory"),
          };
        },
        error: /outside the review results/u,
      },
      {
        name: "invalid result filename",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(
            repo,
            890,
            headSha,
            rollingReviewResultFixture(890, headSha),
            "docs/sentinel-review-results/not-a-result.json",
          );
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "invalid result filename"),
          };
        },
        error: /file name is invalid/u,
      },
      {
        name: "nested result filename",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await Deno.mkdir(`${repo}/docs/sentinel-review-results/nested`, { recursive: true });
          await Deno.writeTextFile(
            `${repo}/docs/sentinel-review-results/nested/891-${headSha}.json`,
            JSON.stringify(rollingReviewResultFixture(891, headSha)),
          );
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "nested result filename"),
          };
        },
        error: /file name is invalid/u,
      },
      {
        name: "invalid JSON body",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(repo, 900, headSha, "not-json");
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "invalid JSON body"),
          };
        },
        error: /not valid JSON/u,
      },
      {
        name: "body PR mismatch",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(
            repo,
            910,
            headSha,
            rollingReviewResultFixture(910, headSha, { pr_number: 911 }),
          );
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "body PR mismatch"),
          };
        },
        error: /pull request number is invalid/u,
      },
      {
        name: "body head mismatch",
        prepare: async ({ repo, baseSha }) => {
          const fileHeadSha = "a".repeat(40);
          await writeMatrixVerifierResult(
            repo,
            920,
            fileHeadSha,
            rollingReviewResultFixture(920, fileHeadSha, { head_sha: "c".repeat(40) }),
          );
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "body head mismatch"),
          };
        },
        error: /head SHA does not match its file identity/u,
      },
      {
        name: "modified result instead of add",
        prepare: async ({ repo }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(repo, 930, headSha, rollingReviewResultFixture(930, headSha));
          const plannedBaseSha = await commitMatrixVerifierFixture(repo, "existing result before modification");
          await writeMatrixVerifierResult(
            repo,
            930,
            headSha,
            rollingReviewResultFixture(930, headSha, { raw_review_text: "modified" }),
          );
          return {
            plannedBaseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "modify rolling result"),
          };
        },
        error: /not a single-file add/u,
      },
      {
        name: "internally inconsistent body",
        prepare: async ({ repo, baseSha }) => {
          const headSha = "a".repeat(40);
          await writeMatrixVerifierResult(
            repo,
            940,
            headSha,
            rollingReviewResultFixture(940, headSha, { failure: "unexpected failure" }),
          );
          return {
            plannedBaseSha: baseSha,
            currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "inconsistent result body"),
          };
        },
        error: /internally inconsistent/u,
      },
      {
        name: "empty commit",
        prepare: async ({ repo, baseSha }) => ({
          plannedBaseSha: baseSha,
          currentDevelopmentSha: await commitMatrixVerifierFixture(repo, "empty commit", true),
        }),
        error: /changes multiple paths/u,
      },
    ];

    for (const testCase of cases) {
      const fixture = await createMatrixVerifierRepository();
      try {
        const advance = await testCase.prepare(fixture);
        await assert.rejects(
          verifyMatrixConvergenceAdvance(fixture.repo, advance.plannedBaseSha, advance.currentDevelopmentSha),
          testCase.error,
          testCase.name,
        );
      } finally {
        await Deno.remove(fixture.repo, { recursive: true });
      }
    }
  },
});

Deno.test("matrix convergence compares triage ownership in planner fingerprint order", async () => {
  const interval = computeSentinelInterval("incident", now);
  const finding = (id: string, fingerprint: string, path: string): TriageReport["findings"][number] => ({
    id,
    fingerprint,
    severity: "P1",
    title: `Repair ${id}`,
    affected_surface: path,
    allowed_paths: [path],
    shared_paths: [],
    depends_on: [],
    evidence: [{ source: "repository", reference: path, detail: `Defect in ${id}` }],
    proposed_correction: `Correct ${id}`,
    validation_requirements: [`Validate ${id}`],
    actionable: true,
  });
  const triage: TriageReport = {
    schema_version: 1,
    interval,
    findings: [
      finding("first-by-id", "f".repeat(64), "src/first.ts"),
      finding("second-by-id", "0".repeat(64), "src/second.ts"),
    ],
    no_findings_reason: null,
  };
  const plan = await buildMatrixPlan({
    run_id: "ownership-order",
    run_attempt: 1,
    base_sha: "a".repeat(40),
    evidence_digests: [],
    findings: triage.findings.map((item) => ({
      id: item.id,
      fingerprint: item.fingerprint,
      allowed_paths: item.allowed_paths,
      prohibited_paths: SENTINEL_POLICY.protectedImplementationPaths,
      shared_paths: item.shared_paths,
      depends_on: item.depends_on,
      validation_requirements: item.validation_requirements,
    })),
  });
  assert.doesNotThrow(() => assertTriageMatchesMatrixPlan(triage, plan));
});

Deno.test("matrix convergence accepts production-canonical persisted plan ownership", async () => {
  const interval = computeSentinelInterval("incident", now);
  const finding = (
    id: string,
    fingerprint: string,
    allowedPaths: string[],
    sharedPaths: string[],
    dependsOn: string[],
    validations: string[],
  ): TriageReport["findings"][number] => ({
    id,
    fingerprint,
    severity: "P1",
    title: `Repair ${id}`,
    affected_surface: allowedPaths[0]!,
    allowed_paths: allowedPaths,
    shared_paths: sharedPaths,
    depends_on: dependsOn,
    evidence: [{ source: "repository", reference: allowedPaths[0]!, detail: `Defect in ${id}` }],
    proposed_correction: `Correct ${id}`,
    validation_requirements: validations,
    actionable: true,
  });
  // Production shape: findings declared out of fingerprint order and with
  // unsorted path, dependency, and validation arrays. IDs mix punctuation and
  // letter case so ordering must be canonical, not linguistic.
  const triage: TriageReport = {
    schema_version: 1,
    interval,
    findings: [
      finding(
        "triage.Z-pattern:repair",
        "f".repeat(64),
        ["src/z-pattern.ts", "src/a-pattern.ts"],
        ["tests/z-pattern.test.ts", "tests/a-pattern.test.ts"],
        ["triage.A-pattern:repair"],
        ["deno task sentinel:test-local", "deno fmt --check"],
      ),
      finding(
        "triage.A-pattern:repair",
        "0".repeat(64),
        ["src/b-pattern.ts", "src/c-pattern.ts"],
        ["tests/b-pattern.test.ts"],
        [],
        ["deno check scripts/sentinel/main.ts"],
      ),
    ],
    no_findings_reason: null,
  };
  const plan = await buildMatrixPlan({
    run_id: "production-ownership",
    run_attempt: 1,
    base_sha: "a".repeat(40),
    evidence_digests: [],
    findings: triage.findings.map((item) => ({
      id: item.id,
      fingerprint: item.fingerprint,
      allowed_paths: item.allowed_paths,
      prohibited_paths: SENTINEL_POLICY.protectedImplementationPaths,
      shared_paths: item.shared_paths,
      depends_on: item.depends_on,
      validation_requirements: item.validation_requirements,
    })),
  });
  // The converge job persists the plan with canonicalMatrixJson (sorted object
  // keys) and main.ts parses that exact text back through parseMatrixPlanV1.
  const persisted = `${canonicalMatrixJson(plan)}\n`;
  const reparsed = await parseMatrixPlanV1(persisted);
  assert.equal(Object.keys(reparsed.ownership[0]!)[0], "allowed_paths");
  assert.doesNotThrow(() => assertTriageMatchesMatrixPlan(triage, reparsed));
});

Deno.test("matrix convergence rejects genuine ownership changes after canonical persistence", async () => {
  const interval = computeSentinelInterval("incident", now);
  const finding = (id: string, fingerprint: string, path: string): TriageReport["findings"][number] => ({
    id,
    fingerprint,
    severity: "P1",
    title: `Repair ${id}`,
    affected_surface: path,
    allowed_paths: [path],
    shared_paths: [],
    depends_on: [],
    evidence: [{ source: "repository", reference: path, detail: `Defect in ${id}` }],
    proposed_correction: `Correct ${id}`,
    validation_requirements: [`Validate ${id}`],
    actionable: true,
  });
  const triage: TriageReport = {
    schema_version: 1,
    interval,
    findings: [
      finding("first-by-id", "f".repeat(64), "src/first.ts"),
      finding("second-by-id", "0".repeat(64), "src/second.ts"),
    ],
    no_findings_reason: null,
  };
  const planInput = (prohibited: string[]): Array<{
    id: string;
    fingerprint: string;
    allowed_paths: string[];
    prohibited_paths: string[];
    shared_paths: string[];
    depends_on: string[];
    validation_requirements: string[];
  }> =>
    triage.findings.map((item) => ({
      id: item.id,
      fingerprint: item.fingerprint,
      allowed_paths: [...item.allowed_paths],
      prohibited_paths: prohibited,
      shared_paths: [...item.shared_paths],
      depends_on: [...item.depends_on],
      validation_requirements: [...item.validation_requirements],
    }));

  // An immutable plan whose prohibited paths differ from the current policy
  // (for example, a plan built before the policy was extended).
  const reparseWithProhibited = async (prohibited: string[]): Promise<MatrixPlanV1> => {
    const plan = await buildMatrixPlan({
      run_id: "ownership-mismatch",
      run_attempt: 1,
      base_sha: "a".repeat(40),
      evidence_digests: [],
      findings: planInput(prohibited),
    });
    return await parseMatrixPlanV1(`${canonicalMatrixJson(plan)}\n`);
  };
  const variant = (overrides: {
    fingerprint?: string;
    allowedPaths?: string[];
    dependsOn?: string[];
    validations?: string[];
  }): TriageReport => ({
    schema_version: 1,
    interval,
    findings: [
      {
        ...triage.findings[0]!,
        fingerprint: overrides.fingerprint ?? triage.findings[0]!.fingerprint,
        allowed_paths: overrides.allowedPaths ?? triage.findings[0]!.allowed_paths,
      },
      {
        ...triage.findings[1]!,
        depends_on: overrides.dependsOn ?? triage.findings[1]!.depends_on,
        validation_requirements: overrides.validations ?? triage.findings[1]!.validation_requirements,
      },
    ],
    no_findings_reason: null,
  });

  // The legitimate production round trip is accepted.
  const canonical = await reparseWithProhibited([...SENTINEL_POLICY.protectedImplementationPaths]);
  assert.doesNotThrow(() => assertTriageMatchesMatrixPlan(triage, canonical));

  const mismatch = /Matrix convergence triage does not match immutable plan ownership/;
  assert.throws(
    () => assertTriageMatchesMatrixPlan(variant({ fingerprint: "e".repeat(64) }), canonical),
    mismatch,
  );
  assert.throws(
    () => assertTriageMatchesMatrixPlan(variant({ allowedPaths: ["src/other.ts"] }), canonical),
    mismatch,
  );
  assert.throws(
    () => assertTriageMatchesMatrixPlan(variant({ dependsOn: ["first-by-id"] }), canonical),
    mismatch,
  );
  assert.throws(
    () => assertTriageMatchesMatrixPlan(variant({ validations: ["Validate something else"] }), canonical),
    mismatch,
  );

  const changedPlanProhibited = await reparseWithProhibited([
    ...SENTINEL_POLICY.protectedImplementationPaths,
    "scripts/sentinel/matrix.ts",
  ]);
  assert.throws(() => assertTriageMatchesMatrixPlan(triage, changedPlanProhibited), mismatch);
});

Deno.test("observe cycle cannot reach replay, repair, Git, deployment, promotion, or rollback capabilities", async () => {
  const interval = computeSentinelInterval("observe", now);
  const triage: TriageReport = {
    schema_version: 1,
    interval,
    findings: [
      {
        id: "finding-1",
        fingerprint: "0123456789abcdef".repeat(4),
        severity: "P1",
        title: "Actionable provider failure",
        affected_surface: "/v1/responses",
        allowed_paths: ["src/provider.ts"],
        shared_paths: [],
        depends_on: [],
        evidence: [{ source: "deno_log", reference: "line:1", detail: "provider transport failed" }],
        proposed_correction: "Repair the provider transport path.",
        validation_requirements: ["Replay the failed request."],
        actionable: true,
      },
      {
        id: "finding-2",
        fingerprint: "fedcba9876543210".repeat(4),
        severity: "P3",
        title: "Efficiency opportunity",
        affected_surface: "provider catalog",
        allowed_paths: ["src/provider.ts"],
        shared_paths: [],
        depends_on: [],
        evidence: [{ source: "repository", reference: "src/provider.ts", detail: "duplicate lookup" }],
        proposed_correction: "Reuse the existing lookup.",
        validation_requirements: ["Measure lookup count."],
        actionable: false,
      },
    ],
    no_findings_reason: null,
  };
  const callOrder: string[] = [];
  const forbiddenAccesses: string[] = [];
  const forbiddenCapabilities = new Set([
    "exportReplay",
    "runReplay",
    "implement",
    "review",
    "writeGit",
    "deploy",
    "promote",
    "rollback",
  ]);
  const dependencies = new Proxy({
    capture: () => {
      callOrder.push("capture");
      return Promise.resolve({ path: "/private/raw.jsonl", byte_count: 41, sha256: "a".repeat(64) });
    },
    analyze: () => {
      callOrder.push("analyze");
      return Promise.resolve({
        triage,
        invocation: {
          slot: 1 as const,
          headroomPercent: 73,
          probes: [
            { kind: "available" as const, slot: 1 as const, headroomPercent: 73, observedAtMs: now },
            { kind: "available" as const, slot: 2 as const, headroomPercent: 62, observedAtMs: now },
          ] as const,
          stdout: "",
          stderr: "",
          lastMessage: null,
          nativeReviewOutput: null,
        },
      });
    },
    verifyEvidence: () => {
      callOrder.push("verifyEvidence");
      return Promise.resolve();
    },
    writeTriage: () => {
      callOrder.push("writeTriage");
      return Promise.resolve();
    },
    writeObservation: () => {
      callOrder.push("writeObservation");
      return Promise.resolve();
    },
    complete: () => {
      callOrder.push("complete");
      return Promise.resolve();
    },
    exportReplay: () => Promise.resolve(),
    runReplay: () => Promise.resolve(),
    implement: () => Promise.resolve(),
    review: () => Promise.resolve(),
    writeGit: () => Promise.resolve(),
    deploy: () => Promise.resolve(),
    promote: () => Promise.resolve(),
    rollback: () => Promise.resolve(),
  }, {
    get(target, property, receiver) {
      if (typeof property === "string" && forbiddenCapabilities.has(property)) forbiddenAccesses.push(property);
      return Reflect.get(target, property, receiver);
    },
  });

  const observation = await runObserveCycle(interval, dependencies);

  assert.deepEqual(callOrder, [
    "capture",
    "analyze",
    "verifyEvidence",
    "writeTriage",
    "writeObservation",
    "complete",
  ]);
  assert.deepEqual(forbiddenAccesses, []);
  assert.deepEqual(observation.findings, {
    total: 2,
    actionable: 1,
    by_severity: { P0: 0, P1: 1, P2: 0, P3: 1 },
  });
  assert.deepEqual(observation.raw_log, { byte_count: 41, sha256: "a".repeat(64) });
});

Deno.test("sentinel schedule windows anchor to the immutable GitHub run creation time", () => {
  const invoked = Date.parse("2026-08-21T06:17:00.000Z");
  const created = "2026-08-21T06:00:03.000Z";
  assert.equal(resolveCycleAnchorMs(created, invoked), Date.parse(created));
  assert.equal(resolveCycleAnchorMs(null, invoked), invoked);
  assert.throws(() => resolveCycleAnchorMs("invalid", invoked), /timestamp is invalid/);
  assert.throws(
    () => resolveCycleAnchorMs("2026-08-21T06:23:00.000Z", invoked),
    /unexpectedly in the future/,
  );
});

Deno.test("sentinel deployment dispatches are build-only for preview and production candidates", () => {
  const correlationId = "sentinel-1234567890abcdef";
  assert.deepEqual(sentinelDeploymentInputs(true, correlationId), {
    deploy_preview: true,
    sentinel_build_only: true,
    sentinel_correlation_id: correlationId,
  });
  assert.deepEqual(sentinelDeploymentInputs(false, correlationId), {
    deploy_preview: false,
    sentinel_build_only: true,
    sentinel_correlation_id: correlationId,
  });
  assert.throws(() => sentinelDeploymentInputs(true, "short"), /correlation ID is invalid/);
});

Deno.test("GitHub issue rollback preserves the terminal issue-job ledger", () => {
  const baseSha = "1".repeat(40);
  const candidateSha = "2".repeat(40);
  const common = ["diff", "--no-ext-diff", "--no-textconv", "--binary", candidateSha, baseSha];

  assert.deepEqual(candidateRevertDiffArguments(baseSha, candidateSha, false), common);
  assert.deepEqual(candidateRevertDiffArguments(baseSha, candidateSha, true), [
    ...common,
    "--",
    ".",
    ":(exclude)docs/sentinel-issue-jobs.md",
  ]);
});

Deno.test("sentinel deployment attestation is bound to one run, app, SHA, and revision", () => {
  const gitSha = "2".repeat(40);
  const value = {
    schema_version: 1,
    run_id: 41,
    app: "p-ai-ubq-fi",
    git_sha: gitSha,
    revision: "revision-41",
  };
  assert.deepEqual(
    parseSentinelDeploymentAttestation(value, { runId: 41, app: "p-ai-ubq-fi", gitSha }),
    value,
  );
  assert.throws(
    () => parseSentinelDeploymentAttestation(value, { runId: 42, app: "p-ai-ubq-fi", gitSha }),
    /does not match/,
  );
  assert.throws(
    () =>
      parseSentinelDeploymentAttestation({ ...value, extra: true }, {
        runId: 41,
        app: "p-ai-ubq-fi",
        gitSha,
      }),
    /does not match/,
  );
});

Deno.test("sentinel revision-control dispatch carries exact current, target, and development identities", () => {
  const targetGitSha = "2".repeat(40);
  const currentGitSha = "1".repeat(40);
  assert.deepEqual(
    sentinelRevisionControlInputs({
      correlationId: "sentinel:12345678",
      app: "ai-ubq-fi",
      targetGitSha,
      targetRevision: "target-revision",
      expectedCurrent: {
        gitSha: currentGitSha,
        revisionId: "current-revision",
        healthUrls: ["https://example.test/health"],
        snapshottedAt: "2026-08-21T00:00:00.000Z",
      },
      expectedDevelopmentGitSha: targetGitSha,
    }),
    {
      correlation_id: "sentinel:12345678",
      target_app: "ai-ubq-fi",
      target_git_sha: targetGitSha,
      target_revision: "target-revision",
      expected_current_git_sha: currentGitSha,
      expected_current_revision: "current-revision",
      expected_development_git_sha: targetGitSha,
    },
  );
});

Deno.test("rollback preflight requires known Git and production identities before promotion", () => {
  const baseSha = "1".repeat(40);
  const candidateSha = "2".repeat(40);
  const previousProduction = { gitSha: baseSha, revisionId: "previous-revision" };
  assert.deepEqual(
    evaluateRollbackPreflight({
      observedDevelopmentSha: candidateSha,
      baseSha,
      candidateSha,
      candidateRevisionId: "candidate-revision",
      observedProduction: { gitSha: candidateSha, revisionId: "candidate-revision" },
      previousProduction,
    }),
    { promotePrevious: true, revertDevelopment: true },
  );
  assert.deepEqual(
    evaluateRollbackPreflight({
      observedDevelopmentSha: baseSha,
      baseSha,
      candidateSha,
      candidateRevisionId: null,
      observedProduction: previousProduction,
      previousProduction,
    }),
    { promotePrevious: false, revertDevelopment: false },
  );
  assert.deepEqual(
    evaluateRollbackPreflight({
      observedDevelopmentSha: candidateSha,
      baseSha,
      candidateSha,
      candidateRevisionId: null,
      observedProduction: { gitSha: candidateSha, revisionId: "candidate-routed-before-resolution" },
      previousProduction,
    }),
    { promotePrevious: true, revertDevelopment: true },
  );
  assert.throws(
    () =>
      evaluateRollbackPreflight({
        observedDevelopmentSha: "3".repeat(40),
        baseSha,
        candidateSha,
        candidateRevisionId: "candidate-revision",
        observedProduction: { gitSha: candidateSha, revisionId: "candidate-revision" },
        previousProduction,
      }),
    /origin\/development advanced/,
  );
  assert.throws(
    () =>
      evaluateRollbackPreflight({
        observedDevelopmentSha: candidateSha,
        baseSha,
        candidateSha,
        candidateRevisionId: "candidate-revision",
        observedProduction: { gitSha: "3".repeat(40), revisionId: "external-revision" },
        previousProduction,
      }),
    /Production identity changed/,
  );
});

Deno.test("retained replay loading fails closed on aggregate budgets and deduplicates fingerprints", () => {
  assert.doesNotThrow(() => assertRetainedReplayArtifactBudget([{ sizeInBytes: MAX_MATCHING_REPLAY_ARCHIVE_BYTES }]));
  assert.throws(
    () =>
      assertRetainedReplayArtifactBudget([
        { sizeInBytes: MAX_MATCHING_REPLAY_ARCHIVE_BYTES },
        { sizeInBytes: 1 },
      ]),
    /aggregate archive byte limit/,
  );
  assert.throws(
    () =>
      assertRetainedReplayArtifactBudget(
        Array.from({ length: MAX_MATCHING_REPLAY_ARTIFACTS + 1 }, () => ({ sizeInBytes: 0 })),
      ),
    /count limit/,
  );

  const capture = (fingerprint: string, capturedAtMs: number): ExportedSentinelReplayCapture => ({
    manifest: {
      version: 1,
      capture_id: `${fingerprint}-${capturedAtMs}`,
      fingerprint,
      case_group_digest: "a".repeat(64),
      captured_at_ms: capturedAtMs,
      expires_at_ms: capturedAtMs + 1,
      algorithm: "AES-256-GCM",
      compression: "gzip",
      iv: "iv",
      chunk_count: 1,
      ciphertext_bytes: 1,
    },
    chunks: ["ciphertext"],
  });
  const newest = capture("f".repeat(64), 2);
  const olderDuplicate = capture("f".repeat(64), 1);
  const distinct = capture("e".repeat(64), 0);
  assert.deepEqual(deduplicateRetainedReplayCaptures([newest, olderDuplicate, distinct]), [newest, distinct]);
});

Deno.test("sentinel event fingerprints are deterministic and duplicate events collapse", async () => {
  const interval = computeSentinelInterval("incident", now);
  const nextScheduledInterval = computeSentinelInterval("incident", now + 5 * 60_000);
  const first = await eventDedupeKey({ repository: "ubiquity/ai.ubq.fi", event: "incident", interval, signalId: "a" });
  const again = await eventDedupeKey({ repository: "ubiquity/ai.ubq.fi", event: "incident", interval, signalId: "a" });
  const delayedWindow = await eventDedupeKey({
    repository: "ubiquity/ai.ubq.fi",
    event: "incident",
    interval: computeSentinelInterval("incident", now + 60_000),
    signalId: "a",
  });
  const different = await eventDedupeKey({
    repository: "ubiquity/ai.ubq.fi",
    event: "incident",
    interval,
    signalId: "b",
  });
  const scheduled = await eventDedupeKey({ repository: "ubiquity/ai.ubq.fi", event: "incident", interval });
  const nextScheduled = await eventDedupeKey({
    repository: "ubiquity/ai.ubq.fi",
    event: "incident",
    interval: nextScheduledInterval,
  });
  assert.equal(first, again);
  assert.equal(first, delayedWindow);
  assert.notEqual(first, different);
  assert.notEqual(scheduled, nextScheduled);
  assert.equal(sentinelEvidenceArtifactName(first), `sentinel-evidence-v1-${first}`);
  assert.deepEqual(deduplicateEvents([{ id: first }, { id: first }, { id: different }], (item) => item.id), [
    { id: first },
    { id: different },
  ]);
});

Deno.test("native review parser blocks P0/P1, backlogs P2/P3, and fails closed on missing output", async () => {
  const parsed = await parseNativeReview(
    `Review findings:\n- [P1] Preserve terminal SSE failure — src/openai.ts:100\n  The stream can close early.\n- [P2] Bound a retry loop — scripts/job.ts:20\n  This can waste one request.`,
    1,
  );
  assert.equal(parsed.parse_status, "findings");
  assert.equal(parsed.findings.length, 2);
  assert.deepEqual(blockingReviewFindings(parsed).map((finding) => finding.severity), ["P1"]);
  assert.equal((await parseNativeReview("No findings.", 1)).parse_status, "no_findings");
  assert.equal((await parseNativeReview("Looks reasonable to me.", 1)).parse_status, "unparseable");
  const missing = await parseNativeReview("", 1);
  assert.equal(missing.parse_status, "unparseable");
  assert.throws(() => blockingReviewFindings(missing), /not parseable/);
});

Deno.test("native review parsing uses the final stdout and normalizes ephemeral checkout paths", async () => {
  const stdout =
    "- [P2] Avoid blocking persistence — /tmp/uos-final/checkout/src/handler.ts:439\n  Return the response first.";
  const stderr = `${stdout}\nCodex progress that must not enter the finding body.`;
  assert.equal(nativeReviewParseInput(stdout, stderr), stdout);
  assert.equal(nativeReviewParseInput("", "Codex progress without a final review."), "");

  const absolute = await parseNativeReview(stdout, 1);
  const relative = await parseNativeReview(
    "- [P2] Avoid blocking persistence — src/handler.ts:439\n  Return the response first.",
    1,
  );
  assert.equal(absolute.findings.length, 1);
  assert.equal(absolute.findings[0]?.location, "src/handler.ts:439");
  assert.equal(absolute.findings[0]?.title, relative.findings[0]?.title);
  assert.equal(absolute.findings[0]?.fingerprint, relative.findings[0]?.fingerprint);
});

Deno.test("rendered native review parser accepts only explicit clean verdicts and rejects malformed output", async () => {
  assert.equal(
    (await parseNativeReview(
      "No actionable defects were found in the changes. Focused Sentinel tests pass.",
      1,
    )).parse_status,
    "no_findings",
  );
  assert.equal(
    (await parseNativeReview("I did not find any actionable defects.", 1)).parse_status,
    "no_findings",
  );
  assert.equal(
    (await parseNativeReview(
      "Review comment:\n\nThe workflow can lose a repair signal under load.",
      1,
    )).parse_status,
    "unparseable",
  );
  assert.equal(
    (await parseNativeReview(
      "Full review comments:\n\n- Preserve the repair signal under load.",
      1,
    )).parse_status,
    "unparseable",
  );
  assert.equal((await parseNativeReview("The patch looks correct.", 1)).parse_status, "unparseable");
  assert.equal((await parseNativeReview("[P1]", 1)).parse_status, "unparseable");
  assert.equal(
    (await parseNativeReview(
      "Review comment:\n\n- [P1] Valid finding — src/review.ts:12\n  This finding is complete.\n- [P2]",
      1,
    )).parse_status,
    "unparseable",
  );
  assert.equal(
    (await parseNativeReview("Reviewer failed to output a response.", 1)).parse_status,
    "unparseable",
  );
  const rendered = await parseNativeReview(
    "Review comment:\n\n- [P2] Preserve the capture snapshot — /tmp/uos/candidate-worktree/src/handler.ts:439-444\n  Concurrent cleanup can zero the body.",
    1,
  );
  assert.equal(rendered.parse_status, "findings");
  assert.equal(rendered.findings[0]?.location, "src/handler.ts:439-444");
  assert.match(rendered.findings[0]?.title ?? "", /src\/handler\.ts:439-444/u);
});

Deno.test("structured native review parser uses protocol fields and rejects Codex prose fallback", async () => {
  const checkout = "/tmp/uos/candidate-worktree";
  const structured = await parseStructuredNativeReview(
    {
      findings: [{
        title: "[P1] Preserve the capture snapshot",
        body: "Concurrent cleanup can zero the body.",
        confidence_score: 0.98,
        priority: 1,
        code_location: {
          absolute_file_path: `${checkout}/src/handler.ts`,
          line_range: { start: 439, end: 444 },
        },
      }, {
        title: "Bound retries",
        body: "The loop needs a fixed maximum.",
        confidence_score: 0.8,
        priority: 2,
        code_location: {
          absolute_file_path: `${checkout}/scripts/job.ts`,
          line_range: { start: 20, end: 20 },
        },
      }],
      overall_correctness: "patch is incorrect",
      overall_explanation: "Two defects remain.",
      overall_confidence_score: 0.96,
    },
    1,
    checkout,
  );
  assert.equal(structured.parse_status, "findings");
  assert.deepEqual(structured.findings.map((finding) => finding.severity), ["P1", "P2"]);
  assert.equal(structured.findings[0]?.location, "src/handler.ts:439-444");
  assert.equal(structured.findings[0]?.title, "Preserve the capture snapshot — src/handler.ts:439-444");
  assert.deepEqual(blockingReviewFindings(structured).map((finding) => finding.severity), ["P1"]);

  const clean = await parseStructuredNativeReview(
    {
      findings: [],
      overall_correctness: "patch is correct",
      overall_explanation: "The patch is correct.",
      overall_confidence_score: 0.9,
    },
    1,
    checkout,
  );
  assert.equal(clean.parse_status, "no_findings");

  for (
    const malformed of [
      {
        findings: [],
        overall_correctness: "",
        overall_explanation: "There is a blocking race in src/handler.ts.",
        overall_confidence_score: 0,
      },
      {
        findings: [],
        overall_correctness: "patch is incorrect",
        overall_explanation: "A defect remains.",
        overall_confidence_score: 0.9,
      },
      {
        findings: [{
          title: "[P2] Contradictory finding",
          body: "A clean verdict cannot include a defect.",
          confidence_score: 0.9,
          priority: 2,
          code_location: {
            absolute_file_path: `${checkout}/src/handler.ts`,
            line_range: { start: 1, end: 2 },
          },
        }],
        overall_correctness: "patch is correct",
        overall_explanation: "The patch is correct.",
        overall_confidence_score: 0.9,
      },
      {
        findings: [{
          title: "[P2] Mismatched priority",
          body: "The title disagrees with the protocol field.",
          confidence_score: 0.9,
          priority: 1,
          code_location: {
            absolute_file_path: `${checkout}/src/handler.ts`,
            line_range: { start: 1, end: 2 },
          },
        }],
        overall_correctness: "patch is incorrect",
        overall_explanation: "A defect remains.",
        overall_confidence_score: 0.9,
      },
      {
        findings: [{
          title: "Relative location",
          body: "The protocol location must be absolute and inside the checkout.",
          confidence_score: 0.9,
          priority: 1,
          code_location: {
            absolute_file_path: "src/handler.ts",
            line_range: { start: 1, end: 2 },
          },
        }],
        overall_correctness: "patch is incorrect",
        overall_explanation: "A defect remains.",
        overall_confidence_score: 0.9,
      },
    ]
  ) {
    assert.equal((await parseStructuredNativeReview(malformed, 1, checkout)).parse_status, "unparseable");
  }
});

const githubIssueBody = (files = ["src/http.ts", "tests/static-assets.test.ts"]): string =>
  `Implement the bounded repository correction.\n\nAcceptance:\n- Preserve the public contract.\n- Add focused coverage.\n\nFiles:\n${
    files.map((path) => `- ${path}`).join("\n")
  }\n`;

const ownerBacklogIssueBody = (files = ["src/paid_fallback_ledger.ts"]): string =>
  `## Context\nThe bounded owner backlog needs a repository repair in ${
    files.map((path) => `\`${path}\``).join(", ")
  }.\n\n## Gap\nThe current implementation does not complete the requested maintenance.\n\n## Proposed\nImplement the owner-authored maintenance proposal within the cited source-file scope.\n\n## References\n- Existing owner backlog.\n`;

const sentinelGitHubIssue = (overrides: Partial<GitHubIssue> = {}): GitHubIssue => ({
  id: 10_113,
  nodeId: "I_kwDOIssue113",
  number: 113,
  state: "open",
  title: "Complete browser HTTP support",
  body: githubIssueBody(),
  htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/113",
  authorLogin: "0x4007",
  authorAssociation: "MEMBER",
  labels: ["Time: <2 Hours", "Priority: 2 (Medium)"],
  assignees: [],
  locked: false,
  comments: 0,
  createdAt: "2026-08-23T19:06:03Z",
  updatedAt: "2026-08-23T19:07:27Z",
  isPullRequest: false,
  ...overrides,
});

const noIssueRelations: GitHubIssueRelations = {
  parentIssueNumber: null,
  subIssueCount: 0,
  blockedByCount: 0,
  blockingCount: 0,
  latestBodyEdit: null,
  latestTitleEdit: null,
};

const inertIssueComments = (count: number): readonly GitHubIssueComment[] =>
  Array.from({ length: count }, (_, index) => ({
    id: 50_000 + index,
    authorLogin: "ubiquity-os[bot]",
    authorType: "Bot",
    body: `> [!WARNING]
> You are not allowed to set labels.

<!-- UbiquityOS - updateLabels - ${
      "a".repeat(64)
    } - @0x4007 - https://console.deno.com/ubiquity-os/daemon-pricing/observability/logs?start=2026-08-26T23%3A32%3A29Z&end=2026-08-26T23%3A34%3A29Z&tz=Etc%2FUTC
{
  "caller": "updateLabels"
}
-->
`,
    createdAt: "2026-08-26T23:33:29Z",
    updatedAt: "2026-08-26T23:33:29Z",
  }));

const githubIssueSource = (
  issues: readonly GitHubIssue[],
  relations: Readonly<Record<number, GitHubIssueRelations>> = {},
  permissions: Readonly<Record<string, GitHubRepositoryPermission>> = {},
): GitHubIssueJobSource => {
  const byNumber = new Map(issues.map((issue) => [issue.number, issue]));
  return {
    listOpenIssues: () => Promise.resolve(issues),
    getIssue: (number) => {
      const issue = byNumber.get(number);
      if (!issue) throw new Error(`Missing issue ${number}`);
      return Promise.resolve(issue);
    },
    listIssueComments: (number) => Promise.resolve(inertIssueComments(byNumber.get(number)?.comments ?? 0)),
    getIssueRelations: (number) => Promise.resolve(relations[number] ?? noIssueRelations),
    getRepositoryPermission: (login) => Promise.resolve(permissions[login] ?? "admin"),
  };
};

Deno.test("broad intake admits ordinary sources and orders numeric priority descending then oldest", async () => {
  const ordinary = sentinelGitHubIssue({
    number: 140,
    id: 10_140,
    nodeId: "I_kwDOIssue140",
    title: "Ordinary prose issue without template",
    body: "Please make the runway projection refill-aware and keep the old number visible for contrast.",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/140",
    labels: [],
    createdAt: "2026-08-28T00:00:00Z",
    updatedAt: "2026-08-28T00:01:00Z",
  });
  const broadJob = await createGitHubIssueJob("ubiquity/ai.ubq.fi", ordinary, noIssueRelations);
  assert.ok(broadJob);
  assert.equal(broadJob!.intake, "backlog");
  assert.equal(broadJob!.queuePriority, null);
  assert.deepEqual(broadJob!.files, []);
  assert.deepEqual(broadJob!.acceptance, []);
  // Independent admission: assignees, locked, unprivileged author, parent/
  // subissue, outgoing blockers, long/missing estimate, arbitrary labels.
  for (
    const issue of [
      sentinelGitHubIssue({ assignees: ["worker"] }),
      sentinelGitHubIssue({ locked: true }),
      sentinelGitHubIssue({ authorLogin: "write-user", labels: ["Time: <3 Days"] }),
      sentinelGitHubIssue({ labels: ["Priority: 2 (Medium)", "Time: <4 Hours"] }),
      sentinelGitHubIssue({
        ...ordinary,
        id: 10_141,
        number: 141,
        htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/141",
        labels: ["Time: <30 Days"],
      }),
    ]
  ) assert.ok(await createGitHubIssueJob("ubiquity/ai.ubq.fi", issue, noIssueRelations));
  const relational = await createGitHubIssueJob("ubiquity/ai.ubq.fi", sentinelGitHubIssue(), {
    ...noIssueRelations,
    parentIssueNumber: 1,
    subIssueCount: 2,
    blockedByCount: 1,
    blockingCount: 1,
  });
  assert.ok(relational);
  // Ordering: numeric priority descending, then created_at ascending, then
  // number; highest recognized duplicate; absent/unrecognized last.
  const low = sentinelGitHubIssue({
    number: 200,
    id: 10_200,
    nodeId: "I_kwDOIssue200",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/200",
    labels: ["Priority: 2 (Medium)"],
    createdAt: "2026-08-20T00:00:00Z",
    updatedAt: "2026-08-20T00:01:00Z",
  });
  const high = sentinelGitHubIssue({
    number: 201,
    id: 10_201,
    nodeId: "I_kwDOIssue201",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/201",
    labels: ["Priority: 3 (High)"],
    createdAt: "2026-08-21T00:00:00Z",
    updatedAt: "2026-08-21T00:01:00Z",
  });
  const duplicate = sentinelGitHubIssue({
    number: 202,
    id: 10_202,
    nodeId: "I_kwDOIssue202",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/202",
    labels: ["Priority: 2 (Medium)", "Priority: 4 (Critical)"],
    createdAt: "2026-08-19T00:00:00Z",
    updatedAt: "2026-08-19T00:01:00Z",
  });
  const unrecognized = sentinelGitHubIssue({
    number: 203,
    id: 10_203,
    nodeId: "I_kwDOIssue203",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/203",
    labels: ["Priority: Emergency"],
    createdAt: "2026-08-18T00:00:00Z",
    updatedAt: "2026-08-18T00:01:00Z",
  });
  const selected = await selectNextGitHubIssueJobSelection(
    githubIssueSource([low, high, duplicate, unrecognized]),
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
    new Date("2026-08-28T06:00:00Z"),
  );
  assert.equal(selected?.job?.number, 202);
  assert.equal(selected?.job?.queuePriority, 4);
  assert.equal(selected?.job?.queuePriorityAmbiguous, true);
  assert.equal(selected?.job?.priority, "P2");
  // A terminal recovery decision on the first-priority source skips it and
  // the queue continues in strict order: highest recognized next, then age.
  const duplicateJob = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    duplicate,
    noIssueRelations,
    inertIssueComments(duplicate.comments),
  );
  assert.ok(duplicateJob);
  const second = await selectNextGitHubIssueJobSelection(
    githubIssueSource([unrecognized, low, high, duplicate]),
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(parseSentinelRecoveryLedger({
      ...emptySentinelRecoveryLedger(),
      records: [recoveryRecordFixture({
        identity: {
          repository: "ubiquity/ai.ubq.fi",
          source_kind: "github_issue",
          source_id: String(duplicate.id),
          source_revision: duplicateJob!.fingerprint,
          candidate_generation: 1,
        },
        phase: "rejected",
        disposition: "rejected",
        updated_at: "2026-08-28T00:00:00.000Z",
        reason: "rejected/no_candidate_diff",
        next_action: null,
      })],
    })),
    new Date("2026-08-28T06:00:00Z"),
  );
  assert.equal(second?.job?.number, high.number);
  assert.equal(second?.job?.queuePriority, 3);
  assert.equal(second?.queue.entries.length, 2);
  assert.equal(second?.queue.entries[0]?.reason?.startsWith("recovery_unavailable"), true);
});

Deno.test("legacy owner-backlog projection is shape-derived and permission-free", async () => {
  const issue = sentinelGitHubIssue({
    number: 136,
    id: 10_136,
    nodeId: "I_kwDOIssue136",
    title: "Automate bounded owner backlog maintenance",
    body: ownerBacklogIssueBody(["src/paid_fallback_ledger.ts", "src/quota_projection.ts"]),
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/136",
    labels: [],
  });
  const selected = await createGitHubIssueJob("ubiquity/ai.ubq.fi", issue, noIssueRelations);
  assert.ok(selected);
  assert.equal(selected!.intake, "owner_backlog");
  assert.equal(selected!.priority, "P3");
  assert.equal(selected!.priorityLabel, "Priority: 2 (Medium)");
  assert.equal(selected!.timeLabel, "Time: <2 Hours");
  assert.deepEqual(selected!.files, ["src/paid_fallback_ledger.ts", "src/quota_projection.ts"]);
  const repeatedCitation = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    {
      ...issue,
      body: ownerBacklogIssueBody(["src/paid_fallback_ledger.ts:10", "src/paid_fallback_ledger.ts:20"]),
    },
    noIssueRelations,
  );
  assert.deepEqual(repeatedCitation?.files, ["src/paid_fallback_ledger.ts"]);
  // A malformed legacy source is admitted under broad backlog intake, never
  // silently dropped: shape-derived projection is not an admission veto.
  const emptyScope = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    { ...issue, body: ownerBacklogIssueBody([]) },
    noIssueRelations,
  );
  assert.ok(emptyScope);
  assert.equal(emptyScope!.intake, "backlog");
  assert.deepEqual(emptyScope!.files, []);
});

Deno.test("GitHub issue selection is deterministic, snapshot-bound, and becomes triage work", async () => {
  const p3 = sentinelGitHubIssue({
    id: 10_120,
    nodeId: "I_kwDOIssue120",
    number: 120,
    title: "Older High-priority issue",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/120",
    labels: ["Priority: 3 (High)", "Time: <1 Hour"],
    createdAt: "2026-08-20T00:00:00Z",
    updatedAt: "2026-08-20T00:01:00Z",
  });
  const p2 = sentinelGitHubIssue();
  const source = githubIssueSource([p3, p2]);
  const emptyLedger = renderGitHubIssueJobLedger([]);
  const selected = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    emptyLedger,
    recoverySelectionContext(),
  );
  assert.equal(selected?.number, 120, "High GitHub issues sort before Medium GitHub issues");
  assert.equal(selected?.priority, "P2");
  assert.ok(selected);

  const interval = computeSentinelInterval("hourly", now);
  const work = selectSentinelWork("hourly", 0, interval, renderReviewBacklog([]), recoverySelectionContext(), selected);
  assert.equal(work.source, "github_issue");
  assert.equal(work.reason, "hourly_github_issue");
  assert.equal(work.issueJob?.fingerprint, selected.fingerprint);
  assert.ok(work.triage && isTriageReport(work.triage));
  assert.equal(work.triage?.findings[0]?.severity, "P2");
  assert.equal(work.triage?.findings[0]?.evidence[0]?.source, "github_issue");
  assert.deepEqual(githubIssueJobTriageReport(selected, interval), work.triage);
  const medium = await getCurrentGitHubIssueJob(source, "ubiquity/ai.ubq.fi", p2.number);
  assert.equal(medium?.priority, "P3");

  const current = await getCurrentGitHubIssueJob(source, "ubiquity/ai.ubq.fi", selected.number);
  assert.equal(githubIssueJobsMatch(selected, current), true);
  const selectedHint = parseGitHubIssueJobHint(renderGitHubIssueJobHint(selected));
  const emptyHint = parseGitHubIssueJobHint(renderGitHubIssueJobHint(null));
  assert.equal(githubIssueJobMatchesHint(selectedHint, current), true);
  assert.equal(githubIssueJobMatchesHint(emptyHint, null), true);
  assert.equal(githubIssueJobMatchesHint(emptyHint, current), false);
  assert.equal(githubIssueJobMatchesHint(selectedHint, null), false);
  const changed = await getCurrentGitHubIssueJob(
    githubIssueSource([{ ...p3, updatedAt: "2026-08-23T20:00:00Z" }]),
    "ubiquity/ai.ubq.fi",
    selected.number,
  );
  assert.equal(githubIssueJobsMatch(selected, changed), false);
  assert.equal(githubIssueJobMatchesHint(selectedHint, changed), false);
});

Deno.test("comment captures are material: count changes and human discussion are captured, never vetoed", async () => {
  // A comment-count change between listing and detail inspection is captured
  // as the current source (never an aborted queue); material digest binds it.
  const listed = sentinelGitHubIssue({ comments: 3 });
  const source: GitHubIssueJobSource = {
    listOpenIssues: () => Promise.resolve([listed]),
    getIssue: () => Promise.resolve({ ...listed, comments: 4 }),
    listIssueComments: () => Promise.resolve(inertIssueComments(4)),
    getIssueRelations: () => Promise.resolve(noIssueRelations),
    getRepositoryPermission: () => Promise.resolve("admin"),
  };
  const raced = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
  );
  assert.equal(raced?.comments, 4);
  assert.equal(raced?.materialDigest, null);
  // Ordinary human discussion is untrusted task data, not an admission veto:
  // the issue is admitted and its material digest binds the actual comment.
  const humanIssue = sentinelGitHubIssue({ comments: 1 });
  const humanSource: GitHubIssueJobSource = {
    ...githubIssueSource([humanIssue]),
    listIssueComments: () =>
      Promise.resolve([{
        id: 70_001,
        authorLogin: "human-reviewer",
        authorType: "User",
        body: "I am working on this issue.",
        createdAt: "2026-08-26T23:33:29Z",
        updatedAt: "2026-08-26T23:33:29Z",
      }]),
  };
  const human = await selectNextGitHubIssueJob(
    humanSource,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
  );
  assert.equal(human?.number, humanIssue.number);
  assert.equal(human?.materialDigest !== null, true);
  assert.deepEqual(human?.capturedComments.map((comment) => comment.id), [70_001]);
  // Inert UbiquityOS notices are the only context normalized away (no digest).
  const inert = await selectNextGitHubIssueJob(
    githubIssueSource([sentinelGitHubIssue({ comments: 1 })]),
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
  );
  assert.equal(inert?.materialDigest, null);
  // A same-count ordinary-comment edit changes the source identity: the old
  // snapshot is never normalized away.
  const edited = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    sentinelGitHubIssue({ comments: 1 }),
    noIssueRelations,
    [{
      id: 70_001,
      authorLogin: "human-reviewer",
      authorType: "User",
      body: "I am still working on this issue now.",
      createdAt: "2026-08-26T23:33:29Z",
      updatedAt: "2026-08-26T23:33:29Z",
    }],
  );
  assert.notEqual(edited?.fingerprint, human?.fingerprint);
});

Deno.test("GitHub issue authority binds and rechecks the author and latest content editors", async () => {
  const issue = sentinelGitHubIssue({ authorAssociation: "MEMBER" });
  const editedRelations: GitHubIssueRelations = {
    ...noIssueRelations,
    latestBodyEdit: { editorLogin: "body-writer", editedAt: "2026-08-23T19:07:00Z" },
    latestTitleEdit: {
      editorLogin: "title-writer",
      editedAt: "2026-08-23T19:07:10Z",
      title: issue.title,
    },
  };
  const writable = githubIssueSource([issue], { [issue.number]: editedRelations }, {
    "0x4007": "admin",
    "body-writer": "write",
    "title-writer": "admin",
  });
  const selected = await selectNextGitHubIssueJob(
    writable,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
  );
  assert.ok(selected);
  assert.equal(selected.authorLogin, "0x4007");
  assert.deepEqual(selected.relations, editedRelations);

  // Author/editor repository permissions are no longer an admission rule (the
  // owner instruction authorizes repository-wide processing); editor identity
  // still binds the immutable source fingerprint, so a changed editor is a
  // changed source, never a normalization.
  const untrustedBodyEditor = githubIssueSource([issue], { [issue.number]: editedRelations }, {
    "0x4007": "read",
    "body-writer": "read",
    "title-writer": "read",
  });
  const untrustedBodyJob = await getCurrentGitHubIssueJob(untrustedBodyEditor, "ubiquity/ai.ubq.fi", issue.number);
  assert.ok(untrustedBodyJob);
  assert.equal(untrustedBodyJob!.relations.latestBodyEdit?.editorLogin, "body-writer");
  // Editor identity binds the immutable source: a job without the editor is a
  // different source, never an admission rejection.
  const withoutEditors = await getCurrentGitHubIssueJob(githubIssueSource([issue]), "ubiquity/ai.ubq.fi", issue.number);
  assert.ok(withoutEditors);
  assert.equal(githubIssueJobsMatch(withoutEditors!, selected), false);
  assert.equal(githubIssueJobsMatch(untrustedBodyJob!, selected), true);

  const wrongCurrentTitlePromise = getCurrentGitHubIssueJob(
    githubIssueSource([issue], {
      [issue.number]: {
        ...editedRelations,
        latestTitleEdit: { ...editedRelations.latestTitleEdit!, title: "Stale title" },
      },
    }),
    "ubiquity/ai.ubq.fi",
    issue.number,
  );
  await assert.rejects(wrongCurrentTitlePromise, /inconsistent|source is inconsistent/i);

  const renamed = await getCurrentGitHubIssueJob(
    githubIssueSource([{ ...issue, authorLogin: "different-writer" }]),
    "ubiquity/ai.ubq.fi",
    issue.number,
  );
  assert.equal(githubIssueJobsMatch(selected, renamed), false);

  const differentEditor = await getCurrentGitHubIssueJob(
    githubIssueSource([issue], {
      [issue.number]: {
        ...editedRelations,
        latestBodyEdit: { editorLogin: "other-writer", editedAt: "2026-08-23T19:07:00Z" },
      },
    }),
    "ubiquity/ai.ubq.fi",
    issue.number,
  );
  assert.equal(githubIssueJobsMatch(selected, differentEditor), false);
});

Deno.test("GitHub issue ledger prevents unchanged retries and permits an edited snapshot", async () => {
  const issue = sentinelGitHubIssue({ comments: 3 });
  const source = githubIssueSource([issue]);
  const selected = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    recoverySelectionContext(),
  );
  assert.ok(selected);
  const ledger = applyGitHubIssueJobDisposition(
    renderGitHubIssueJobLedger([]),
    selected,
    "a".repeat(40),
    new Date("2026-08-23T20:00:00Z"),
    "manual_required",
  );
  assert.equal(parseGitHubIssueJobLedger(ledger)[0]?.number, 113);
  assert.equal(parseGitHubIssueJobLedger(ledger)[0]?.comments, 3);
  assert.equal(await selectNextGitHubIssueJob(source, "ubiquity/ai.ubq.fi", ledger, recoverySelectionContext()), null);
  assert.throws(
    () =>
      applyGitHubIssueJobDisposition(
        ledger,
        selected,
        "a".repeat(40),
        new Date("2026-08-23T21:00:00Z"),
        "resolved",
      ),
    /already has a terminal/,
  );
  const commented = { ...issue, comments: 4, updatedAt: "2026-08-23T20:30:00Z" };
  assert.equal(
    await selectNextGitHubIssueJob(
      githubIssueSource([commented]),
      "ubiquity/ai.ubq.fi",
      ledger,
      recoverySelectionContext(),
    ),
    null,
  );
  const edited = sentinelGitHubIssue({
    body: githubIssueBody(["src/http.ts"]),
    comments: 4,
    updatedAt: "2026-08-23T21:00:00Z",
  });
  assert.equal(
    (await selectNextGitHubIssueJob(
      githubIssueSource([edited]),
      "ubiquity/ai.ubq.fi",
      ledger,
      recoverySelectionContext(),
    ))?.number,
    113,
  );
});

Deno.test("the checked-in GitHub issue ledger is canonical", () => {
  assert.deepEqual(
    renderGitHubIssueJobLedger(parseGitHubIssueJobLedger(issueJobLedger)),
    issueJobLedger,
  );
});

Deno.test("GitHub issue ledger round-trips literal entity text in issue titles", async () => {
  const title = "Keep literal &#x7c;, &#124;, &lt;, &gt;, and &amp; text";
  const job = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    sentinelGitHubIssue({ title }),
    noIssueRelations,
  );
  assert.ok(job);
  const ledger = applyGitHubIssueJobDisposition(
    renderGitHubIssueJobLedger([]),
    job,
    "a".repeat(40),
    new Date("2026-08-23T20:00:00Z"),
    "manual_required",
  );
  const parsed = parseGitHubIssueJobLedger(ledger);
  assert.equal(parsed[0]?.title, title);
  assert.equal(renderGitHubIssueJobLedger(parsed), ledger);
});

Deno.test("GitHub issue implementation stays inside declared Files and blocks in-scope review findings", async () => {
  const job = await createGitHubIssueJob("ubiquity/ai.ubq.fi", sentinelGitHubIssue(), noIssueRelations);
  assert.ok(job);
  assert.deepEqual(
    evaluateGitHubIssueJobImplementation(job, "implemented", ["src/http.ts"], ["src/http.ts"]),
    { disposition: "resolved", continueToRuntimeValidation: true },
  );
  assert.deepEqual(evaluateGitHubIssueJobImplementation(job, "already_fixed", [], []), {
    disposition: "manual_required",
    continueToRuntimeValidation: false,
  });
  assert.throws(
    () => evaluateGitHubIssueJobImplementation(job, "implemented", ["README.md"], ["README.md"]),
    /outside the frozen planned scope/,
  );
  assert.throws(
    () => requireResolvedGitHubIssueJobImplementation(job, "already_fixed", [], []),
    /does not retain/,
  );
  const review = {
    schema_version: 1 as const,
    round: 1,
    parse_status: "findings" as const,
    findings: [
      {
        fingerprint: "1".repeat(64),
        severity: "P2" as const,
        title: "In scope",
        body: "",
        location: "src/http.ts:1",
      },
      {
        fingerprint: "2".repeat(64),
        severity: "P2" as const,
        title: "Out of scope",
        body: "",
        location: "README.md:1",
      },
      {
        fingerprint: "3".repeat(64),
        severity: "P1" as const,
        title: "Blocking",
        body: "",
        location: "README.md:2",
      },
    ],
  };
  assert.deepEqual(blockingIssueReviewFindings(review, job.files).map((finding) => finding.title), [
    "In scope",
    "Blocking",
  ]);
  assert.deepEqual(issueReviewBacklogFindings(review, job.files).map((finding) => finding.title), ["Out of scope"]);
  assert.throws(
    () => blockingIssueReviewFindings({ ...review, parse_status: "unparseable", findings: [] }, job.files),
    /not parseable/,
  );
});

// Whole-issue planned scope: exact files match exactly, explicit directories
// cover descendants at a component boundary, protected descendants denied.
const broad = await createGitHubIssueJob(
  "ubiquity/ai.ubq.fi",
  sentinelGitHubIssue({ body: "Ordinary prose, no template or paths." }),
  noIssueRelations,
);
assert.deepEqual(broad?.files, []);
assert.deepEqual(
  evaluateGitHubIssueJobImplementation(broad!, "implemented", ["src/http.ts", "tests/http.test.ts"], [
    "src/http.ts",
    "tests/http.test.ts",
  ], ["src/http.ts", "tests/"]),
  { disposition: "resolved", continueToRuntimeValidation: true },
);
assert.throws(
  () =>
    evaluateGitHubIssueJobImplementation(
      broad!,
      "implemented",
      ["src/http-2.ts"],
      ["src/http-2.ts"],
      ["src/http.ts", "tests/"],
    ),
  /outside the frozen planned scope|frozen planned scope/,
);
assert.deepEqual(
  evaluateGitHubIssueJobImplementation(
    broad!,
    "implemented",
    ["tests/new-dir/http.test.ts"],
    ["tests/new-dir/http.test.ts"],
    ["src/", "tests/"],
  ),
  { disposition: "resolved", continueToRuntimeValidation: true },
);
assert.throws(
  () =>
    evaluateGitHubIssueJobImplementation(
      broad!,
      "implemented",
      ["tests/fixtures-x.test.ts"],
      ["tests/fixtures-x.test.ts"],
      ["src/", "tests/fixtures/"],
    ),
  /outside the frozen planned scope|frozen planned scope/,
);
assert.throws(
  () =>
    evaluateGitHubIssueJobImplementation(
      broad!,
      "implemented",
      ["scripts/sentinel/main.ts"],
      ["scripts/sentinel/main.ts"],
      ["src/", "tests/"],
    ),
  /protected path/,
);

Deno.test("selected issue controller route applies the frozen plan scope when job.files are empty", () => {
  // Ordinary issue with no source hint files: the controller route alone can
  // carry the frozen plan scope into the real disposition check.
  const job = broad!;
  assert.deepEqual(job.files, []);
  const workSelection: SentinelWorkSelection = {
    source: "github_issue",
    reason: "hourly_github_issue",
    backlogEntry: null,
    issueJob: job,
    triage: {
      schema_version: 1,
      interval: computeSentinelInterval("hourly", now),
      findings: [{
        id: issueJobFindingId(job),
        fingerprint: job.fingerprint,
        severity: "P3" as const,
        title: job.title,
        affected_surface: "src/paid_fallback_ledger.ts",
        allowed_paths: ["src/", "tests/"],
        shared_paths: [],
        depends_on: [],
        evidence: [{ source: "github_issue", reference: job.htmlUrl, detail: job.body }],
        proposed_correction: "Implement the bounded repair.",
        validation_requirements: ["Run deno fmt and affected tests"],
        actionable: true,
      }],
      no_findings_reason: null,
    },
  };
  // Matching changes inside the frozen scope resolve despite the empty
  // source hint files, exactly as the controller closures evaluate them.
  assert.deepEqual(
    evaluateSelectedIssueImplementation(workSelection, "implemented", [
      "src/paid_fallback_ledger.ts",
      "tests/paid_fallback_ledger.test.ts",
    ], [
      "src/paid_fallback_ledger.ts",
      "tests/paid_fallback_ledger.test.ts",
    ]),
    { disposition: "resolved", continueToRuntimeValidation: true },
  );
  assert.doesNotThrow(() =>
    requireResolvedSelectedIssueImplementation(workSelection, "implemented", [
      "src/paid_fallback_ledger.ts",
    ], ["src/paid_fallback_ledger.ts"])
  );
  // Changes outside the frozen scope are rejected by the same route.
  assert.throws(
    () => evaluateSelectedIssueImplementation(workSelection, "implemented", ["README.md"], ["README.md"]),
    /outside the frozen planned scope/,
  );
  assert.throws(
    () =>
      requireResolvedSelectedIssueImplementation(
        workSelection,
        "implemented",
        ["docs/sentinel.md"],
        ["docs/sentinel.md"],
      ),
    /outside the frozen planned scope/,
  );
  // Protected paths are rejected even when they sit inside the frozen scope.
  assert.throws(
    () =>
      evaluateSelectedIssueImplementation(
        workSelection,
        "implemented",
        ["scripts/sentinel/main.ts"],
        ["scripts/sentinel/main.ts"],
      ),
    /protected path/,
  );
});

Deno.test("changed_files mismatch retains the Git checkpoint as a durable validation failure", async () => {
  const job = await createGitHubIssueJob("ubiquity/ai.ubq.fi", sentinelGitHubIssue(), noIssueRelations);
  assert.ok(job);
  assert.throws(
    () => evaluateGitHubIssueJobImplementation(job, "implemented", ["src/http.ts"], ["src/other.ts"]),
    (error) => {
      assert.ok(error instanceof SentinelChangedFilesMismatchError);
      assert.deepEqual(error.actualChangedFiles, ["src/http.ts"]);
      assert.deepEqual(error.reportedChangedFiles, ["src/other.ts"]);
      assert.equal(error.source, "github_issue");
      return true;
    },
  );

  const finalized = finalizeSentinelCandidate({
    repository: "ubiquity/ai.ubq.fi",
    sourceKind: "github_issue",
    sourceId: String(job.number),
    sourceRevision: job.fingerprint,
    candidateGeneration: 1,
    runId: "33197180235",
    attempt: 1,
    leaseToken: "33197180235-1",
    baseSha: "b".repeat(40),
    candidateBranch: "sentinel/candidate-33197180235-1",
    candidateSha: "c".repeat(40),
    treeSha: "d".repeat(40),
    changedFiles: ["src/http.ts"],
    reportedChangedFiles: ["src/other.ts"],
    failureFingerprint: "e".repeat(64),
    createdAt: "2026-08-28T18:00:00.000Z",
    updatedAt: "2026-08-28T18:01:00.000Z",
  });
  assert.equal(finalized.reportMatches, false);
  assert.deepEqual(finalized.untrackedChangedFiles, []);
  assert.equal(finalized.record.phase, "validation_failed");
  assert.equal(finalized.record.disposition, "active");
  assert.equal(finalized.record.reason, "report_diff_mismatch");
  assert.equal(finalized.record.candidate_branch, "sentinel/candidate-33197180235-1");
  assert.equal(finalized.record.candidate_sha, "c".repeat(40));
  assert.deepEqual(finalized.record.changed_files, ["src/http.ts"]);
  assert.equal(finalized.record.tree_sha, "d".repeat(40));
  assert.deepEqual(finalized.record, finalized.recoveryRecord);
  assert.deepEqual(
    createSentinelCandidateRecoveryRecord({
      repository: "ubiquity/ai.ubq.fi",
      sourceKind: "github_issue",
      sourceId: String(job.number),
      sourceRevision: job.fingerprint,
      candidateGeneration: 1,
      runId: "33197180235",
      attempt: 1,
      leaseToken: "33197180235-1",
      baseSha: "b".repeat(40),
      candidateBranch: "sentinel/candidate-33197180235-1",
      candidateSha: "c".repeat(40),
      treeSha: "d".repeat(40),
      changedFiles: ["src/http.ts"],
      reportedChangedFiles: ["src/other.ts"],
      failureFingerprint: "e".repeat(64),
      createdAt: "2026-08-28T18:00:00.000Z",
      updatedAt: "2026-08-28T18:01:00.000Z",
    }),
    finalized.record,
  );
  assert.throws(
    () =>
      finalizeSentinelCandidate({
        repository: "ubiquity/ai.ubq.fi",
        sourceKind: "github_issue",
        sourceId: String(job.number),
        sourceRevision: job.fingerprint,
        candidateGeneration: 1,
        runId: "33197180235",
        attempt: 1,
        leaseToken: "33197180235-1",
        baseSha: "b".repeat(40),
        candidateBranch: "sentinel/candidate-33197180235-1",
        candidateSha: "c".repeat(40),
        changedFiles: ["src/http.ts"],
        untrackedChangedFiles: ["tmp/untracked.ts"],
      }),
    /untracked changed files/,
  );
});

Deno.test("Sentinel retry fingerprints ignore run volatility but bind source and generation", async () => {
  const identity = {
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue" as const,
    source_id: "136",
    source_revision: "issue-revision-a",
    candidate_generation: 2,
  };
  const first = await stableSentinelFailureFingerprint({
    identity,
    failure_class: "transient_transport",
    code: "invocation_timeout",
    phase: "implementation",
    signature: "stream idle timeout",
  });
  const second = await stableSentinelFailureFingerprint({
    identity,
    failure_class: "transient_transport",
    code: "invocation_timeout",
    phase: "implementation",
    signature: "  stream   idle timeout\r\n",
  });
  assert.equal(first, second);
  assert.match(first, /^[0-9a-f]{64}$/u);
  assert.notEqual(
    first,
    await stableSentinelFailureFingerprint({
      identity: { ...identity, source_revision: "issue-revision-b" },
      failure_class: "transient_transport",
      code: "invocation_timeout",
      phase: "implementation",
      signature: "stream idle timeout",
    }),
  );
  assert.notEqual(
    first,
    await stableSentinelFailureFingerprint({
      identity: { ...identity, candidate_generation: 3 },
      failure_class: "transient_transport",
      code: "invocation_timeout",
      phase: "implementation",
      signature: "stream idle timeout",
    }),
  );
});

Deno.test("Sentinel retry classification separates capacity, transport, and invalid reports", () => {
  assert.deepEqual(classifySentinelFailure(new CodexInvocationError("accounts_unavailable")), {
    failure_class: "capacity_quota",
    code: "accounts_unavailable",
    phase: null,
    signature: "accounts_unavailable",
    retryable: true,
    validation_repairable: false,
  });
  assert.equal(
    classifySentinelFailure(new CodexInvocationError("invocation_timeout")).failure_class,
    "transient_transport",
  );
  assert.equal(
    classifySentinelFailure(new CodexInvocationError("last_message_missing")).failure_class,
    "invalid_implementation_report",
  );
  assert.equal(classifySentinelFailure(new Error("unknown failure")).retryable, false);
});

Deno.test("Sentinel retry backoff is exponential, jittered, and bounded", () => {
  assert.equal(
    computeSentinelRetryBackoffMs({ attempt: 1, base_delay_ms: 100, max_delay_ms: 500, jitter_ratio: 0 }),
    100,
  );
  assert.equal(
    computeSentinelRetryBackoffMs({ attempt: 2, base_delay_ms: 100, max_delay_ms: 500, jitter_ratio: 0 }),
    200,
  );
  assert.equal(
    computeSentinelRetryBackoffMs({ attempt: 20, base_delay_ms: 100, max_delay_ms: 500, jitter_ratio: 0 }),
    500,
  );
  assert.equal(
    computeSentinelRetryBackoffMs({
      attempt: 1,
      base_delay_ms: 100,
      max_delay_ms: 500,
      jitter_ratio: 0.2,
      random: () => 0,
    }),
    80,
  );
  assert.ok(
    computeSentinelRetryBackoffMs({ attempt: 20, max_delay_ms: SENTINEL_RETRY_MAX_DELAY_MS, random: () => 0.99 }) <=
      SENTINEL_RETRY_MAX_DELAY_MS,
  );
});

Deno.test("Sentinel retry circuit breaker and validation repair are source-revision aware", async () => {
  const identity = {
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue" as const,
    source_id: "136",
    source_revision: "issue-revision-a",
    candidate_generation: 1,
  };
  const fingerprint = "a".repeat(64);
  const history = [1, 2].map((attempt) =>
    createSentinelRetryAttempt({
      identity,
      attempt,
      failure_class: "transient_transport",
      failure_fingerprint: fingerprint,
      observed_at: `2026-08-28T18:0${attempt}:00.000Z`,
    })
  );
  const circuit = await evaluateSentinelRetryPolicy({
    identity,
    history,
    failure: { failure_class: "transient_transport", failure_fingerprint: fingerprint },
    now: "2026-08-28T18:03:00.000Z",
    random: () => 0.5,
  });
  assert.equal(circuit.decision.disposition, "manual_required");
  assert.equal(circuit.decision.circuit_open, true);
  assert.equal(circuit.decision.identical_failure_count, SENTINEL_RETRY_CIRCUIT_THRESHOLD);

  const changed = await evaluateSentinelRetryPolicy({
    identity,
    history: [
      ...history,
      createSentinelRetryAttempt({
        identity,
        attempt: 3,
        failure_class: "transient_transport",
        failure_fingerprint: fingerprint,
        observed_at: "2026-08-28T18:03:00.000Z",
      }),
    ],
    failure: {
      failure_class: "transient_transport",
      failure_fingerprint: fingerprint,
      source_revision: "issue-revision-b",
    },
    now: "2026-08-28T18:04:00.000Z",
    random: () => 0.5,
  });
  assert.equal(changed.decision.disposition, "fresh_generation");
  assert.equal(changed.decision.circuit_open, false);
  assert.equal(changed.decision.candidate_generation, 2);

  const validation = await evaluateSentinelRetryPolicy({
    identity,
    history: [],
    failure: { failure_class: "validation_failure", failure_fingerprint: "b".repeat(64) },
    now: "2026-08-28T18:05:00.000Z",
    random: () => 0.5,
  });
  assert.equal(validation.decision.disposition, "retry_wait");
  assert.equal(validation.decision.validation_repair_allowed, true);
  const repeatedValidation = await evaluateSentinelRetryPolicy({
    identity,
    history: validation.history,
    failure: { failure_class: "validation_failure", failure_fingerprint: "c".repeat(64) },
    now: "2026-08-28T18:06:00.000Z",
    random: () => 0.5,
  });
  assert.equal(repeatedValidation.decision.disposition, "manual_required");
  assert.equal(repeatedValidation.decision.circuit_open, false);
  assert.match(repeatedValidation.decision.reason, /one validation repair/u);
});

Deno.test("Sentinel retry preserves the durable checkpoint and monotonic state version", async () => {
  const checkpoint = createSentinelCandidateRecoveryRecord({
    repository: "ubiquity/ai.ubq.fi",
    sourceKind: "github_issue",
    sourceId: "136",
    sourceRevision: "issue-revision-a",
    candidateGeneration: 4,
    runId: "33197180235",
    attempt: 7,
    leaseToken: "33197180235-7",
    baseSha: "b".repeat(40),
    candidateBranch: "sentinel/candidate-github_issue-136-issue-revision-a-g4",
    candidateSha: "c".repeat(40),
    treeSha: "d".repeat(40),
    changedFiles: ["src/http.ts"],
    createdAt: "2026-08-28T18:00:00.000Z",
    updatedAt: "2026-08-28T18:01:00.000Z",
  });
  const applied = await applySentinelRetryPolicyToRecovery({
    record: { ...checkpoint, state_version: 9 },
    history: [],
    failure: { failure_class: "validation_failure", failure_fingerprint: "e".repeat(64) },
    now: "2026-08-28T18:02:00.000Z",
    random: () => 0.5,
  });
  assert.equal(applied.after.phase, "retry_wait");
  assert.equal(applied.after.state_version, 10);
  assert.equal(applied.after.identity.candidate_generation, 4);
  assert.equal(applied.after.candidate_branch, checkpoint.candidate_branch);
  assert.equal(applied.after.candidate_sha, checkpoint.candidate_sha);
  assert.equal(applied.after.base_sha, checkpoint.base_sha);
  assert.equal(applied.after.failure_fingerprint, "e".repeat(64));

  const fresh = createFreshSentinelRecoveryRecord({
    record: applied.after,
    source_revision: "issue-revision-b",
    run_id: "33197180236",
    lease_token: "33197180236-1",
    now: "2026-08-28T18:03:00.000Z",
  });
  assert.equal(fresh.phase, "claimed");
  assert.equal(fresh.identity.candidate_generation, 5);
  assert.equal(fresh.identity.source_revision, "issue-revision-b");
  assert.equal(fresh.state_version, 11);
  assert.equal(fresh.candidate_sha, null);
  assert.equal(fresh.predecessor !== null, true);
});

Deno.test("a clean durable checkpoint remains eligible for review with its exact SHA", () => {
  const checkpointSha = "c".repeat(40);
  assert.equal(candidateShaForReview(checkpointSha, ["src/http.ts"]), checkpointSha);
  assert.equal(candidateShaForReview(checkpointSha, []), null);
  assert.throws(
    () => candidateShaForReview("not-a-sha", ["src/http.ts"]),
    /full Git SHA/,
  );
});

Deno.test("review backlog deduplicates fingerprints while retaining first observation and disposition", async () => {
  const report = await parseNativeReview("- [P2] Bound retries — src/retry.ts:9\n  Add a fixed maximum.", 1);
  const finding = report.findings[0]!;
  const firstAt = new Date("2026-08-20T00:00:00.000Z");
  const latestAt = new Date("2026-08-21T00:00:00.000Z");
  const initialOpen = mergeReviewBacklog(renderReviewBacklog([]), [finding], "a".repeat(40), firstAt);
  const initial = renderReviewBacklog(
    parseReviewBacklog(initialOpen).map((entry) => ({ ...entry, disposition: "accepted_risk" as const })),
  );
  const latest = mergeReviewBacklog(initial, [finding], "b".repeat(40), latestAt);
  assert.equal(latest.match(new RegExp(finding.fingerprint, "g"))?.length, 1);
  assert.match(latest, /2026-08-20T00:00:00.000Z/);
  assert.match(latest, /2026-08-21T00:00:00.000Z/);
  assert.match(latest, new RegExp("`" + "b".repeat(40) + "`"));
  assert.match(latest, /accepted_risk/);
});

Deno.test("review backlog output is deterministic and uses canonical Deno Markdown alignment", async () => {
  const report = await parseNativeReview(
    "- [P2] Escape `Markdown` | safely — src/retry.ts:9\n  Preserve readable evidence.",
    1,
  );
  const observedAt = new Date("2026-08-21T00:00:00.000Z");
  const first = mergeReviewBacklog(renderReviewBacklog([]), report.findings, "a".repeat(40), observedAt);
  const again = mergeReviewBacklog(renderReviewBacklog([]), report.findings, "a".repeat(40), observedAt);
  assert.equal(first, again);
  assert.match(
    first,
    /Every\s+severity [^.]*P0, P1, P2, and P3[^.]*never block the reviewed pull request merge\./su,
  );
  assert.ok([...first].every((character) => character.charCodeAt(0) <= 0x7f));
  assert.match(first, /&#96;Markdown&#96; &#124; safely &#x2014;/u);

  const tableLines = first.split("\n").filter((line) => line.startsWith("|"));
  assert.equal(tableLines.length, 3);
  const separators = [...tableLines[0]!.matchAll(/\|/gu)].map((match) => match.index);
  for (const line of tableLines) {
    assert.deepEqual([...line.matchAll(/\|/gu)].map((match) => match.index), separators);
  }
});

const reviewBacklogEntry = (overrides: Partial<ReviewBacklogEntry> = {}): ReviewBacklogEntry => ({
  fingerprint: "a".repeat(64),
  severity: "P2",
  first: "2026-08-20T00:00:00.000Z",
  latest: "2026-08-20T00:00:00.000Z",
  sha: "b".repeat(40),
  location: "src/handler.ts:439",
  finding: "Keep `Markdown` | text — exact.",
  disposition: "open",
  ...overrides,
});

Deno.test("review backlog parsing is strict and round-trips renderer escapes", () => {
  const entry = reviewBacklogEntry();
  const markdown = renderReviewBacklog([entry]);
  assert.deepEqual(parseReviewBacklog(markdown), [entry]);

  const duplicate = renderReviewBacklog([entry, entry]);
  assert.throws(() => parseReviewBacklog(duplicate), /duplicate fingerprint/);
  assert.throws(() => parseReviewBacklog(""), /canonical complete form/);
  assert.throws(() => parseReviewBacklog("# Sentinel Review Backlog\n"), /canonical complete form/);
  assert.throws(
    () => parseReviewBacklog(markdown.replace(/^\| `/mu, "  `")),
    /canonical complete form/,
  );
  assert.throws(
    () => parseReviewBacklog(markdown.replace(/open\s+\|/u, "unknown |")),
    /row is invalid|canonical complete form/,
  );
  assert.throws(
    () => parseReviewBacklog(renderReviewBacklog([{ ...entry, first: "not-a-timestamp" }])),
    /row is invalid/,
  );
  const unknownLocation = renderReviewBacklog([{ ...entry, location: "unknown" }]);
  assert.equal(parseReviewBacklog(unknownLocation)[0]?.location, "unknown");
  assert.equal(selectNextReviewBacklogEntry(unknownLocation, recoverySelectionContext()), null);
  assert.throws(
    () => renderReviewBacklog([{ ...entry, finding: "—".repeat(800) }]),
    /row exceeds its length limit/,
  );
  assert.throws(
    () =>
      renderReviewBacklog(
        Array.from({ length: 256 }, (_, index) => ({
          ...entry,
          fingerprint: index.toString(16).padStart(64, "0"),
          finding: "x".repeat(800),
        })),
      ),
    /byte limit/,
  );
});

Deno.test("backlog implementation decisions reconcile already-fixed work and reject report mismatches", () => {
  assert.deepEqual(
    evaluateReviewBacklogImplementation(
      "implemented",
      ["src/handler.ts"],
      ["src/handler.ts"],
      "src/handler.ts",
    ),
    { disposition: "resolved", continueToRuntimeValidation: true },
  );
  for (const status of ["implemented", "blocked", "not_actionable"] as const) {
    assert.deepEqual(evaluateReviewBacklogImplementation(status, [], []), {
      disposition: "manual_required",
      continueToRuntimeValidation: false,
    });
  }
  assert.deepEqual(evaluateReviewBacklogImplementation("already_fixed", [], []), {
    disposition: "manual_required",
    continueToRuntimeValidation: false,
  });
  assert.deepEqual(evaluateReviewBacklogImplementation("already_fixed", [], [], "src/handler.ts", true), {
    disposition: "resolved",
    continueToRuntimeValidation: false,
  });
  assert.throws(
    () => evaluateReviewBacklogImplementation("implemented", ["src/handler.ts"], []),
    /does not match/,
  );
  assert.throws(
    () => evaluateReviewBacklogImplementation("blocked", ["src/handler.ts"], ["src/handler.ts"]),
    /cannot retain/,
  );
  assert.throws(
    () => evaluateReviewBacklogImplementation("already_fixed", ["src/handler.ts"], ["src/handler.ts"]),
    /cannot retain/,
  );
  assert.throws(
    () => requireResolvedReviewBacklogImplementation("already_fixed", [], [], "src/handler.ts"),
    /does not retain/,
  );
  assert.throws(
    () => evaluateReviewBacklogImplementation("implemented", ["README.md"], ["README.md"], "src/handler.ts"),
    /affected path/,
  );

  const entry = reviewBacklogEntry();
  assert.equal(reviewBacklogEntriesMatch(entry, { ...entry }), true);
  assert.equal(reviewBacklogEntriesMatch(entry, { ...entry, latest: "2026-08-21T00:00:00.000Z" }), false);
  assert.equal(reviewBacklogEntriesMatch(entry, null), false);

  const sha = "a".repeat(40);
  assert.equal(shouldDeferHourlyBacklogWork(undefined, sha), false);
  assert.equal(shouldDeferHourlyBacklogWork(sha, sha), false);
  assert.equal(shouldDeferHourlyBacklogWork("b".repeat(40), sha), true);
  assert.throws(() => shouldDeferHourlyBacklogWork("invalid", sha), /hint SHA is invalid/);
});

Deno.test("quiet backlog work selects one eligible P2 before P3 and skips protected paths", () => {
  const entries: ReviewBacklogEntry[] = [
    reviewBacklogEntry({
      fingerprint: "1".repeat(64),
      first: "2026-08-18T00:00:00.000Z",
      latest: "2026-08-18T00:00:00.000Z",
      location: "scripts/sentinel/main.ts:10",
    }),
    reviewBacklogEntry({
      fingerprint: "2".repeat(64),
      severity: "P3",
      first: "2026-08-17T00:00:00.000Z",
      latest: "2026-08-17T00:00:00.000Z",
      location: "src/handler.ts:20",
    }),
    reviewBacklogEntry({
      fingerprint: "3".repeat(64),
      first: "2026-08-19T00:00:00.000Z",
      latest: "2026-08-19T00:00:00.000Z",
      location: "src/handler.ts:30",
    }),
    reviewBacklogEntry({
      fingerprint: "4".repeat(64),
      first: "2026-08-19T00:00:00.000Z",
      latest: "2026-08-19T00:00:00.000Z",
      location: "src/handler.ts:40",
    }),
    reviewBacklogEntry({
      fingerprint: "0".repeat(64),
      first: "2026-08-16T00:00:00.000Z",
      latest: "2026-08-16T00:00:00.000Z",
      disposition: "resolved",
    }),
  ];
  const markdown = renderReviewBacklog(entries);
  assert.equal(selectNextReviewBacklogEntry(markdown, recoverySelectionContext())?.fingerprint, "3".repeat(64));

  const interval = computeSentinelInterval("hourly", now);
  const selection = selectSentinelWork("hourly", 0, interval, markdown, recoverySelectionContext());
  assert.equal(selection.source, "review_backlog");
  assert.equal(selection.reason, "hourly_review_backlog");
  assert.equal(selection.backlogEntry?.fingerprint, "3".repeat(64));
  assert.ok(selection.triage && isTriageReport(selection.triage));
  assert.equal(selection.triage?.findings[0]?.id, `review-backlog:${"3".repeat(64)}`);
  assert.equal(selectSentinelWork("incident", 0, interval, markdown, recoverySelectionContext()).source, "triage");
  assert.equal(selectSentinelWork("preview", 0, interval, markdown, recoverySelectionContext()).source, null);
  assert.equal(
    selectSentinelWork("hourly", 0, interval, renderReviewBacklog([entries[0]!]), recoverySelectionContext()).source,
    null,
  );
});

Deno.test("backlog implementation dispositions stop retries and recurrence reopens resolved work", async () => {
  const report = await parseNativeReview(
    "- [P2] Avoid blocking persistence — src/handler.ts:439\n  Return the response first.",
    1,
  );
  const finding = report.findings[0]!;
  const observedAt = new Date("2026-08-20T00:00:00.000Z");
  const open = mergeReviewBacklog(renderReviewBacklog([]), [finding], "a".repeat(40), observedAt);
  const resolved = applyReviewBacklogImplementationDisposition(
    open,
    finding.fingerprint,
    "resolved",
  );
  assert.equal(resolved.disposition, "resolved");
  assert.equal(parseReviewBacklog(resolved.markdown)[0]?.disposition, "resolved");
  assert.equal(parseReviewBacklog(resolved.markdown)[0]?.latest, observedAt.toISOString());
  assert.equal(selectNextReviewBacklogEntry(resolved.markdown, recoverySelectionContext()), null);
  assert.throws(
    () =>
      applyReviewBacklogImplementationDisposition(
        resolved.markdown,
        finding.fingerprint,
        "resolved",
      ),
    /selected open/,
  );

  const manual = applyReviewBacklogImplementationDisposition(
    open,
    finding.fingerprint,
    "manual_required",
  );
  assert.equal(manual.disposition, "manual_required");
  assert.equal(selectNextReviewBacklogEntry(manual.markdown, recoverySelectionContext()), null);

  const recurrence = mergeReviewBacklog(
    resolved.markdown,
    [finding],
    "b".repeat(40),
    new Date("2026-08-22T00:00:00.000Z"),
  );
  assert.equal(parseReviewBacklog(recurrence)[0]?.disposition, "open");
  const retainedManual = mergeReviewBacklog(
    manual.markdown,
    [finding],
    "b".repeat(40),
    new Date("2026-08-22T00:00:00.000Z"),
  );
  assert.equal(parseReviewBacklog(retainedManual)[0]?.disposition, "manual_required");
});

Deno.test("targeted backlog recurrence blocks review and empty replay skips another model call", async () => {
  const report = await parseNativeReview(
    "- [P2] Avoid blocking persistence — src/handler.ts:439\n  Return the response first.",
    1,
  );
  const fingerprint = report.findings[0]!.fingerprint;
  assert.equal(blockingReviewFindings(report).length, 0);
  assert.deepEqual(blockingReviewFindings(report, fingerprint), report.findings);
  assert.equal(requiresReplayEvaluation([]), false);
  assert.equal(requiresReplayEvaluation([{ capture_fingerprint: "a".repeat(64) } as ReplayResult]), true);
});

Deno.test("review finding identity does not change when only severity changes", async () => {
  const p2 = await parseNativeReview("- [P2] Bound retries — src/retry.ts:9\n  Add a fixed maximum.", 1);
  const p3 = await parseNativeReview("- [P3] Bound retries — src/retry.ts:9\n  Add a fixed maximum.", 1);
  assert.equal(p2.findings[0]?.fingerprint, p3.findings[0]?.fingerprint);
});

Deno.test("review policy permits exactly three implementation-review rounds", () => {
  assert.equal(SENTINEL_POLICY.maximumReviewRounds, 3);
  assert.equal(canStartReviewRound(0), true);
  assert.equal(canStartReviewRound(2), true);
  assert.equal(canStartReviewRound(3), false);
  assert.equal(canStartReviewRound(4), false);
});

Deno.test("candidate type checking uses the populated cache without remote access", () => {
  const argumentsSet = new Set<string>(CANDIDATE_DENO_CHECK_ARGS);
  assert.deepEqual(CANDIDATE_DENO_CHECK_ARGS.slice(0, 2), ["check", "--frozen"]);
  assert.equal(argumentsSet.has("--no-remote"), false);
  assert.equal(argumentsSet.has("--cached-only"), false);
});

const replayCase = (overrides: Partial<ReplayCase> = {}): ReplayCase => ({
  fingerprint: "f".repeat(64),
  case_group_digest: "g".repeat(64),
  captured_at_ms: now,
  endpoint: "/v1/responses?trace=exact",
  method: "POST",
  content_type: "application/json; charset=utf-8",
  compatibility_headers: { accept: "text/event-stream", "x-codex-client-version": "0.149.0" },
  body: new Uint8Array([32, 123, 34, 120, 34, 58, 49, 125, 10]),
  original: {
    status: 502,
    stream: true,
    framing_valid: true,
    completed: false,
    terminal_type: "response.failed",
    failure_kind: "read_error",
    provider_route: "chatgpt_codex",
    failure_signature: "original-signature",
  },
  ...overrides,
});

Deno.test("replay preserves exact bytes and compatibility headers while replacing authorization and host", async () => {
  const input = replayCase();
  let called = false;
  const result = await replayOneCase({
    replayCase: input,
    previewBaseUrl: "https://preview.example.test",
    previewCredential: "preview-only-token",
    fetchImpl: async (request, init) => {
      called = true;
      assert.equal(String(request), "https://preview.example.test/v1/responses?trace=exact");
      assert.equal(init?.method, "POST");
      assert.deepEqual(new Uint8Array(await new Response(init?.body).arrayBuffer()), input.body);
      const headers = new Headers(init?.headers);
      assert.equal(headers.get("authorization"), "Bearer preview-only-token");
      assert.equal(headers.get("content-type"), input.content_type);
      assert.equal(headers.get("x-codex-client-version"), "0.149.0");
      assert.equal(headers.get("host"), null);
      return new Response(
        `data: {"type":"response.created"}\n\ndata: {"type":"response.completed"}\n\n`,
        { status: 200, headers: { "content-type": "text/event-stream", "x-uos-upstream": "chatgpt_codex" } },
      );
    },
  });
  assert.equal(called, true);
  assert.equal(result.outcome, "improved");
  assert.equal(result.sse_framing_valid, true);
  assert.equal(result.terminal_event, "response.completed");
  assert.equal(result.comparison.provider_matches_original, true);
  assert.equal(result.comparison.framing_matches_original, true);
});

Deno.test("replay permits inference-only endpoints and rejects stateful embedding jobs before transport", async () => {
  assert.equal(isInferenceOnlyReplayEndpoint("/v1/responses"), true);
  assert.equal(isInferenceOnlyReplayEndpoint("/v1/chat/completions?trace=1"), true);
  assert.equal(isInferenceOnlyReplayEndpoint("/uos/embeddings"), true);
  assert.equal(isInferenceOnlyReplayEndpoint("/uos/embedding-jobs"), false);
  assert.equal(isInferenceOnlyReplayEndpoint("/admin/defaults"), false);

  let transported = false;
  const replayCase: ReplayCase = {
    fingerprint: "stateful-job",
    case_group_digest: "a".repeat(64),
    captured_at_ms: now,
    endpoint: "/uos/embedding-jobs",
    method: "POST",
    content_type: "application/json",
    compatibility_headers: {},
    body: new TextEncoder().encode('{"input":["sensitive"]}'),
    original: {
      status: 502,
      stream: false,
      framing_valid: true,
      completed: false,
      terminal_type: "http.error",
      failure_kind: "server_error",
      provider_route: "voyage",
      failure_signature: "original",
    },
  };
  const result = await replayOneCase({
    replayCase,
    previewBaseUrl: "https://preview.example",
    previewCredential: "preview-token",
    fetchImpl: () => {
      transported = true;
      return Promise.resolve(new Response(null, { status: 500 }));
    },
  });
  assert.equal(transported, false);
  assert.equal(result.attempted, false);
  assert.equal(result.unavailable_reason, "case_target_not_inference_only");
  replayCase.body.fill(0);
});

Deno.test("replay never executes returned tool calls and invalid SSE framing is not an improvement", async () => {
  const result = await replayOneCase({
    replayCase: replayCase(),
    previewBaseUrl: "https://preview.example.test",
    previewCredential: "preview-only-token",
    fetchImpl: () =>
      Promise.resolve(
        new Response(
          `data: {"type":"response.output_item.done","item":{"type":"function_call","name":"danger"}}\nnot-sse\n\ndata: {"type":"response.completed"}\n\n`,
          { status: 200, headers: { "content-type": "text/event-stream" } },
        ),
      ),
  });
  assert.equal(result.sse_framing_valid, false);
  assert.equal(result.outcome, "regressed");
  assert.equal(result.comparison.framing_matches_original, false);
});

Deno.test("SSE inspection recognizes failure terminals and retained replay selection matches only case groups", () => {
  const observation = inspectSse(
    `event: response.failed\ndata: {"type":"response.failed","error":{"code":"provider_error"}}\n\n`,
  );
  assert.equal(observation.framingValid, true);
  assert.equal(observation.terminalEvent, "response.failed");
  assert.equal(observation.failureKind, "provider_error");

  const current = replayCase();
  const duplicate = replayCase({ body: new Uint8Array([7]) });
  const matching = replayCase({ fingerprint: "m".repeat(64), captured_at_ms: now - 1 });
  const unrelated = replayCase({
    fingerprint: "u".repeat(64),
    case_group_digest: "z".repeat(64),
    captured_at_ms: now - 2,
  });
  const selected = selectCurrentAndMatchingRegressionCases([current], [duplicate, matching, unrelated]);
  assert.deepEqual(
    selected.map((item) => item.fingerprint),
    [matching.fingerprint, current.fingerprint],
  );
  zeroUnselectedReplayBodies([current, duplicate, matching, unrelated], selected);
  assert.deepEqual([...duplicate.body], [0]);
  assert.equal(current.body.some((byte) => byte !== 0), true);
});

Deno.test("implementation contract requires a disposition for every triage finding", () => {
  const triage: TriageReport = {
    schema_version: 1,
    interval: computeSentinelInterval("hourly", now),
    findings: [{
      id: "one",
      fingerprint: "1".repeat(64),
      severity: "P1",
      title: "one",
      affected_surface: "responses",
      allowed_paths: ["src/provider.ts"],
      shared_paths: [],
      depends_on: [],
      evidence: [{ source: "deno_log", reference: "line 1", detail: "failure" }],
      proposed_correction: "fix it",
      validation_requirements: ["test"],
      actionable: true,
    }],
    no_findings_reason: null,
  };
  const valid: ImplementationReport = {
    schema_version: 1,
    candidate_sha: null,
    dispositions: [{ finding_id: "one", status: "implemented", summary: "fixed", changed_files: [], validation: [] }],
    replay_acceptances: [],
    summary: "done",
  };
  assert.doesNotThrow(() => assertCompleteFindingDispositions(triage, valid));
  assert.doesNotThrow(() => assertActionableFindingsResolved(triage, valid));
  assert.throws(
    () => assertCompleteFindingDispositions(triage, { ...valid, dispositions: [] }),
    /every triage finding/,
  );
  assert.throws(
    () =>
      assertCompleteFindingDispositions(triage, {
        ...valid,
        dispositions: [valid.dispositions[0]!, valid.dispositions[0]!],
      }),
    /every triage finding/,
  );
  const blocked = {
    ...valid,
    dispositions: [{ ...valid.dispositions[0]!, status: "blocked" as const }],
  };
  assert.doesNotThrow(() => assertCompleteFindingDispositions(triage, blocked));
  assert.throws(() => assertActionableFindingsResolved(triage, blocked), /remain unresolved/);
});

Deno.test("triage requires a concrete reason only when it has no findings", () => {
  const interval = computeSentinelInterval("hourly", now);
  assert.equal(isTriageReport({ schema_version: 1, interval, findings: [], no_findings_reason: "" }), false);
  assert.equal(
    isTriageReport({ schema_version: 1, interval, findings: [], no_findings_reason: "No failures in the interval." }),
    true,
  );
});

Deno.test("triage fingerprints use the matrix SHA-256 contract", () => {
  const interval = computeSentinelInterval("hourly", now);
  const finding = {
    id: "one",
    fingerprint: "1".repeat(64),
    severity: "P1",
    title: "one",
    affected_surface: "responses",
    allowed_paths: ["src/provider.ts"],
    shared_paths: [],
    depends_on: [],
    evidence: [{ source: "repository", reference: "src/provider.ts", detail: "failure" }],
    proposed_correction: "fix it",
    validation_requirements: ["test"],
    actionable: true,
  } as const;
  assert.equal(isTriageReport({ schema_version: 1, interval, findings: [finding], no_findings_reason: null }), true);
  assert.equal(
    isTriageReport({
      schema_version: 1,
      interval,
      findings: [{ ...finding, fingerprint: "1".repeat(16) }],
      no_findings_reason: null,
    }),
    false,
  );
});

Deno.test("monitoring policy rejects keep with regression and defaults only insufficient traffic to keep", () => {
  const base = {
    schema_version: 1,
    evidence: ["sample"],
    traffic_sufficient: true,
    observed_regression: false,
  };
  assert.equal(parseMonitorDecision(JSON.stringify({ ...base, decision: "rollback" })).decision, "rollback");
  assert.equal(
    parseMonitorDecision(JSON.stringify({ ...base, decision: "rollback", traffic_sufficient: false })).decision,
    "keep",
  );
  assert.throws(
    () => parseMonitorDecision(JSON.stringify({ ...base, decision: "keep", observed_regression: true })),
    /cannot keep/,
  );
});

Deno.test("durable production decisions use the declared deployment identity contract", () => {
  const candidate = {
    app: "ai-ubq-fi",
    git_sha: "2".repeat(40),
    revision: "candidate-revision",
    health_url: "https://ai.ubq.fi/health",
    observed_at: "2026-08-21T06:30:00.000Z",
  };
  const previous = {
    app: "ai-ubq-fi",
    git_sha: "1".repeat(40),
    revision: "previous-revision",
    health_url: "https://ai.ubq.fi/health",
    observed_at: "2026-08-21T06:00:00.000Z",
  };
  assert.deepEqual(
    durableProductionDecision(
      {
        schema_version: 1,
        decision: "keep",
        evidence: ["No candidate regression observed."],
        traffic_sufficient: false,
        observed_regression: false,
      },
      candidate,
      previous,
    ),
    {
      schema_version: 1,
      decision: "keep",
      evidence: ["No candidate regression observed."],
      traffic_sufficient: false,
      candidate,
      previous,
    },
  );
});

Deno.test("replay bundle artifact Bloom names never omit a recorded case group", () => {
  const recorded = ["1".repeat(64), "a".repeat(64)];
  const name = replayIndexArtifactName(recorded);
  assert.match(name, /^sentinel-replay-bundle-v1-[A-Za-z0-9_-]{43}$/);
  assert.equal(replayIndexArtifactMayMatch(name, new Set([recorded[0]!])), true);
  assert.equal(replayIndexArtifactMayMatch(`${name}-123456`, new Set([recorded[1]!])), true);
  assert.equal(replayIndexArtifactMayMatch("sentinel-replay-bundle-v1-invalid", new Set(recorded)), false);
  assert.equal(replayIndexArtifactMayMatch(`${name}-bad/run`, new Set(recorded)), false);
});

Deno.test("matrix issue delivery merge validation fixture variants", async () => {
  type DeepMutable<T> = T extends object ? { -readonly [K in keyof T]: DeepMutable<T[K]> } : T;

  const runId = "123456789";
  const planBase = "a".repeat(40);
  const effectiveBase = "b".repeat(40);
  const cellHead = "c".repeat(40);
  const head = "d".repeat(40);
  const merged = "e".repeat(40);
  const branch = "sentinel/candidate-fixture";
  const marker = "<!-- provider-sentinel:issue:fixture -->";

  const plan = await buildMatrixPlan({
    run_id: runId,
    run_attempt: 1,
    base_sha: planBase,
    evidence_digests: [],
    findings: [{
      id: "finding-1",
      fingerprint: "1".repeat(64),
      allowed_paths: ["src/example.ts"],
    }],
  });
  const cell = plan.cells[0]!;

  const unsigned: DeepMutable<MatrixCycleReportV1> = {
    schema_version: 1,
    run_id: runId,
    run_attempt: 1,
    plan_digest: plan.manifest_digest,
    base_sha: planBase,
    cell_dispositions: [{
      cell_id: cell.cell_id,
      branch: cell.branch,
      finding_ids: [...cell.finding_ids],
      status: "accepted",
      head_sha: cellHead,
      reason: null,
    }],
    accepted_ancestry: [{
      cell_id: cell.cell_id,
      cell_head_sha: cellHead,
      integrated_head_sha: head,
      is_ancestor: true,
    }],
    rejected_branches: [],
    blocked_branches: [],
    integrated_candidate: {
      base_sha: effectiveBase,
      branch,
      head_sha: head,
      tree_sha: "f".repeat(40),
    },
    delivery: {
      status: "published",
      pr_number: 129,
      merge_sha: merged,
      reason: null,
    },
    cycle_digest: "0".repeat(64),
  };

  const cycle: DeepMutable<MatrixCycleReportV1> = {
    ...unsigned,
    cycle_digest: await matrixCycleReportDigest(unsigned),
  };

  const pullRequest = {
    schema_version: 1 as const,
    issue_number: 112,
    fingerprint: "1".repeat(64),
    pull_request_number: 129,
    pull_request_url: "https://github.com/ubiquity/ai.ubq.fi/pull/129",
    head_branch: branch,
    head_sha: head,
    base_branch: "development" as const,
    marker,
    reused: false,
  };

  const rawPull = {
    number: 129,
    html_url: pullRequest.pull_request_url,
    state: "closed",
    merged_at: "2026-09-05T12:00:00Z",
    merge_commit_sha: merged,
    body: marker,
    head: { ref: branch, sha: head },
    base: { ref: "development" },
  };

  const commit = {
    sha: merged,
    parents: [{ sha: effectiveBase }, { sha: head }],
  };

  const variants = [
    "valid_effective_base",
    "wrong_plan_digest",
    "wrong_cycle_digest",
    "wrong_run",
    "wrong_attempt",
    "wrong_pr",
    "wrong_status",
    "wrong_delivery_sha",
    "wrong_marker",
    "open_pr",
    "missing_merge_sha",
    "wrong_commit_sha",
    "wrong_base_parent",
    "wrong_head_parent",
    "swapped_parents",
    "one_parent",
    "three_parents",
    "pr_branch_drift",
    "pr_head_drift",
    "pr_base_drift",
    "valid_rollback",
  ] as const;

  for (const variant of variants) {
    let p: typeof plan = structuredClone(plan);
    const c = structuredClone(cycle);
    const pr = structuredClone(rawPull);
    const co = structuredClone(commit);

    let outcome: "kept" | "rolled_back" = "kept";
    let workflowRunId = runId;
    let workflowRunAttempt = 1;

    switch (variant) {
      case "wrong_plan_digest":
        p = { ...p, manifest_digest: "9".repeat(64) };
        break;
      case "wrong_cycle_digest":
        c.cycle_digest = "9".repeat(64);
        break;
      case "wrong_run":
        workflowRunId = "222";
        break;
      case "wrong_attempt":
        workflowRunAttempt = 2;
        break;
      case "wrong_pr":
        c.delivery.pr_number = 130;
        break;
      case "wrong_status":
        c.delivery.status = "rolled_back";
        break;
      case "wrong_delivery_sha":
        c.delivery.merge_sha = "9".repeat(40);
        break;
      case "wrong_marker":
        pr.body = "removed";
        break;
      case "open_pr":
        pr.state = "open";
        break;
      case "missing_merge_sha":
        pr.merge_commit_sha = "";
        break;
      case "wrong_commit_sha":
        co.sha = "9".repeat(40);
        break;
      case "wrong_base_parent":
        co.parents[0] = { sha: planBase };
        break;
      case "wrong_head_parent":
        co.parents[1] = { sha: cellHead };
        break;
      case "swapped_parents":
        co.parents.reverse();
        break;
      case "one_parent":
        co.parents.pop();
        break;
      case "three_parents":
        co.parents.push({ sha: planBase });
        break;
      case "pr_branch_drift":
        pr.head.ref = `${branch}-drift`;
        break;
      case "pr_head_drift":
        pr.head.sha = planBase;
        break;
      case "pr_base_drift":
        pr.base.ref = "main";
        break;
      case "valid_rollback":
        outcome = "rolled_back";
        c.delivery.status = "rolled_back";
        break;
    }

    if (c.delivery.status === "rolled_back") {
      c.delivery.reason = "fixture rollback";
    }
    if (variant !== "wrong_cycle_digest") {
      c.cycle_digest = await matrixCycleReportDigest(c);
    }

    const calls: string[] = [];
    const fetcher = (input: RequestInfo | URL): Promise<Response> => {
      const url = String(input);
      calls.push(url);
      if (url.endsWith("/pulls/129")) {
        return Promise.resolve(Response.json(pr));
      }
      if (url.endsWith(`/commits/${merged}`)) {
        return Promise.resolve(Response.json(co));
      }
      throw new Error("Unexpected fixture network target");
    };

    const execute = () =>
      validateMatrixIssueDeliveryMerge({
        token: "fixture-only",
        repository: "ubiquity/ai.ubq.fi",
        workflowRunId,
        workflowRunAttempt,
        pullRequest,
        outcome,
        deployedSha: merged,
        matrixPlanValue: p,
        matrixCycleValue: c,
        fetcher: fetcher as typeof fetch,
      });

    if (variant.startsWith("valid")) {
      await execute();
      assert.equal(calls.length, 2);
    } else {
      await assert.rejects(execute);
    }
  }
});

Deno.test({
  name: "matrix convergence artifact materialize workflow fixture: valid, missing selection, changed run, empty plan",
  ignore: matrixVerifierTestsIgnored,
  async fn() {
    const enc = new TextEncoder();
    const key = new Uint8Array(32).fill(7);
    const repositoryPath = fileURLToPath(new URL("..", import.meta.url)).replace(/\/$/, "");

    const workflow = await Deno.readTextFile(`${repositoryPath}/.github/workflows/provider-sentinel.yml`);
    const start = workflow.indexOf("- name: Materialize and verify encrypted matrix convergence inputs");
    const block = workflow.slice(start, workflow.indexOf("\n      - name:", start + 1));
    const marker = "deno eval --frozen --lock=deno.lock '\n";
    const evalStart = block.indexOf(marker) + marker.length;
    assert.ok(evalStart >= marker.length);
    const snippet = block
      .slice(evalStart, block.lastIndexOf("\n          '"))
      .replaceAll('from "./scripts/', `from "${repositoryPath}/scripts/`);

    const fp = "b".repeat(64);
    const finding = {
      id: `github-issue:208:${fp}`,
      fingerprint: fp,
      severity: "P3",
      title: "Fixture issue",
      affected_surface: "src/fix.ts",
      allowed_paths: ["src/fix.ts"],
      shared_paths: [],
      depends_on: [],
      evidence: [{
        source: "github_issue",
        reference: "https://github.com/ubiquity/ai.ubq.fi/issues/208",
        detail: "fixture",
      }],
      proposed_correction: "Fix fixture",
      validation_requirements: ["fixture validation"],
      actionable: true,
    };
    const triage = {
      schema_version: 1,
      interval: { start: "2026-09-05T00:00:00Z", end: "2026-09-05T01:00:00Z", duration_ms: 3600000 },
      findings: [finding],
      no_findings_reason: null,
    };
    const plan = await buildMatrixPlan({
      run_id: "12345",
      run_attempt: 1,
      base_sha: "a".repeat(40),
      evidence_digests: [],
      findings: [{ ...finding, prohibited_paths: [] }],
    });
    const cell = plan.cells[0]!;

    const unsigned = {
      schema_version: 1 as const,
      run_id: plan.run_id,
      run_attempt: 1,
      plan_digest: plan.manifest_digest,
      cell_id: cell.cell_id,
      base_sha: plan.base_sha,
      branch: cell.branch,
      head_sha: "d".repeat(40),
      tree_sha: "e".repeat(40),
      changed_paths: ["src/fix.ts"],
      finding_dispositions: [{
        finding_id: finding.id,
        fingerprint: fp,
        status: "implemented" as const,
        summary: "fixed",
        changed_files: ["src/fix.ts"],
        validation: ["fixture validation"],
      }],
      validation: { passed: true, checks: [{ name: "focused", passed: true, detail: "passed" }] },
      replay: { attempted: false, passed: true, results: [] },
      status: "succeeded" as const,
      failure_reason: null,
      artifact_sha256: "f".repeat(64),
      report_digest: "0".repeat(64),
    };
    const report = { ...unsigned, report_digest: await matrixCellReportDigest(unsigned) };

    const recovery = {
      schema_version: 1,
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "208",
        source_revision: fp,
        candidate_generation: 1,
      },
      run_id: plan.run_id,
      attempt: 1,
      lease_token: "prepare-lease",
      base_sha: plan.base_sha,
      phase: "claimed",
      disposition: "active",
      state_version: 1,
      created_at: "2026-09-05T00:00:00Z",
      updated_at: "2026-09-05T00:00:00Z",
      candidate_branch: null,
      candidate_sha: null,
      changed_files: [],
      tree_sha: null,
      failure_class: null,
      failure_fingerprint: null,
      artifact_ids: [],
      artifact_digests: [],
      reason: "fixture",
      next_action: "fixture",
      predecessor: null,
    };
    const selection = {
      schema_version: 1,
      issue_id: 208,
      issue_number: 208,
      fingerprint: fp,
      body_sha256: "c".repeat(64),
      comments: 0,
      priority: "P3",
      time_label: "Time: <2 Hours",
      files: ["src/fix.ts"],
      updated_at: "2026-09-05T00:00:00Z",
    };

    const file = (path: string, value: unknown): { path: string; bytes: Uint8Array<ArrayBuffer> } => ({
      path,
      bytes: enc.encode(JSON.stringify(value)),
    });

    const runMaterialize = async (
      tmp: string,
      planBundle: readonly { path: string; bytes: Uint8Array<ArrayBuffer> }[],
      cellBundle: readonly { path: string; bytes: Uint8Array<ArrayBuffer> }[],
      expectedPlanDigest: string,
    ): Promise<{ code: number; stderr: string }> => {
      await Deno.mkdir(`${tmp}/.sentinel/reports/matrix`, { recursive: true });
      await Deno.mkdir(`${tmp}/sentinel-matrix-plan/fixture`, { recursive: true });
      await Deno.mkdir(`${tmp}/sentinel-matrix-cell-reports/fixture`, { recursive: true });
      await Deno.writeFile(
        `${tmp}/sentinel-matrix-plan/fixture/sentinel-evidence-v1.json`,
        await encryptSentinelArtifact([...planBundle], key),
      );
      await Deno.writeFile(
        `${tmp}/sentinel-matrix-cell-reports/fixture/sentinel-evidence-v1.json`,
        await encryptSentinelArtifact([...cellBundle], key),
      );
      const processOutput = await new Deno.Command(Deno.execPath(), {
        args: ["eval", "--no-config", "--cached-only", snippet],
        cwd: tmp,
        clearEnv: true,
        env: {
          DENO_DIR: `${tmp}/cache`,
          RUNNER_TEMP: tmp,
          EXPECTED_RUN_ID: plan.run_id,
          EXPECTED_RUN_ATTEMPT: "1",
          EXPECTED_BASE_SHA: plan.base_sha,
          EXPECTED_PLAN_DIGEST: expectedPlanDigest,
          SENTINEL_ARTIFACT_KEY: btoa(String.fromCharCode(...key)),
        },
        stdout: "piped",
        stderr: "piped",
      }).output();
      return { code: processOutput.code, stderr: new TextDecoder().decode(processOutput.stderr) };
    };

    for (const variant of ["valid", "missing_selection", "changed_run"] as const) {
      const tmp = await Deno.makeTempDir({ prefix: "uos-matrix-artifact-proof-" });
      try {
        const planBundle = [
          file("reports/matrix-plan.json", plan),
          file("reports/triage.json", triage),
          file(
            "reports/recovery-record-v1.json",
            variant === "changed_run" ? { ...recovery, run_id: "67890" } : recovery,
          ),
        ];
        if (variant !== "missing_selection") {
          planBundle.push(file("reports/github-issue-selection.json", selection));
        }
        const { code, stderr } = await runMaterialize(
          tmp,
          planBundle,
          [file(`reports/matrix/${cell.cell_id}/cell.json`, report)],
          plan.manifest_digest,
        );
        if (variant === "valid") {
          assert.equal(code, 0, stderr);
          assert.deepEqual(
            JSON.parse(await Deno.readTextFile(`${tmp}/.sentinel/reports/github-issue-selection.json`)),
            selection,
          );
          assert.deepEqual(
            JSON.parse(await Deno.readTextFile(`${tmp}/.sentinel/reports/recovery-record-v1.json`)),
            recovery,
          );
          assert.deepEqual(JSON.parse(await Deno.readTextFile(`${tmp}/${cell.report_path}`)), report);
        } else {
          assert.notEqual(code, 0);
          assert.match(
            stderr,
            variant === "missing_selection"
              ? /github issue selection does not match/
              : /recovery record identity changed/,
          );
        }
      } finally {
        await Deno.remove(tmp, { recursive: true });
      }
    }

    const emptyTriage = {
      schema_version: 1,
      interval: { start: "2026-09-05T00:00:00Z", end: "2026-09-05T01:00:00Z", duration_ms: 3600000 },
      findings: [],
      no_findings_reason: "Fixture window had no actionable findings.",
    };
    const emptyPlan = await buildMatrixPlan({
      run_id: "12345",
      run_attempt: 1,
      base_sha: "a".repeat(40),
      evidence_digests: [],
      findings: [],
    });
    const emptyTmp = await Deno.makeTempDir({ prefix: "uos-matrix-artifact-proof-" });
    try {
      const { code, stderr } = await runMaterialize(
        emptyTmp,
        [file("reports/matrix-plan.json", emptyPlan), file("reports/triage.json", emptyTriage)],
        [],
        emptyPlan.manifest_digest,
      );
      assert.equal(code, 0, stderr);
      await assert.rejects(Deno.stat(`${emptyTmp}/.sentinel/reports/github-issue-selection.json`));
      await assert.rejects(Deno.stat(`${emptyTmp}/.sentinel/reports/recovery-record-v1.json`));
    } finally {
      await Deno.remove(emptyTmp, { recursive: true });
    }
  },
});

type MatrixPreparedConvergenceFixture = Readonly<{
  runId: string;
  runAttempt: number;
  repository: string;
  interval: ReturnType<typeof computeSentinelInterval>;
  job: GitHubIssueJob;
  triage: TriageReport;
  plan: MatrixPlanV1;
  prepared: SentinelRecoveryRecordV1;
  issueSelection: GitHubIssueSelectionReport;
}>;

const matrixPreparedConvergenceFixture = async (): Promise<MatrixPreparedConvergenceFixture> => {
  const runId = "23546719284";
  const runAttempt = 1;
  const repository = "ubiquity/ai.ubq.fi";
  const interval = computeSentinelInterval("hourly", now);
  const job = await createGitHubIssueJob(repository, sentinelGitHubIssue(), noIssueRelations);
  assert.ok(job);
  const triage = githubIssueJobTriageReport(job, interval);
  const plan = await buildMatrixPlan({
    run_id: runId,
    run_attempt: runAttempt,
    base_sha: "2".repeat(40),
    evidence_digests: [],
    findings: triage.findings.map((finding) => ({
      id: finding.id,
      fingerprint: finding.fingerprint,
      allowed_paths: finding.allowed_paths,
      prohibited_paths: SENTINEL_POLICY.protectedImplementationPaths,
      shared_paths: finding.shared_paths,
      depends_on: finding.depends_on,
      validation_requirements: finding.validation_requirements,
    })),
  });
  const prepared = parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository,
      source_kind: "github_issue",
      source_id: String(job.issueId),
      source_revision: job.fingerprint,
      candidate_generation: 1,
    },
    run_id: plan.run_id,
    attempt: runAttempt,
    lease_token: `${runId}-${runAttempt}`,
    base_sha: plan.base_sha,
    phase: "claimed",
    disposition: "active",
    state_version: 1,
    created_at: "2026-08-21T06:00:00.000Z",
    updated_at: "2026-08-21T06:00:00.000Z",
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: [],
    artifact_digests: [],
    reason: "Matrix convergence prepared github issue work",
    next_action: "Run the github issue implementation stage",
    predecessor: null,
  });
  const issueSelection: GitHubIssueSelectionReport = {
    schema_version: 1,
    issue_id: job.issueId,
    issue_number: job.number,
    fingerprint: job.fingerprint,
    body_sha256: job.bodySha256,
    comments: job.comments,
    priority: job.priority,
    time_label: job.timeLabel,
    files: [...job.files],
    updated_at: job.updatedAt,
  };
  return { runId, runAttempt, repository, interval, job, triage, plan, prepared, issueSelection };
};

type MatrixConvergenceInputs = Readonly<{
  plan: MatrixPlanV1;
  triage: TriageReport;
  preparedRecovery: SentinelRecoveryRecordV1;
  issueSelection: GitHubIssueSelectionReport | null;
  selectedIssueJob: GitHubIssueJob | null;
}>;

Deno.test("matrix prepared recovery binds github issue convergence identity exactly", async () => {
  const { runId, runAttempt, repository, job, triage, plan, prepared, issueSelection } =
    await matrixPreparedConvergenceFixture();
  assert.equal(prepared.schema_version, 1);
  assert.deepEqual(prepared.identity, {
    repository,
    source_kind: "github_issue",
    source_id: String(job.issueId),
    source_revision: job.fingerprint,
    candidate_generation: 1,
  });
  assert.equal(prepared.run_id, plan.run_id);
  assert.equal(prepared.attempt, runAttempt);
  assert.equal(prepared.lease_token, `${runId}-${runAttempt}`);
  assert.equal(prepared.base_sha, plan.base_sha);
  assert.equal(prepared.phase, "claimed");
  assert.equal(prepared.disposition, "active");
  assert.equal(prepared.state_version, 1);
  assert.ok(Number.isFinite(Date.parse(prepared.created_at)));
  assert.ok(Number.isFinite(Date.parse(prepared.updated_at)));
  assert.equal(prepared.candidate_branch, null);
  assert.equal(prepared.candidate_sha, null);
  assert.equal(prepared.tree_sha, null);
  assert.equal(prepared.failure_class, null);
  assert.equal(prepared.failure_fingerprint, null);
  assert.equal(prepared.predecessor, null);
  assert.deepEqual(prepared.changed_files, []);
  assert.deepEqual(prepared.artifact_ids, []);
  assert.deepEqual(prepared.artifact_digests, []);
  assert.ok(prepared.reason !== null && prepared.reason.trim().length > 0);
  assert.ok(prepared.next_action !== null && prepared.next_action.trim().length > 0);
  assert.deepEqual(issueSelection, {
    schema_version: 1,
    issue_id: job.issueId,
    issue_number: job.number,
    fingerprint: job.fingerprint,
    body_sha256: job.bodySha256,
    comments: job.comments,
    priority: job.priority,
    time_label: job.timeLabel,
    files: [...job.files],
    updated_at: job.updatedAt,
  });
  const selection = await bindMatrixConvergenceWork({
    repository,
    runId,
    runAttempt,
    plan,
    triage,
    preparedRecovery: prepared,
    issueSelection,
    selectedIssueJob: job,
  });
  assert.equal(selection.source, "github_issue");
  assert.equal(selection.reason, "hourly_github_issue");
  assert.equal(selection.backlogEntry, null);
  assert.equal(selection.issueJob, job);
  assert.equal(selection.triage, triage);
});

Deno.test("matrix prepared recovery rejects github issue convergence transport drift", async () => {
  const fixture = await matrixPreparedConvergenceFixture();
  const bindWith = async (overrides: Partial<MatrixConvergenceInputs>) =>
    await bindMatrixConvergenceWork({
      repository: fixture.repository,
      runId: fixture.runId,
      runAttempt: fixture.runAttempt,
      plan: overrides.plan ?? fixture.plan,
      triage: overrides.triage ?? fixture.triage,
      preparedRecovery: overrides.preparedRecovery ?? fixture.prepared,
      issueSelection: overrides.issueSelection === undefined ? fixture.issueSelection : overrides.issueSelection,
      selectedIssueJob: overrides.selectedIssueJob === undefined ? fixture.job : overrides.selectedIssueJob,
    });
  const rejectCases: ReadonlyArray<
    Readonly<{
      name: string;
      drift: (fixture: MatrixPreparedConvergenceFixture) => Partial<MatrixConvergenceInputs>;
      error: RegExp;
    }>
  > = [
    {
      name: "missing issue selection",
      drift: () => ({ issueSelection: null }),
      error: /requires its selection report and current issue job/,
    },
    {
      name: "missing current issue job",
      drift: () => ({ selectedIssueJob: null }),
      error: /requires its selection report and current issue job/,
    },
    {
      name: "changed issue id",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, issue_id: current.job.issueId + 1 } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue number",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, issue_number: current.job.number + 1 } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue fingerprint",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, fingerprint: "0".repeat(64) } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue body digest",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, body_sha256: "c".repeat(64) } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue comments",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, comments: current.job.comments + 1 } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue priority",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, priority: "P2" } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue time label",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, time_label: "Time: <8 Hours" } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue files",
      drift: (current) => ({
        issueSelection: { ...current.issueSelection, files: [...current.issueSelection.files, "src/extra.ts"] },
      }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed issue updated at",
      drift: (current) => ({ issueSelection: { ...current.issueSelection, updated_at: "2026-08-23T19:08:27Z" } }),
      error: /does not match the current issue job/,
    },
    {
      name: "changed plan run",
      drift: (current) => ({ plan: { ...current.plan, run_id: "transport-drift" } }),
      error: /identity changed in transport/,
    },
    {
      name: "changed plan attempt",
      drift: (current) => ({ plan: { ...current.plan, run_attempt: 2 } }),
      error: /identity changed in transport/,
    },
    {
      name: "changed recovery run",
      drift: (current) => ({ preparedRecovery: { ...current.prepared, run_id: "transport-drift" } }),
      error: /run identity changed in transport/,
    },
    {
      name: "changed recovery repository",
      drift: (current) => ({
        preparedRecovery: {
          ...current.prepared,
          identity: { ...current.prepared.identity, repository: "elsewhere/repository" },
        },
      }),
      error: /repository differs/,
    },
    {
      name: "changed recovery base sha",
      drift: (current) => ({ preparedRecovery: { ...current.prepared, base_sha: "3".repeat(40) } }),
      error: /base SHA differs/,
    },
    {
      name: "changed recovery source id",
      drift: (current) => ({
        preparedRecovery: {
          ...current.prepared,
          identity: { ...current.prepared.identity, source_id: "10_114" },
        },
      }),
      error: /does not match the prepared recovery identity/,
    },
    {
      name: "changed recovery source revision",
      drift: (current) => ({
        preparedRecovery: {
          ...current.prepared,
          identity: { ...current.prepared.identity, source_revision: "0".repeat(64) },
        },
      }),
      error: /does not match the prepared recovery identity/,
    },
    {
      name: "changed recovery phase state",
      drift: (current) => ({ preparedRecovery: { ...current.prepared, phase: "implementation_running" } }),
      error: /not an active claimed recovery/,
    },
    {
      name: "changed triage ownership",
      drift: (current) => ({
        triage: { ...current.triage, findings: [{ ...current.triage.findings[0]!, fingerprint: "0".repeat(64) }] },
      }),
      error: /does not match immutable plan ownership/,
    },
    {
      name: "changed triage content",
      drift: (current) => ({
        triage: { ...current.triage, findings: [{ ...current.triage.findings[0]!, severity: "P2" }] },
      }),
      error: /triage differs from the prepared matrix convergence triage/,
    },
  ];
  for (const rejectCase of rejectCases) {
    await assert.rejects(() => bindWith(rejectCase.drift(fixture)), rejectCase.error, rejectCase.name);
  }
});

Deno.test("matrix prepared recovery reuses the exact prepared ledger record", async () => {
  const fixture = await matrixPreparedConvergenceFixture();
  const ledger = parseSentinelRecoveryRecord(JSON.parse(JSON.stringify(fixture.prepared)));
  assert.notEqual(ledger, fixture.prepared);
  const reused = reuseMatrixPreparedRecoveryRecord(fixture.prepared, ledger);
  assert.equal(reused, ledger);
  assert.deepEqual(reused, fixture.prepared);
  assert.throws(
    () => reuseMatrixPreparedRecoveryRecord(fixture.prepared, null),
    /lost its current recovery record/,
  );
  const rejectCases: ReadonlyArray<Readonly<{ name: string; current: unknown }>> = [
    { name: "changed attempt", current: { ...ledger, attempt: 2 } },
    { name: "changed lease token", current: { ...ledger, lease_token: "transport-drift-1" } },
    {
      name: "changed candidate generation",
      current: { ...ledger, identity: { ...ledger.identity, candidate_generation: 2 } },
    },
    { name: "changed base sha", current: { ...ledger, base_sha: "3".repeat(40) } },
    { name: "changed phase", current: { ...ledger, phase: "implementation_running" } },
    { name: "changed state version", current: { ...ledger, state_version: 2 } },
  ];
  for (const rejectCase of rejectCases) {
    const drifted = parseSentinelRecoveryRecord(rejectCase.current);
    assert.throws(
      () => reuseMatrixPreparedRecoveryRecord(fixture.prepared, drifted),
      /differs from the ledger recovery record/,
      rejectCase.name,
    );
  }
});

Deno.test("planning barrier classification requires evidence-bearing stable reasons only", () => {
  assert.equal(isStablePlanningBlocker("requires_protected_action: scripts/sentinel/main.ts"), true);
  assert.equal(isStablePlanningBlocker("unresolved_dependency: issue 209 is required and open"), true);
  assert.equal(isStablePlanningBlocker("needs_owner_decision: the owner cannot be resolved"), true);
  assert.equal(isStablePlanningBlocker("scope_not_bounded: issue text has no bounded path"), true);
  assert.equal(isStablePlanningBlocker("scope_not_bounded"), false);
  assert.equal(isStablePlanningBlocker("requires_protected_action"), false);
  assert.equal(isStablePlanningBlocker("unresolved_dependency"), false);
  assert.equal(isStablePlanningBlocker("needs_owner_decision: "), false);
  assert.equal(isStablePlanningBlocker("planning_failed"), false);
  assert.equal(isStablePlanningBlocker("planning_failed: transport outage"), false);
  assert.equal(isStablePlanningBlocker("plan_scope_invalid"), false);
  assert.equal(isStablePlanningBlocker("planning_interval_mismatch"), false);
  assert.equal(isStablePlanningBlocker("unresolved_dependency_garbage"), false);
  assert.equal(isStablePlanningBlocker("requires_protected_action-suffix"), false);
});

Deno.test("planning persistence writes a first fresh-source retry_wait record through the production helper", async () => {
  const job = await planningJobFixture();
  const persistence = planningPersistenceStore();
  const outcome = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-1",
    runAttempt: 1,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "planning_failed",
    now: "2026-09-01T00:00:00.000Z",
  });
  assert.equal(outcome.reason, "planning_failed");
  assert.equal(outcome.persisted, true);
  assert.equal(persistence.writes.length, 1);
  assert.match(persistence.writes[0]!.message, /chore\(sentinel\): planning retry /u);
  const after = persistence.writes[0]!.ledger;
  const key = sentinelRecoveryIdentityKey(planningIdentity(job));
  const record = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key);
  assert.ok(record);
  assert.equal(record.phase, "retry_wait");
  assert.equal(record.disposition, "active");
  assert.equal(record.attempt, 1);
  assert.equal(record.state_version, 1);
  assert.equal(record.base_sha, "b".repeat(40));
  assert.equal(record.run_id, "run-1");
  assert.equal(record.lease_token, "run-1-1");
  assert.equal(record.identity.candidate_generation, 1);
  assert.equal(record.failure_class, "transient_transport");
  assert.ok(record.failure_fingerprint !== null && /^[0-9a-f]{64}$/u.test(record.failure_fingerprint));
  assert.equal(record.reason, "planning_blocked:planning_failed");
  assert.equal(after.retry_history.length, 1);
  assert.equal(after.retry_history[0]!.attempt, 1);
  assert.equal(after.retry_history[0]!.identity.candidate_generation, 1);
  assert.equal(after.retry_decisions.length, 1);
  assert.equal(after.retry_decisions[0]!.identity_key, key);
  assert.equal(after.retry_decisions[0]!.decision.disposition, "retry_wait");
  assert.equal(after.retry_decisions[0]!.decision.attempt_count, 1);
  assert.equal(after.retry_decisions[0]!.decision.candidate_generation, 1);
});

Deno.test("planning persistence continues the same-identity attempt history on a due retry", async () => {
  const job = await planningJobFixture();
  const persistence = planningPersistenceStore();
  const first = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-1",
    runAttempt: 1,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "planning_failed",
    now: "2026-09-01T00:00:00.000Z",
  });
  assert.equal(first.persisted, true);
  const retryAt = persistence.writes[0]!.ledger.retry_decisions[0]!.decision.retry_at;
  assert.ok(retryAt !== null);
  const second = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-2",
    runAttempt: 2,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "planning_failed",
    now: retryAt,
  });
  assert.equal(second.persisted, true);
  assert.equal(persistence.writes.length, 2);
  const after = persistence.writes[1]!.ledger;
  const key = sentinelRecoveryIdentityKey(planningIdentity(job));
  const record = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key);
  assert.ok(record);
  // The due retry resumes the exact durable record instead of resetting it:
  // generation, state version, run lineage and retry phase are preserved.
  assert.equal(record.state_version, 1);
  assert.equal(record.attempt, 1);
  assert.equal(record.run_id, "run-1");
  assert.equal(record.identity.candidate_generation, 1);
  assert.equal(record.phase, "retry_wait");
  assert.equal(after.retry_history.length, 2);
  assert.deepEqual(after.retry_history.map((attempt) => attempt.attempt), [1, 2]);
  assert.deepEqual(
    after.retry_history.map((attempt) => attempt.identity.candidate_generation),
    [1, 1],
  );
  assert.equal(after.retry_decisions.length, 1);
  assert.equal(after.retry_decisions[0]!.identity_key, key);
  assert.equal(after.retry_decisions[0]!.decision.attempt_count, 2);
  assert.equal(after.retry_decisions[0]!.decision.identical_failure_count, 2);
  assert.equal(after.retry_decisions[0]!.decision.candidate_generation, 1);
  assert.equal(after.retry_decisions[0]!.decision.disposition, "retry_wait");
});

Deno.test("planning persistence opens the circuit terminal with complete retained evidence", async () => {
  const job = await planningJobFixture();
  const persistence = planningPersistenceStore();
  const attempt = (runId: string, runAttempt: number, now: string) =>
    persistPlanningOutcome({
      store: persistence.store,
      repository: "ubiquity/ai.ubq.fi",
      job,
      runId,
      runAttempt,
      pinnedBaseSha: "b".repeat(40),
      blockedReason: "planning_failed",
      now,
    });
  await attempt("run-1", 1, "2026-09-01T00:00:00.000Z");
  const firstRetryAt = persistence.writes[0]!.ledger.retry_decisions[0]!.decision.retry_at;
  assert.ok(firstRetryAt !== null);
  await attempt("run-2", 2, firstRetryAt);
  const secondRetryAt = persistence.writes[1]!.ledger.retry_decisions[0]!.decision.retry_at;
  assert.ok(secondRetryAt !== null);
  const third = await attempt("run-3", 3, secondRetryAt);
  assert.equal(third.persisted, true);
  assert.equal(persistence.writes.length, 3);
  const after = persistence.writes[2]!.ledger;
  const key = sentinelRecoveryIdentityKey(planningIdentity(job));
  const record = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key);
  assert.ok(record);
  // The third identical failure opens the circuit: the same durable record
  // transitions to the terminal manual decision without losing evidence.
  assert.equal(record.phase, "manual_required");
  assert.equal(record.disposition, "manual_required");
  assert.equal(record.state_version, 2);
  assert.equal(record.run_id, "run-1");
  assert.equal(record.lease_token, "run-1-1");
  assert.equal(record.identity.candidate_generation, 1);
  assert.equal(record.failure_class, "transient_transport");
  assert.ok(record.failure_fingerprint !== null && /^[0-9a-f]{64}$/u.test(record.failure_fingerprint));
  assert.equal(record.reason, "planning_blocked:planning_failed");
  assert.equal(record.next_action, "Resolve the named blocker before another attempt.");
  assert.equal(after.retry_history.length, 3);
  assert.deepEqual(after.retry_history.map((attempt) => attempt.attempt), [1, 2, 3]);
  assert.equal(after.retry_decisions.length, 1);
  assert.equal(after.retry_decisions[0]!.identity_key, key);
  assert.equal(after.retry_decisions[0]!.decision.disposition, "manual_required");
  assert.equal(after.retry_decisions[0]!.decision.circuit_open, true);
  assert.equal(after.retry_decisions[0]!.decision.attempt_count, 3);
  assert.equal(after.retry_decisions[0]!.decision.identical_failure_count, 3);
  assert.equal(after.retry_decisions[0]!.decision.candidate_generation, 1);
  // A later run never reopens the terminal circuit and writes nothing.
  const terminalAttempt = await attempt("run-4", 4, "2026-09-02T00:00:00.000Z");
  assert.equal(terminalAttempt.persisted, false);
  assert.equal(persistence.writes.length, 3);
});

Deno.test("planning persistence records a stable blocker terminal on a fresh source", async () => {
  const job = await planningJobFixture();
  const persistence = planningPersistenceStore();
  const outcome = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-1",
    runAttempt: 1,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "requires_protected_action: scripts/sentinel/main.ts",
    now: "2026-09-01T00:00:00.000Z",
  });
  assert.equal(outcome.persisted, true);
  assert.equal(persistence.writes.length, 1);
  const after = persistence.writes[0]!.ledger;
  const key = sentinelRecoveryIdentityKey(planningIdentity(job));
  const record = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key);
  assert.ok(record);
  assert.equal(record.phase, "manual_required");
  assert.equal(record.disposition, "manual_required");
  assert.equal(record.state_version, 1);
  assert.equal(record.reason, "planning_blocked:requires_protected_action: scripts/sentinel/main.ts");
  assert.equal(record.next_action, "Resolve the named blocker before another attempt.");
  assert.equal(record.failure_class, null);
  assert.equal(record.failure_fingerprint, null);
  assert.equal(after.retry_history.length, 1);
  assert.equal(after.retry_history[0]!.failure_class, "invalid_implementation_report");
  assert.equal(after.retry_history[0]!.attempt, 1);
  assert.equal(after.retry_decisions.length, 0);
});

Deno.test("planning persistence retains attempt history when a stable blocker terminates an active record", async () => {
  const job = await planningJobFixture();
  const persistence = planningPersistenceStore();
  const first = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-1",
    runAttempt: 1,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "planning_failed",
    now: "2026-09-01T00:00:00.000Z",
  });
  assert.equal(first.persisted, true);
  const retryAt = persistence.writes[0]!.ledger.retry_decisions[0]!.decision.retry_at;
  assert.ok(retryAt !== null);
  const second = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-2",
    runAttempt: 2,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "requires_protected_action: scripts/sentinel/main.ts",
    now: retryAt,
  });
  assert.equal(second.persisted, true);
  assert.equal(persistence.writes.length, 2);
  const after = persistence.writes[1]!.ledger;
  const key = sentinelRecoveryIdentityKey(planningIdentity(job));
  const record = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key);
  assert.ok(record);
  assert.equal(record.phase, "manual_required");
  assert.equal(record.disposition, "manual_required");
  assert.equal(record.state_version, 2);
  assert.equal(record.identity.candidate_generation, 1);
  assert.equal(record.failure_class, "invalid_implementation_report");
  assert.ok(record.failure_fingerprint !== null && /^[0-9a-f]{64}$/u.test(record.failure_fingerprint));
  assert.equal(record.reason, "planning_blocked:requires_protected_action: scripts/sentinel/main.ts");
  assert.equal(record.next_action, "Resolve the named blocker before another attempt.");
  // The terminal transition never clears prior attempts or the durable decision.
  assert.equal(after.retry_history.length, 2);
  assert.deepEqual(after.retry_history.map((attempt) => attempt.attempt), [1, 2]);
  assert.deepEqual(
    after.retry_history.map((attempt) => attempt.failure_class),
    ["transient_transport", "invalid_implementation_report"],
  );
  assert.equal(after.retry_decisions.length, 1);
  assert.equal(after.retry_decisions[0]!.identity_key, key);
  assert.equal(after.retry_decisions[0]!.decision.attempt_count, 2);
});

Deno.test("planning persistence never writes over a concurrent claim", async () => {
  const job = await planningJobFixture();
  const claimed = recoveryRecordFixture({
    identity: planningIdentity(job, 2),
    run_id: "run-claim",
    attempt: 1,
    lease_token: "run-claim-1",
    phase: "claimed",
    disposition: "active",
    state_version: 3,
    reason: "Concurrent run claimed the source",
  });
  const persistence = planningPersistenceStore(
    parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [claimed] }),
  );
  const outcome = await persistPlanningOutcome({
    store: persistence.store,
    repository: "ubiquity/ai.ubq.fi",
    job,
    runId: "run-1",
    runAttempt: 1,
    pinnedBaseSha: "b".repeat(40),
    blockedReason: "planning_failed",
    now: "2026-09-01T00:00:00.000Z",
  });
  assert.equal(outcome.reason, "planning_failed");
  assert.equal(outcome.persisted, false);
  assert.equal(persistence.writes.length, 0);
});

Deno.test("matrix prepared recovery binds review backlog convergence identity exactly", async () => {
  const runId = "23546719285";
  const runAttempt = 1;
  const repository = "ubiquity/ai.ubq.fi";
  const interval = computeSentinelInterval("hourly", now);
  const backlogEntry = reviewBacklogEntry();
  const triage = reviewBacklogTriageReport(backlogEntry, interval);
  const plan = await buildMatrixPlan({
    run_id: runId,
    run_attempt: runAttempt,
    base_sha: "2".repeat(40),
    evidence_digests: [],
    findings: triage.findings.map((finding) => ({
      id: finding.id,
      fingerprint: finding.fingerprint,
      allowed_paths: finding.allowed_paths,
      prohibited_paths: SENTINEL_POLICY.protectedImplementationPaths,
      shared_paths: finding.shared_paths,
      depends_on: finding.depends_on,
      validation_requirements: finding.validation_requirements,
    })),
  });
  const prepared = parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository,
      source_kind: "review_backlog",
      source_id: backlogEntry.fingerprint,
      source_revision: backlogEntry.sha,
      candidate_generation: 1,
    },
    run_id: plan.run_id,
    attempt: runAttempt,
    lease_token: `${runId}-${runAttempt}`,
    base_sha: plan.base_sha,
    phase: "claimed",
    disposition: "active",
    state_version: 1,
    created_at: "2026-08-21T07:00:00.000Z",
    updated_at: "2026-08-21T07:00:00.000Z",
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: [],
    artifact_digests: [],
    reason: "Matrix convergence prepared review backlog work",
    next_action: "Run the review backlog implementation stage",
    predecessor: null,
  });
  assert.deepEqual(prepared.identity, {
    repository,
    source_kind: "review_backlog",
    source_id: backlogEntry.fingerprint,
    source_revision: backlogEntry.sha,
    candidate_generation: 1,
  });
  assert.equal(prepared.run_id, plan.run_id);
  assert.equal(prepared.base_sha, plan.base_sha);
  assert.equal(prepared.phase, "claimed");
  assert.equal(prepared.disposition, "active");
  assert.equal(prepared.candidate_branch, null);
  assert.equal(prepared.candidate_sha, null);
  assert.equal(prepared.tree_sha, null);
  assert.equal(prepared.failure_class, null);
  assert.equal(prepared.failure_fingerprint, null);
  assert.equal(prepared.predecessor, null);
  assert.deepEqual(prepared.changed_files, []);
  assert.deepEqual(prepared.artifact_ids, []);
  assert.deepEqual(prepared.artifact_digests, []);
  assert.ok(prepared.reason !== null && prepared.reason.trim().length > 0);
  assert.ok(prepared.next_action !== null && prepared.next_action.trim().length > 0);
  const selection = await bindMatrixConvergenceWork({
    repository,
    runId,
    runAttempt,
    plan,
    triage,
    preparedRecovery: prepared,
    issueSelection: null,
    selectedIssueJob: null,
    selectedBacklogEntry: backlogEntry,
  });
  assert.equal(selection.source, "review_backlog");
  assert.equal(selection.reason, "hourly_review_backlog");
  assert.equal(selection.backlogEntry, backlogEntry);
  assert.equal(selection.issueJob, null);
  assert.equal(selection.triage, triage);

  const bindWith = async (
    overrides: Readonly<{
      triage?: TriageReport;
      issueSelection?: GitHubIssueSelectionReport | null;
      selectedBacklogEntry?: ReviewBacklogEntry | null;
    }>,
  ) =>
    await bindMatrixConvergenceWork({
      repository,
      runId,
      runAttempt,
      plan,
      triage: overrides.triage ?? triage,
      preparedRecovery: prepared,
      issueSelection: overrides.issueSelection === undefined ? null : overrides.issueSelection,
      selectedIssueJob: null,
      selectedBacklogEntry: overrides.selectedBacklogEntry === undefined
        ? backlogEntry
        : overrides.selectedBacklogEntry,
    });
  const unexpectedSelection: GitHubIssueSelectionReport = {
    schema_version: 1,
    issue_id: 10_113,
    issue_number: 113,
    fingerprint: "1".repeat(64),
    body_sha256: "2".repeat(64),
    comments: 0,
    priority: "P2",
    time_label: "Time: <2 Hours",
    files: ["src/http.ts"],
    updated_at: "2026-08-23T19:07:27Z",
  };
  const rejectCases: ReadonlyArray<
    Readonly<{
      name: string;
      drift: () => Parameters<typeof bindWith>[0];
      error: RegExp;
    }>
  > = [
    {
      name: "missing backlog entry",
      drift: () => ({ selectedBacklogEntry: null }),
      error: /requires its exact backlog entry without an issue selection/,
    },
    {
      name: "unexpected issue selection",
      drift: () => ({ issueSelection: unexpectedSelection }),
      error: /requires its exact backlog entry without an issue selection/,
    },
    {
      name: "changed backlog fingerprint",
      drift: () => ({ selectedBacklogEntry: { ...backlogEntry, fingerprint: "b".repeat(64) } }),
      error: /does not match the prepared recovery identity/,
    },
    {
      name: "changed backlog sha",
      drift: () => ({ selectedBacklogEntry: { ...backlogEntry, sha: "c".repeat(40) } }),
      error: /does not match the prepared recovery identity/,
    },
    {
      name: "changed backlog triage",
      drift: () => ({ triage: { ...triage, findings: [{ ...triage.findings[0]!, severity: "P3" }] } }),
      error: /triage differs from the prepared matrix convergence triage/,
    },
  ];
  for (const rejectCase of rejectCases) {
    await assert.rejects(() => bindWith(rejectCase.drift()), rejectCase.error, rejectCase.name);
  }
});

Deno.test("convergence binds the exact prepared source even when an earlier item becomes due", async () => {
  const runId = "23546719286";
  const runAttempt = 1;
  const repository = "ubiquity/ai.ubq.fi";
  const interval = computeSentinelInterval("hourly", now);
  const earlier = reviewBacklogEntry({ fingerprint: "9".repeat(64), sha: "7".repeat(40), severity: "P0" });
  const prepared = reviewBacklogEntry({ fingerprint: "8".repeat(64), sha: "6".repeat(40), severity: "P1" });
  const markdown = renderReviewBacklog([earlier, prepared]);
  // The earlier entry's recovery retry becomes due while the cells run: a
  // plain reselection would now return it and retarget prepared convergence.
  const waiting = recoveryRecordFixture({
    identity: {
      repository,
      source_kind: "review_backlog",
      source_id: earlier.fingerprint,
      source_revision: earlier.sha,
      candidate_generation: 1,
    },
    phase: "retry_wait",
    state_version: 5,
    updated_at: "2026-08-28T18:05:00.000Z",
  });
  const waitingKey = sentinelRecoveryIdentityKey(waiting.identity);
  const due = {
    disposition: "retry_wait" as const,
    should_retry: true,
    circuit_open: false,
    validation_repair_allowed: false,
    source_revision_changed: false,
    candidate_generation: 1,
    attempt_count: 1,
    identical_failure_count: 1,
    backoff_ms: 60_000,
    retry_at: "2026-08-28T18:15:00.000Z",
    failure_class: "runner_interruption" as const,
    failure_fingerprint: "f".repeat(64),
    reason: "The bounded Sentinel retry policy scheduled another attempt.",
    next_action: "Retry after the scheduled delay.",
  };
  const ledger = parseSentinelRecoveryLedger({
    ...emptySentinelRecoveryLedger(),
    records: [waiting],
    retry_decisions: [{ identity_key: waitingKey, decision: due }],
  });
  const dueContext = recoverySelectionContext(ledger, null, "2026-08-28T18:30:00.000Z");
  assert.equal(selectNextReviewBacklogEntry(markdown, dueContext)?.fingerprint, earlier.fingerprint);
  // Exact prepared-source lookup still returns the prepared entry, so the
  // convergence binding below stays on the prepared work.
  const selected = findReviewBacklogEntry(markdown, prepared.fingerprint, prepared.sha);
  assert.equal(selected?.fingerprint, prepared.fingerprint);
  assert.equal(selected?.sha, prepared.sha);
  assert.deepEqual(selected, prepared);
  const triage = reviewBacklogTriageReport(prepared, interval);
  const plan = await buildMatrixPlan({
    run_id: runId,
    run_attempt: runAttempt,
    base_sha: "2".repeat(40),
    evidence_digests: [],
    findings: triage.findings.map((finding) => ({
      id: finding.id,
      fingerprint: finding.fingerprint,
      allowed_paths: finding.allowed_paths,
      prohibited_paths: SENTINEL_POLICY.protectedImplementationPaths,
      shared_paths: finding.shared_paths,
      depends_on: finding.depends_on,
      validation_requirements: finding.validation_requirements,
    })),
  });
  const preparedRecovery: SentinelRecoveryRecordV1 = parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository,
      source_kind: "review_backlog",
      source_id: prepared.fingerprint,
      source_revision: prepared.sha,
      candidate_generation: 1,
    },
    run_id: plan.run_id,
    attempt: runAttempt,
    lease_token: `${runId}-${runAttempt}`,
    base_sha: plan.base_sha,
    phase: "claimed",
    disposition: "active",
    state_version: 1,
    created_at: "2026-08-28T18:00:00.000Z",
    updated_at: "2026-08-28T18:00:00.000Z",
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: [],
    artifact_digests: [],
    reason: "Matrix convergence prepared review backlog work",
    next_action: "Run the review backlog implementation stage",
    predecessor: null,
  });
  const selection = await bindMatrixConvergenceWork({
    repository,
    runId,
    runAttempt,
    plan,
    triage,
    preparedRecovery,
    issueSelection: null,
    selectedIssueJob: null,
    selectedBacklogEntry: selected,
  });
  assert.equal(selection.backlogEntry?.fingerprint, prepared.fingerprint);
  assert.equal(selection.source, "review_backlog");
});

const protectedSnapshotPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run", command: "git" }),
]);
const protectedSnapshotTestsIgnored = protectedSnapshotPermissions.some(
  (permission) => permission.state !== "granted",
);
const protectedSnapshotFifoTestsIgnored = protectedSnapshotTestsIgnored ||
  Deno.build.os !== "linux" ||
  (await Deno.permissions.query({ name: "run", command: "mkfifo" })).state !== "granted";

const createProtectedSnapshotFixture = async (): Promise<string> => {
  const repo = await Deno.makeTempDir({ prefix: "sentinel-protected-snapshot-" });
  await matrixVerifierGit(repo, ["init", "-b", "development"]);
  await matrixVerifierGit(repo, ["config", "user.name", "Sentinel Protected Snapshot Fixture"]);
  await matrixVerifierGit(repo, ["config", "user.email", "sentinel-protected-fixture@example.invalid"]);
  await Deno.mkdir(`${repo}/docs/review-results/nested`, { recursive: true });
  await Deno.writeTextFile(`${repo}/docs/review-results/000-base.json`, "base\n");
  await Deno.writeTextFile(`${repo}/docs/review-results/nested/deep.txt`, "deep\n");
  await matrixVerifierGit(repo, ["add", "--all"]);
  await matrixVerifierGit(repo, ["commit", "--no-gpg-sign", "-m", "protected snapshot base"]);
  return repo;
};

const snapshotProtectedRoot = async (repo: string, path: string): Promise<string> =>
  (await hashProtectedFiles(repo, [path]))[path]!;

Deno.test({
  name: "production protected-path hashing snapshots the review-results directory and preserves file hashing",
  ignore: protectedSnapshotTestsIgnored,
  fn: async () => {
    const cwd = Deno.cwd();
    const paths = SENTINEL_POLICY.protectedImplementationPaths;
    const hashes = await hashProtectedFiles(cwd, paths);
    assert.deepEqual(Object.keys(hashes).sort(), [...paths].sort());
    const directory = hashes["docs/sentinel-review-results"]!;
    assert.match(directory, /^dir:sha256:[0-9a-f]{64}$/u);
    for (const [path, hash] of Object.entries(hashes)) {
      assert.ok(hash.length > 0, `empty protected hash for ${path}`);
    }
    // Repeated production-shaped invocations are byte-for-byte identical.
    assert.deepEqual(await hashProtectedFiles(cwd, paths), hashes);
    // Ordinary file roots keep exact Git blob hashing.
    const expected = await matrixVerifierGit(cwd, ["hash-object", "--no-filters", ".gitleaksignore"]);
    assert.equal(hashes[".gitleaksignore"], expected);
  },
});

Deno.test({
  name: "protected directory snapshots are deterministic and detect nested edits and mode changes",
  ignore: protectedSnapshotTestsIgnored,
  fn: async () => {
    const repo = await createProtectedSnapshotFixture();
    try {
      const baseline = await snapshotProtectedRoot(repo, "docs/review-results");
      assert.match(baseline, /^dir:sha256:[0-9a-f]{64}$/u);
      assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      // Identical trees written in a different order produce the same digest.
      await Deno.mkdir(`${repo}/sibling/nested`, { recursive: true });
      await Deno.writeTextFile(`${repo}/sibling/nested/deep.txt`, "deep\n");
      await Deno.writeTextFile(`${repo}/sibling/000-base.json`, "base\n");
      assert.equal(await snapshotProtectedRoot(repo, "sibling"), baseline);
      // Nested edits change the digest.
      await Deno.writeTextFile(`${repo}/docs/review-results/nested/deep.txt`, "deep but changed\n");
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      await Deno.writeTextFile(`${repo}/docs/review-results/nested/deep.txt`, "deep\n");
      assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      if (Deno.build.os === "linux") {
        // File, directory, and root permission modes are part of the snapshot.
        await Deno.chmod(`${repo}/docs/review-results/nested/deep.txt`, 0o600);
        assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
        await Deno.chmod(`${repo}/docs/review-results/nested/deep.txt`, 0o644);
        assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
        await Deno.chmod(`${repo}/docs/review-results/nested`, 0o700);
        assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
        await Deno.chmod(`${repo}/docs/review-results/nested`, 0o755);
        assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
        await Deno.chmod(`${repo}/docs/review-results`, 0o700);
        assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
        await Deno.chmod(`${repo}/docs/review-results`, 0o755);
        assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      }
    } finally {
      await Deno.remove(repo, { recursive: true });
    }
  },
});

Deno.test({
  name: "protected directory snapshots detect adds, deletes, renames and untracked entries",
  ignore: protectedSnapshotTestsIgnored,
  fn: async () => {
    const repo = await createProtectedSnapshotFixture();
    try {
      const baseline = await snapshotProtectedRoot(repo, "docs/review-results");
      // Untracked additions are part of the snapshot.
      await Deno.writeTextFile(`${repo}/docs/review-results/999-untracked.json`, "untracked\n");
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      await Deno.remove(`${repo}/docs/review-results/999-untracked.json`);
      assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      // A rename changes the relative path even when the bytes are preserved.
      await Deno.rename(
        `${repo}/docs/review-results/000-base.json`,
        `${repo}/docs/review-results/000-renamed.json`,
      );
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      await Deno.rename(
        `${repo}/docs/review-results/000-renamed.json`,
        `${repo}/docs/review-results/000-base.json`,
      );
      assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      // Deletion of a nested entry is detected.
      await Deno.remove(`${repo}/docs/review-results/nested/deep.txt`);
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
    } finally {
      await Deno.remove(repo, { recursive: true });
    }
  },
});

Deno.test({
  name: "protected directory snapshots record symlink targets without following links",
  ignore: protectedSnapshotTestsIgnored,
  fn: async () => {
    const repo = await createProtectedSnapshotFixture();
    try {
      const baseline = await snapshotProtectedRoot(repo, "docs/review-results");
      await Deno.symlink("000-base.json", `${repo}/docs/review-results/alias.json`);
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), baseline);
      const withAlias = await snapshotProtectedRoot(repo, "docs/review-results");
      // Retargeting the symlink changes the snapshot.
      await Deno.remove(`${repo}/docs/review-results/alias.json`);
      await Deno.symlink("nested/deep.txt", `${repo}/docs/review-results/alias.json`);
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), withAlias);
      const retargeted = await snapshotProtectedRoot(repo, "docs/review-results");
      // Replacing a regular file with a directory at the same nested path
      // changes the recorded type.
      await Deno.remove(`${repo}/docs/review-results/000-base.json`);
      await Deno.mkdir(`${repo}/docs/review-results/000-base.json`, { recursive: true });
      await Deno.writeTextFile(`${repo}/docs/review-results/000-base.json/inner.txt`, "inner\n");
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), retargeted);
      await Deno.remove(`${repo}/docs/review-results/000-base.json`, { recursive: true });
      await Deno.writeTextFile(`${repo}/docs/review-results/000-base.json`, "base\n");
      assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), retargeted);
      // A symlink to a file outside the protected root is recorded by target
      // only: content changes beyond the root must not change the snapshot.
      await Deno.writeTextFile(`${repo}/docs/external.txt`, "external\n");
      await Deno.symlink("../external.txt", `${repo}/docs/review-results/external-link`);
      const withExternal = await snapshotProtectedRoot(repo, "docs/review-results");
      assert.notEqual(withExternal, withAlias);
      await Deno.writeTextFile(`${repo}/docs/external.txt`, "external changed\n");
      assert.equal(await snapshotProtectedRoot(repo, "docs/review-results"), withExternal);
      // Retargeting the link to another outside file is a snapshot change.
      await Deno.writeTextFile(`${repo}/docs/external-two.txt`, "second\n");
      await Deno.remove(`${repo}/docs/review-results/external-link`);
      await Deno.symlink("../external-two.txt", `${repo}/docs/review-results/external-link`);
      assert.notEqual(await snapshotProtectedRoot(repo, "docs/review-results"), withExternal);
      await Deno.remove(`${repo}/docs/review-results/external-link`);
      // Symlink cycles are recorded as entries, never followed recursively.
      await Deno.symlink("loop-b", `${repo}/docs/review-results/loop-a`);
      await Deno.symlink("loop-a", `${repo}/docs/review-results/loop-b`);
      const cyclic = await snapshotProtectedRoot(repo, "docs/review-results");
      assert.match(cyclic, /^dir:sha256:[0-9a-f]{64}$/u);
      assert.notEqual(cyclic, withAlias);
      // A root symlink cannot be verified without following it and is rejected.
      await Deno.symlink("docs/review-results", `${repo}/review-link`);
      await assert.rejects(
        hashProtectedFiles(repo, ["review-link"]),
        /review-link is a symlink and cannot be verified without following it/u,
      );
    } finally {
      await Deno.remove(repo, { recursive: true });
    }
  },
});

Deno.test({
  name: "protected path hashing throws for missing, unreadable, and unsupported roots and entries",
  ignore: protectedSnapshotFifoTestsIgnored,
  fn: async () => {
    const repo = await createProtectedSnapshotFixture();
    try {
      await assert.rejects(
        hashProtectedFiles(repo, ["docs/does-not-exist"]),
        /Protected path docs\/does-not-exist is missing/u,
      );
      // A FIFO root cannot be captured and must fail closed.
      await new Deno.Command("mkfifo", { args: [`${repo}/docs/pipe`] }).output();
      await assert.rejects(
        hashProtectedFiles(repo, ["docs/pipe"]),
        /Protected path docs\/pipe has unsupported type fifo/u,
      );
      // An unsupported nested entry fails the whole directory snapshot.
      await new Deno.Command("mkfifo", { args: [`${repo}/docs/review-results/pipe`] }).output();
      await assert.rejects(
        hashProtectedFiles(repo, ["docs/review-results"]),
        /unsupported entry pipe \(fifo\)/u,
      );
      await Deno.remove(`${repo}/docs/review-results/pipe`);
      // An unreadable directory root fails closed instead of snapshotting a
      // marker that could compare equal to a previously captured value.
      await Deno.chmod(`${repo}/docs/review-results`, 0o000);
      try {
        await assert.rejects(
          hashProtectedFiles(repo, ["docs/review-results"]),
          /Protected directory docs\/review-results is unreadable/u,
        );
      } finally {
        await Deno.chmod(`${repo}/docs/review-results`, 0o755);
      }
    } finally {
      await Deno.remove(repo, { recursive: true });
    }
  },
});

Deno.test({
  name: "protected path assertion reports nested changes and deleted protected paths",
  ignore: protectedSnapshotTestsIgnored,
  fn: async () => {
    const repo = await createProtectedSnapshotFixture();
    try {
      const directoryBase = await hashProtectedFiles(repo, ["docs/review-results"]);
      await Deno.writeTextFile(`${repo}/docs/review-results/nested/deep.txt`, "edited\n");
      await assert.rejects(
        assertProtectedFilesUnchanged(repo, directoryBase),
        /changed protected policy file docs\/review-results/u,
      );
      await Deno.writeTextFile(`${repo}/docs/review-results/nested/deep.txt`, "deep\n");
      // Ordinary file roots preserve Git blob hashing for the change report.
      const fileBase = await hashProtectedFiles(repo, ["docs/review-results/000-base.json"]);
      assert.match(fileBase["docs/review-results/000-base.json"]!, /^[0-9a-f]{40}$/u);
      await Deno.writeTextFile(`${repo}/docs/review-results/000-base.json`, "base edited\n");
      await assert.rejects(
        assertProtectedFilesUnchanged(repo, fileBase),
        /changed protected policy file docs\/review-results\/000-base\.json/u,
      );
      await Deno.writeTextFile(`${repo}/docs/review-results/000-base.json`, "base\n");
      // A deleted protected path fails closed on the missing root itself.
      await Deno.remove(`${repo}/docs/review-results/000-base.json`);
      await assert.rejects(
        assertProtectedFilesUnchanged(repo, fileBase),
        /docs\/review-results\/000-base\.json is missing/u,
      );
    } finally {
      await Deno.remove(repo, { recursive: true });
    }
  },
});
