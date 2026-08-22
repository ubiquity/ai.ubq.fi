import assert from "node:assert/strict";
import {
  isAutonomousMode,
  isSentinelProtectedImplementationPath,
  SENTINEL_POLICY,
} from "../scripts/sentinel/policy.ts";
import {
  agentCheckoutPath,
  assertRetainedReplayArtifactBudget,
  deduplicateRetainedReplayCaptures,
  durableProductionDecision,
  evaluateRollbackPreflight,
  evaluateSentinelTriageGate,
  IMPLEMENTATION_CONTINUATION_MS,
  IMPLEMENTATION_INITIAL_MS,
  implementationPrompt,
  isObserveOnlyMode,
  MAX_MATCHING_REPLAY_ARCHIVE_BYTES,
  MAX_MATCHING_REPLAY_ARTIFACTS,
  MONITOR_AGENT_MS,
  parseIncidentStartMs,
  parseMode,
  parseMonitorDecision,
  parseSentinelDeploymentAttestation,
  previewCompletionForDecision,
  replayIndexArtifactMayMatch,
  replayIndexArtifactName,
  resolveCycleAnchorMs,
  runObserveCycle,
  runWithSingleTimeoutContinuation,
  sentinelDeploymentInputs,
  sentinelEvidenceArtifactName,
  sentinelRevisionControlInputs,
  TRIAGE_INCIDENT_MS,
  triageExpectedMaximumRuntimeMs,
  triagePrompt,
  withStageHeartbeat,
  zeroUnselectedReplayBodies,
} from "../scripts/sentinel/main.ts";
import { CodexInvocationError } from "../scripts/sentinel/codex.ts";
import {
  inspectSse,
  isInferenceOnlyReplayEndpoint,
  replayOneCase,
  selectCurrentAndMatchingRegressionCases,
} from "../scripts/sentinel/replay.ts";
import {
  blockingReviewFindings,
  canStartReviewRound,
  mergeReviewBacklog,
  nativeReviewParseInput,
  parseNativeReview,
} from "../scripts/sentinel/review.ts";
import {
  assertActionableFindingsResolved,
  assertCompleteFindingDispositions,
  IMPLEMENTATION_OUTPUT_SCHEMA,
  type ImplementationReport,
  isTriageReport,
  MONITOR_OUTPUT_SCHEMA,
  type ReplayCase,
  TRIAGE_OUTPUT_SCHEMA,
  type TriageReport,
} from "../scripts/sentinel/types.ts";
import {
  computeSentinelInterval,
  deduplicateEvents,
  eventDedupeKey,
  HOURLY_OVERLAP_MS,
  HOURLY_WINDOW_MS,
  INCIDENT_WINDOW_MS,
  OBSERVE_WINDOW_MS,
} from "../scripts/sentinel/windows.ts";
import type { ExportedSentinelReplayCapture } from "../src/sentinel_replay_capture.ts";

const now = Date.parse("2026-08-21T06:00:00.000Z");

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
      "src/sentinel_replay_capture.ts",
      "tests/sentinel-replay-capture.test.ts",
      ".github/workflows/other.yml",
    ]
  ) assert.equal(isSentinelProtectedImplementationPath(path), true, path);
  assert.equal(isSentinelProtectedImplementationPath("src/openai.ts"), false);
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
});

Deno.test("implementation timeout gets one continuation and never retries another failure", async () => {
  assert.equal(IMPLEMENTATION_INITIAL_MS, 20 * 60_000);
  assert.equal(IMPLEMENTATION_CONTINUATION_MS, 10 * 60_000);
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

Deno.test("observe cycle cannot reach replay, repair, Git, deployment, promotion, or rollback capabilities", async () => {
  const interval = computeSentinelInterval("observe", now);
  const triage: TriageReport = {
    schema_version: 1,
    interval,
    findings: [
      {
        id: "finding-1",
        fingerprint: "0123456789abcdef",
        severity: "P1",
        title: "Actionable provider failure",
        affected_surface: "/v1/responses",
        evidence: [{ source: "deno_log", reference: "line:1", detail: "provider transport failed" }],
        proposed_correction: "Repair the provider transport path.",
        validation_requirements: ["Replay the failed request."],
        actionable: true,
      },
      {
        id: "finding-2",
        fingerprint: "fedcba9876543210",
        severity: "P3",
        title: "Efficiency opportunity",
        affected_surface: "provider catalog",
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

Deno.test("native review parser blocks P0/P1, backlogs P2/P3, and fails closed on unknown output", async () => {
  const parsed = await parseNativeReview(
    `Review findings:\n- [P1] Preserve terminal SSE failure — src/openai.ts:100\n  The stream can close early.\n- [P2] Bound a retry loop — scripts/job.ts:20\n  This can waste one request.`,
    1,
  );
  assert.equal(parsed.parse_status, "findings");
  assert.equal(parsed.findings.length, 2);
  assert.deepEqual(blockingReviewFindings(parsed).map((finding) => finding.severity), ["P1"]);
  assert.equal((await parseNativeReview("No findings.", 1)).parse_status, "no_findings");
  const unknown = await parseNativeReview("Looks reasonable to me.", 1);
  assert.equal(unknown.parse_status, "unparseable");
  assert.throws(() => blockingReviewFindings(unknown), /not parseable/);
});

Deno.test("native review parsing uses the final stdout and normalizes ephemeral checkout paths", async () => {
  const stdout =
    "- [P2] Avoid blocking persistence — /tmp/uos-final/checkout/src/handler.ts:439\n  Return the response first.";
  const stderr = `${stdout}\nCodex progress that must not enter the finding body.`;
  assert.equal(nativeReviewParseInput(stdout, stderr), stdout);

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

Deno.test("native review parser accepts explicit Codex clean verdicts and official location ranges", async () => {
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
      "No actionable defects were found in the replay code. However, the workflow can lose a repair signal under load.",
      1,
    )).parse_status,
    "unparseable",
  );
  assert.equal(
    (await parseNativeReview(
      "No actionable defects were found in the replay code, but the workflow can lose a repair signal under load.",
      1,
    )).parse_status,
    "unparseable",
  );
  assert.equal((await parseNativeReview("The patch looks correct.", 1)).parse_status, "unparseable");
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

Deno.test("review backlog deduplicates fingerprints while retaining first observation and disposition", async () => {
  const report = await parseNativeReview("- [P2] Bound retries — src/retry.ts:9\n  Add a fixed maximum.", 1);
  const finding = report.findings[0]!;
  const firstAt = new Date("2026-08-20T00:00:00.000Z");
  const latestAt = new Date("2026-08-21T00:00:00.000Z");
  const initial = mergeReviewBacklog("", [finding], "a".repeat(40), firstAt).replace(
    /open\s+\|/u,
    "accepted_risk |",
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
  const first = mergeReviewBacklog("", report.findings, "a".repeat(40), observedAt);
  const again = mergeReviewBacklog("", report.findings, "a".repeat(40), observedAt);
  assert.equal(first, again);
  assert.match(first, /never enter\nthis backlog\./u);
  assert.ok([...first].every((character) => character.charCodeAt(0) <= 0x7f));
  assert.match(first, /&#96;Markdown&#96; &#124; safely &#x2014;/u);

  const tableLines = first.split("\n").filter((line) => line.startsWith("|"));
  assert.equal(tableLines.length, 3);
  const separators = [...tableLines[0]!.matchAll(/\|/gu)].map((match) => match.index);
  for (const line of tableLines) {
    assert.deepEqual([...line.matchAll(/\|/gu)].map((match) => match.index), separators);
  }
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
