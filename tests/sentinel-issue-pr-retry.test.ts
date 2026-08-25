import assert from "node:assert/strict";
import { matchingIssueDeliveryPullRequests, type PullRequest } from "../scripts/sentinel/issue-pr-pre-push.ts";

const marker = "<!-- provider-sentinel:issue-pr:v1 issue=112 fingerprint=" + "a".repeat(64) + " -->";

const pull = (overrides: Partial<PullRequest>): PullRequest => ({
  number: 1,
  htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/1",
  state: "open",
  mergedAt: null,
  body: marker,
  headRef: "sentinel/candidate-new",
  headSha: "b".repeat(40),
  baseRef: "development",
  ...overrides,
});

Deno.test("rolled-back issue retry ignores a prior delivery for the same immutable fingerprint", () => {
  const oldMergedAttempt = pull({
    number: 10,
    state: "closed",
    mergedAt: "2026-08-25T12:00:00Z",
    headRef: "sentinel/candidate-old",
    headSha: "c".repeat(40),
  });
  const currentAttempt = pull({ number: 11 });

  assert.deepEqual(
    matchingIssueDeliveryPullRequests(
      [oldMergedAttempt, currentAttempt],
      marker,
      "b".repeat(40),
      "sentinel/candidate-new",
    ).map((candidate) => candidate.number),
    [11],
  );
});

Deno.test("same fingerprint without the current candidate identity is eligible for a fresh PR", () => {
  const oldMergedAttempt = pull({
    number: 10,
    state: "closed",
    mergedAt: "2026-08-25T12:00:00Z",
    headRef: "sentinel/candidate-old",
    headSha: "c".repeat(40),
  });

  assert.deepEqual(
    matchingIssueDeliveryPullRequests(
      [oldMergedAttempt],
      marker,
      "b".repeat(40),
      "sentinel/candidate-new",
    ),
    [],
  );
});
