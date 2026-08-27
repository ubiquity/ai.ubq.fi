import assert from "node:assert/strict";

import {
  ensureCandidateWorkflowValidation,
  parseCandidatePreviewEvidence,
  parseCandidateWorkflowValidationRecord,
  parseIssueCandidateDisposition,
} from "../scripts/sentinel/issue-pr-pre-push.ts";
import type { GitHubWorkflowDispatch, GitHubWorkflowRun, WaitForWorkflowOptions } from "../scripts/sentinel/github.ts";

const CANDIDATE_SHA = "b".repeat(40);
const CANDIDATE_BRANCH = "sentinel/candidate-issue-112";
const FINGERPRINT = "a".repeat(64);
const CORRELATION_ID = "sentinel-123e4567-e89b-42d3-a456-426614174000";
const DISPLAY_TITLE = `Deno Deploy ${CORRELATION_ID}`;

const successfulRun = (id: number, overrides: Partial<GitHubWorkflowRun> = {}): GitHubWorkflowRun => ({
  id,
  headSha: CANDIDATE_SHA,
  status: "completed",
  conclusion: "success",
  htmlUrl: `https://github.com/ubiquity/ai.ubq.fi/actions/runs/${id}`,
  createdAt: "2026-08-27T12:00:00Z",
  displayTitle: DISPLAY_TITLE,
  ...overrides,
});

const dispatch = (runId: number): GitHubWorkflowDispatch => ({
  runId,
  runUrl: `https://api.github.com/repos/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
  htmlUrl: `https://github.com/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
});

Deno.test("issue candidate evidence parsers bind disposition, preview, and retry evidence to exact identities", () => {
  assert.deepEqual(
    parseCandidatePreviewEvidence({
      git_sha: CANDIDATE_SHA,
      revision: "preview-revision-112",
      workflow_run_id: 71,
      replay_base_url: "https://preview.example.test",
    }, CANDIDATE_SHA),
    {
      gitSha: CANDIDATE_SHA,
      revision: "preview-revision-112",
      workflowRunId: 71,
    },
  );
  assert.throws(
    () =>
      parseCandidatePreviewEvidence({
        git_sha: "c".repeat(40),
        revision: "preview-revision-112",
        workflow_run_id: 71,
      }, CANDIDATE_SHA),
    /does not match the exact issue candidate/,
  );

  assert.equal(
    parseIssueCandidateDisposition({
      schema_version: 1,
      issue_number: 112,
      fingerprint: FINGERPRINT,
      disposition: "resolved",
    }, { issueNumber: 112, fingerprint: FINGERPRINT }),
    "resolved",
  );
  assert.throws(
    () =>
      parseIssueCandidateDisposition({
        schema_version: 1,
        issue_number: 113,
        fingerprint: FINGERPRINT,
        disposition: "resolved",
      }, { issueNumber: 112, fingerprint: FINGERPRINT }),
    /does not match the exact selection/,
  );

  const persisted = {
    schema_version: 1 as const,
    source: "build_only" as const,
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 72,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  };
  assert.deepEqual(
    parseCandidateWorkflowValidationRecord(persisted, {
      candidateSha: CANDIDATE_SHA,
      candidateBranch: CANDIDATE_BRANCH,
    }),
    persisted,
  );
  assert.throws(
    () =>
      parseCandidateWorkflowValidationRecord({ ...persisted, git_sha: "c".repeat(40) }, {
        candidateSha: CANDIDATE_SHA,
        candidateBranch: CANDIDATE_BRANCH,
      }),
    /does not match the exact candidate/,
  );
});

Deno.test("resolved issue candidates reuse and verify the exact successful preview workflow", async () => {
  const waits: WaitForWorkflowOptions[] = [];
  const result = await ensureCandidateWorkflowValidation({
    client: {
      dispatchWorkflow: () => {
        throw new Error("resolved preview evidence must not dispatch another workflow");
      },
      waitForWorkflow: (options) => {
        waits.push(options);
        return Promise.resolve(successfulRun(81));
      },
    },
    candidateSha: CANDIDATE_SHA,
    candidateBranch: CANDIDATE_BRANCH,
    disposition: "resolved",
    preview: { gitSha: CANDIDATE_SHA, revision: "preview-revision-112", workflowRunId: 81 },
    existingBuildValidation: null,
  });

  assert.deepEqual(waits, [{ runId: 81, headSha: CANDIDATE_SHA }]);
  assert.deepEqual(result, {
    schema_version: 1,
    source: "preview",
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 81,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  });
});

