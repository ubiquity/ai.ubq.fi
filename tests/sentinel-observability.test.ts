import assert from "node:assert/strict";

const workflowPath = ".github/workflows/provider-sentinel.yml";
const summaryStepStart = "      - name: Publish recovery disposition summary";
const runStart = "        run: |\n";

const runScriptFromWorkflow = (workflow: string): string => {
  const stepStart = workflow.indexOf(summaryStepStart);
  assert.notEqual(stepStart, -1, "The recovery summary step is missing");
  const stepEnd = workflow.indexOf("\n      - name:", stepStart + summaryStepStart.length);
  assert.ok(stepEnd > stepStart, "The recovery summary step has no bounded workflow body");
  const step = workflow.slice(stepStart, stepEnd);
  const bodyStart = step.indexOf(runStart);
  assert.notEqual(bodyStart, -1, "The recovery summary step has no shell body");
  return step.slice(bodyStart + runStart.length).split("\n").map((line) => line.slice(10)).join("\n");
};

const fixtures = [
  {
    source: "github_issue/recovered",
    source_revision: "source-recovered",
    disposition: "recovered",
    candidate_sha: "a".repeat(40),
    branch: "sentinel/candidate-recovered",
    failure_fingerprint: "b".repeat(64),
    artifact_expiry: "2026-11-26T00:00:00Z",
    next_action: "Validate the recovered checkpoint.",
  },
  {
    source: "triage/rejected",
    source_revision: "source-rejected",
    disposition: "rejected",
    candidate_sha: null,
    branch: null,
    failure_fingerprint: "c".repeat(64),
    artifact_expiry: "2026-11-27T00:00:00Z",
    next_action: "No automatic action recorded.",
  },
  {
    source: "review_backlog/retrying",
    source_revision: "source-retrying",
    disposition: "retrying",
    phase: "retry_wait",
    candidate_sha: "d".repeat(40),
    branch: "sentinel/candidate-retrying",
    failure_fingerprint: "e".repeat(64),
    artifact_expiry: "2026-11-28T00:00:00Z",
    next_action: "Retry after 2026-08-28T19:00:00Z.",
  },
  {
    source: "incident/manual",
    source_revision: "source-manual",
    disposition: "manual_required",
    candidate_sha: "f".repeat(40),
    branch: "sentinel/candidate-manual",
    failure_fingerprint: "1".repeat(64),
    artifact_expiry: "2026-11-29T00:00:00Z",
    next_action: "Owner review is required.",
    decrypted_candidate_secret: "must-never-render",
  },
] as const;

const [readPermission, runPermission, writePermission] = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "run" }),
  Deno.permissions.query({ name: "write" }),
]);

Deno.test({
  name: "Provider Sentinel recovery summary has a metadata-only workflow contract",
  ignore: readPermission.state !== "granted",
  async fn() {
    const workflow = await Deno.readTextFile(workflowPath);
    assert.notEqual(workflow.indexOf(summaryStepStart), -1);
    for (
      const field of [
        "source",
        "candidate_sha",
        "candidate_branch",
        "failure_fingerprint",
        "artifact_expiry",
        "next_action",
        "recovery-summary-v1.json",
      ]
    ) {
      assert.match(workflow, new RegExp(field.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&")));
    }
    assert.doesNotMatch(workflow, /github_issue_(?:title|body)/u);
  },
});

Deno.test({
  name: "Provider Sentinel recovery summary renders safe terminal and retry fixtures",
  ignore: readPermission.state !== "granted" || runPermission.state !== "granted" ||
    writePermission.state !== "granted",
  async fn() {
    const workflow = await Deno.readTextFile(workflowPath);
    const script = runScriptFromWorkflow(workflow);

    const root = await Deno.makeTempDir({ prefix: "sentinel-observability-test-" });
    try {
      const reports = `${root}/.sentinel/reports/recovery-records`;
      await Deno.mkdir(reports, { recursive: true });
      await Deno.writeTextFile(
        `${root}/.sentinel/reports/recovery-record-v1.json`,
        JSON.stringify(fixtures[0]),
      );
      for (const fixture of fixtures.slice(1)) {
        await Deno.writeTextFile(`${reports}/${fixture.source.split("/")[1]}.json`, JSON.stringify(fixture));
      }
      const scriptPath = `${root}/recovery-summary.sh`;
      await Deno.writeTextFile(scriptPath, script, { mode: 0o700 });
      const summaryPath = `${root}/summary.md`;
      const result = await new Deno.Command("/bin/bash", {
        args: [scriptPath],
        cwd: root,
        env: {
          GITHUB_REPOSITORY: "",
          GITHUB_RUN_ID: "1",
          GITHUB_STEP_SUMMARY: summaryPath,
          PATH: "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
          RUNNER_TEMP: root,
        },
        stdout: "piped",
        stderr: "piped",
      }).output();
      const decoder = new TextDecoder();
      assert.equal(
        result.code,
        0,
        `${decoder.decode(result.stdout)}\n${decoder.decode(result.stderr)}`,
      );
      const summary = await Deno.readTextFile(summaryPath);
      const report = JSON.parse(await Deno.readTextFile(`${root}/.sentinel/reports/recovery-summary-v1.json`)) as {
        counts: Record<string, number>;
        records: readonly Record<string, string>[];
      };
      assert.deepEqual(report.counts, {
        recovered: 1,
        pending: 1,
        retrying: 1,
        rejected: 1,
        manual_required: 1,
      });
      assert.equal(report.records.length, fixtures.length);
      for (const fixture of fixtures) {
        assert.match(summary, new RegExp(fixture.source.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&")));
        assert.match(summary, new RegExp(fixture.artifact_expiry.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&")));
        assert.match(summary, new RegExp(fixture.next_action.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&")));
      }
      assert.match(summary, /a{40}/u);
      assert.match(summary, /d{40}/u);
      assert.match(summary, /f{40}/u);
      assert.match(summary, /b{64}/u);
      assert.match(summary, /e{64}/u);
      assert.match(summary, /1{64}/u);
      assert.doesNotMatch(summary, /must-never-render/u);
      assert.doesNotMatch(JSON.stringify(report), /must-never-render/u);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});