Deno.test("resolved issue candidates cannot reach development without preview workflow evidence", async () => {
  await assert.rejects(
    () =>
      ensureCandidateWorkflowValidation({
        client: {
          dispatchWorkflow: () => Promise.resolve(dispatch(82)),
          waitForWorkflow: () => Promise.resolve(successfulRun(82)),
        },
        candidateSha: CANDIDATE_SHA,
        candidateBranch: CANDIDATE_BRANCH,
        disposition: "resolved",
        preview: null,
        existingBuildValidation: null,
      }),
    /require exact preview workflow evidence/,
  );
});

Deno.test("manual-required issue candidates dispatch an exact non-preview build and wait for its success", async () => {
  let dispatched = false;
  let waited = false;
  const result = await ensureCandidateWorkflowValidation({
    client: {
      dispatchWorkflow: (workflow, ref, inputs) => {
        dispatched = true;
        assert.equal(workflow, "deno-deploy.yml");
        assert.equal(ref, CANDIDATE_BRANCH);
        assert.deepEqual(inputs, {
          deploy_preview: false,
          sentinel_build_only: true,
          sentinel_correlation_id: CORRELATION_ID,
        });
        return Promise.resolve(dispatch(91));
      },
      waitForWorkflow: (options) => {
        waited = true;
        assert.deepEqual(options, {
          runId: 91,
          headSha: CANDIDATE_SHA,
          displayTitle: DISPLAY_TITLE,
        });
        return Promise.resolve(successfulRun(91));
      },
    },
    candidateSha: CANDIDATE_SHA,
    candidateBranch: CANDIDATE_BRANCH,
    disposition: "manual_required",
    preview: null,
    existingBuildValidation: null,
    createCorrelationId: () => CORRELATION_ID,
  });

  assert.equal(dispatched, true);
  assert.equal(waited, true);
  assert.deepEqual(result, {
    schema_version: 1,
    source: "build_only",
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 91,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  });
});

Deno.test("manual candidate validation rejects a response for any workflow run other than the dispatched run", async () => {
  await assert.rejects(
    () =>
      ensureCandidateWorkflowValidation({
        client: {
          dispatchWorkflow: () => Promise.resolve(dispatch(101)),
          waitForWorkflow: () => Promise.resolve(successfulRun(102)),
        },
        candidateSha: CANDIDATE_SHA,
        candidateBranch: CANDIDATE_BRANCH,
        disposition: "manual_required",
        preview: null,
        existingBuildValidation: null,
        createCorrelationId: () => CORRELATION_ID,
      }),
    /wrong workflow run ID/,
  );
});

Deno.test("manual candidate validation reuses exact persisted build evidence after a hook retry", async () => {
  const evidence = parseCandidateWorkflowValidationRecord({
    schema_version: 1,
    source: "build_only",
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 111,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  }, { candidateSha: CANDIDATE_SHA, candidateBranch: CANDIDATE_BRANCH });
  let waited = false;
  const result = await ensureCandidateWorkflowValidation({
    client: {
      dispatchWorkflow: () => {
        throw new Error("an exact prior build validation must not be dispatched twice");
      },
      waitForWorkflow: (options) => {
        waited = true;
        assert.deepEqual(options, {
          runId: 111,
          headSha: CANDIDATE_SHA,
          displayTitle: DISPLAY_TITLE,
        });
        return Promise.resolve(successfulRun(111));
      },
    },
    candidateSha: CANDIDATE_SHA,
    candidateBranch: CANDIDATE_BRANCH,
    disposition: "manual_required",
    preview: null,
    existingBuildValidation: evidence,
  });

  assert.equal(waited, true);
  assert.deepEqual(result, evidence);
});
