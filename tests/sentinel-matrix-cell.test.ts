import assert from "node:assert/strict";
import {
  assertMatrixCellReportDigest,
  assertMatrixCellReportV1,
  buildMatrixPlan,
  canonicalMatrixJson,
  type MatrixPlanV1,
} from "../scripts/sentinel/matrix.ts";
import {
  assertSentinelImplementationPolicy,
  CodexInvocationError,
  SENTINEL_AGENT_POLICIES,
} from "../scripts/sentinel/codex.ts";
import {
  buildMatrixCellRetryRecoveryRecord,
  matrixCellRecoverySourceRevision,
  matrixCellRetryEvidenceDirectory,
  runMatrixCell,
  writeMatrixCellRetryEvidence,
} from "../scripts/sentinel/matrix-cell.ts";
import { classifySentinelFailure, stableSentinelFailureFingerprint } from "../scripts/sentinel/retry.ts";

const permissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run" }),
]);
const canRun = permissions.every((permission) => permission.state === "granted");
const decoder = new TextDecoder();
const gitEnvironment = {
  GIT_CONFIG_GLOBAL: "/dev/null",
  GIT_CONFIG_NOSYSTEM: "1",
  GIT_CONFIG_SYSTEM: "/dev/null",
  GIT_TERMINAL_PROMPT: "0",
};

const git = async (cwd: string, args: readonly string[]): Promise<string> => {
  const result = await new Deno.Command("git", {
    args: [...args],
    cwd,
    env: gitEnvironment,
    stdout: "piped",
    stderr: "piped",
  }).output();
  if (!result.success) {
    throw new Error(`git ${args.join(" ")} failed: ${decoder.decode(result.stderr).trim()}`);
  }
  return decoder.decode(result.stdout).trim();
};

const sha256 = async (value: string): Promise<string> => {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value)));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const reportMessage = (changedFiles: readonly string[], status: "implemented" | "already_fixed" = "implemented") =>
  JSON.stringify({
    schema_version: 1,
    dispositions: [{
      finding_id: "fixture",
      status,
      summary: "The scoped fixture repair is complete.",
      changed_files: [...changedFiles],
      validation: ["fixture validation"],
    }],
    replay_acceptances: [],
    candidate_sha: null,
    summary: "The bounded matrix cell completed the fixture repair.",
  });

type Fixture = Readonly<{
  root: string;
  checkout: string;
  baseSha: string;
  plan: MatrixPlanV1;
}>;

const createFixture = async (marker = "matrix cell fixture\n"): Promise<Fixture> => {
  const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-cell-" });
  const checkout = `${root}/checkout`;
  await Deno.mkdir(checkout);
  await git(checkout, ["init", "-b", "development"]);
  await git(checkout, ["config", "user.name", "Matrix Cell Fixture"]);
  await git(checkout, ["config", "user.email", "matrix-cell-fixture@example.invalid"]);
  await Deno.writeTextFile(`${checkout}/README.md`, marker);
  await Deno.mkdir(`${checkout}/src`);
  await Deno.writeTextFile(`${checkout}/src/fix.ts`, "export const base = true;\n");
  await git(checkout, ["add", "README.md", "src/fix.ts"]);
  await git(checkout, ["commit", "--no-gpg-sign", "-m", "fixture base"]);
  const baseSha = await git(checkout, ["rev-parse", "HEAD"]);
  const plan = await buildMatrixPlan({
    run_id: "m02-test",
    run_attempt: 1,
    base_sha: baseSha,
    evidence_digests: [],
    findings: [{
      id: "fixture",
      fingerprint: "1".repeat(64),
      allowed_paths: ["src/extra.ts", "src/fix.ts"],
      validation_requirements: ["fixture validation"],
    }],
  });
  await git(checkout, ["checkout", "-b", plan.cells[0]!.branch]);
  return { root, checkout, baseSha, plan };
};

const cellOptions = (fixture: Fixture, reportPath: string) => ({
  plan: fixture.plan,
  cell: fixture.plan.cells[0]!,
  checkoutPath: fixture.checkout,
  reportPath,
  sensitiveValues: ["fixture-secret-never-log"],
});

const successfulValidation = () =>
  Promise.resolve({
    passed: true,
    checks: [{ name: "fixture", passed: true, detail: "fixture validation passed" }],
  });

Deno.test({
  name: "matrix cell runner commits a scoped repair and emits a complete trusted receipt",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/cell.json`;
    const observedPrompts: string[] = [];
    const scans: string[][] = [];
    try {
      assertSentinelImplementationPolicy();
      assert.equal(SENTINEL_AGENT_POLICIES.implementation.model, "gpt-5.6-luna");
      assert.equal(SENTINEL_AGENT_POLICIES.implementation.reasoningEffort, "max");
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async ({ attempt, prompt }) => {
          assert.equal(attempt, 1);
          observedPrompts.push(prompt);
          await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: async ({ changedPaths }) => {
          scans.push([...changedPaths]);
          await Promise.resolve();
        },
        validate: successfulValidation,
      });

      assert.equal(report.status, "succeeded");
      assert.equal(report.base_sha, fixture.baseSha);
      assert.equal(report.head_sha !== null, true);
      assert.equal(report.tree_sha !== null, true);
      assert.deepEqual(report.changed_paths, ["src/fix.ts"]);
      assert.deepEqual(scans, [["src/fix.ts"], ["src/fix.ts"]]);
      assert.match(observedPrompts[0]!, /gpt-5\.6-luna at max reasoning/u);
      assert.match(observedPrompts[0]!, /allowed paths/u);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
      assert.equal(JSON.parse(await Deno.readTextFile(reportPath)).report_digest, report.report_digest);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD^"]), fixture.baseSha);
      assert.equal(await git(fixture.checkout, ["status", "--porcelain=v1"]), "");
      assert.equal(report.finding_dispositions[0]!.status, "implemented");
      assert.ok(report.artifact_sha256);
      // The initial pass runs focused validation exactly once: its final result
      // is recorded as a terminal focused-validation check, not only after a
      // bounded correction.
      assert.equal(
        report.validation.checks.filter((item) => item.name === "focused-validation").length,
        1,
      );
      assert.equal(
        report.validation.checks.find((item) => item.name === "focused-validation")!.passed,
        true,
      );
      assert.equal(
        report.validation.checks.some((item) => item.name === "focused:fixture" && item.passed === true),
        true,
      );
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner preserves edits for exactly one bounded timeout continuation",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/timeout.json`;
    const attempts: number[] = [];
    let timeoutScanCount = 0;
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        initialTimeoutMs: 25,
        continuationTimeoutMs: 50,
      }, {
        invokeAgent: async ({ attempt, prompt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, 'export const fixed = "partial";\n');
            throw new CodexInvocationError("invocation_timeout");
          }
          assert.match(prompt, /first bounded implementation-cell invocation timed out/u);
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => {
          timeoutScanCount += 1;
          return Promise.resolve();
        },
        validate: successfulValidation,
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(timeoutScanCount, 3);
      assert.equal(report.status, "succeeded");
      assert.equal(report.validation.checks.some((item) => item.name === "implementation-timeout-checkpoint"), true);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner blocks a path outside the immutable cell contract",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/scope.json`;
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async () => {
          await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
          await Deno.writeTextFile(`${fixture.checkout}/src/apix.ts`, "export const outside = true;\n");
          return { lastMessage: reportMessage(["src/apix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /outside the cell contract/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, []);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner blocks protected changes and retains the candidate evidence",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/protected.json`;
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        protectedPaths: ["src/fix.ts"],
        invokeAgent: async () => {
          await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /protected path/u);
      assert.deepEqual(report.changed_paths, ["src/fix.ts"]);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.equal(await git(fixture.checkout, ["status", "--porcelain=v1"]), "M src/fix.ts");
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner fails validation without creating a trusted commit",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/validation.json`;
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async () => {
          await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: () =>
          Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          }),
      });

      assert.equal(report.status, "failed");
      assert.match(report.failure_reason!, /Focused cell validation failed/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, ["src/fix.ts"]);
      assert.equal(report.finding_dispositions[0]!.status, "blocked");
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name:
    "matrix cell runner spends the unused second invocation on one bounded validation repair and commits the corrected aggregate",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair.json`;
    const attempts: number[] = [];
    const validationRuns: string[][] = [];
    const repairPrompts: string[] = [];
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        initialTimeoutMs: 25,
        continuationTimeoutMs: 50,
      }, {
        invokeAgent: async ({ attempt, prompt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            return { lastMessage: reportMessage(["src/fix.ts"]) };
          }
          repairPrompts.push(prompt);
          assert.match(prompt, /Cell contract/u);
          assert.match(prompt, /UNTRUSTED DATA/u);
          // The repair suffix names the exact pre-correction aggregate: only
          // src/fix.ts existed when the first attempt ended, so the repair is
          // told the aggregate exactly, not merely a permitted-path mention.
          assert.match(
            prompt,
            /Current aggregate changed paths \(repository-relative\): \["src\/fix\.ts"\]/u,
          );
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
          return { lastMessage: reportMessage(["src/extra.ts", "src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: async ({ changedPaths }) => {
          validationRuns.push([...changedPaths]);
          const candidate = await Deno.readTextFile(`${fixture.checkout}/src/fix.ts`);
          if (candidate.includes("fixed = true")) return successfulValidation();
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed: export shape differs" }],
          });
        },
      });

      assert.equal(report.status, "succeeded");
      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 2);
      assert.deepEqual(validationRuns[0], ["src/fix.ts"]);
      assert.deepEqual(validationRuns[1], ["src/extra.ts", "src/fix.ts"]);
      assert.equal(repairPrompts.length, 1);
      assert.equal(repairPrompts[0]!.includes("fixture-secret-never-log"), false);
      assert.deepEqual(report.changed_paths, ["src/extra.ts", "src/fix.ts"]);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD^"]), fixture.baseSha);
      assert.equal(await git(fixture.checkout, ["status", "--porcelain=v1"]), "");
      const trigger = report.validation.checks.find((item) => item.name === "validation-repair-trigger");
      assert.ok(trigger);
      assert.equal(trigger.passed, true);
      assert.match(trigger.detail, /initial focused validation failed/u);
      assert.equal(
        report.validation.checks.filter((item) => item.name === "focused-validation").length,
        1,
      );
      assert.equal(
        report.validation.checks.find((item) => item.name === "focused-validation")!.passed,
        true,
      );
      // The bounded repair passes the same evidence gates as the initial
      // attempt: integrity, exact aggregate path scope, protected paths,
      // secret scan, and terminal finding coverage are all recorded as passed.
      const repairChecks = Object.fromEntries(
        [
          "repair-agent-integrity",
          "repair-path-scope",
          "repair-protected-paths",
          "repair-secret-scan",
          "repair-finding-coverage",
        ]
          .map((name) => {
            const item = report.validation.checks.find((candidate) => candidate.name === name);
            assert.ok(item, `the repair evidence check ${name} must be recorded`);
            return [name, item];
          }),
      );
      for (const item of Object.values(repairChecks)) assert.equal(item.passed, true);
      // The repair-path-scope check carries the exact serialized sorted
      // aggregate: the repair added src/extra.ts to the committed src/fix.ts.
      assert.equal(
        repairChecks["repair-path-scope"]!.detail.includes('["src/extra.ts","src/fix.ts"]'),
        true,
      );
      assert.match(
        repairChecks["repair-agent-integrity"]!.detail,
        /remained unchanged after the bounded validation repair/u,
      );
      assert.match(
        repairChecks["repair-protected-paths"]!.detail,
        /protected Sentinel paths remain unchanged after the bounded validation repair/u,
      );
      assert.match(
        repairChecks["repair-secret-scan"]!.detail,
        /repaired cell files and reachable history contain no trusted credential values/u,
      );
      assert.match(
        repairChecks["repair-finding-coverage"]!.detail,
        /have terminal resolved dispositions after the bounded validation repair/u,
      );
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner keeps a repeated focused validation failure failed without a commit or a third invocation",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-failed.json`;
    const attempts: number[] = [];
    const validationRuns: string[][] = [];
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async ({ attempt }) => {
          attempts.push(attempt);
          await Deno.writeTextFile(
            `${fixture.checkout}/src/fix.ts`,
            attempt === 1 ? "export const fixed = 1;\n" : "export const fixed = 2;\n",
          );
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: ({ changedPaths }) => {
          validationRuns.push([...changedPaths]);
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          });
        },
      });

      assert.equal(report.status, "failed");
      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 2);
      assert.match(report.failure_reason!, /Focused cell validation failed/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, ["src/fix.ts"]);
      assert.equal(report.finding_dispositions[0]!.status, "blocked");
      assert.equal(
        report.validation.checks.some(
          (item) => item.name === "validation-repair-trigger" && item.passed === true,
        ),
        true,
      );
      assert.equal(
        report.validation.checks.some(
          (item) => item.name === "focused-validation" && item.passed === false,
        ),
        true,
      );
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner never triggers a third invocation when the timeout continuation consumed the second call",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-spent.json`;
    const attempts: number[] = [];
    const validationRuns: number[] = [];
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        initialTimeoutMs: 25,
        continuationTimeoutMs: 50,
      }, {
        invokeAgent: async ({ attempt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            throw new CodexInvocationError("invocation_timeout");
          }
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: () => {
          validationRuns.push(1);
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          });
        },
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 1);
      assert.equal(report.status, "failed");
      assert.match(report.failure_reason!, /Focused cell validation failed/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.equal(
        report.validation.checks.some((item) => item.name === "validation-repair-trigger"),
        false,
      );
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner keeps a timed-out validation repair retry_pending without a third invocation",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-timeout.json`;
    const attempts: number[] = [];
    const validationRuns: number[] = [];
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: async ({ attempt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            return { lastMessage: reportMessage(["src/fix.ts"]) };
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: () => {
          validationRuns.push(1);
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          });
        },
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 1);
      assert.equal(report.status, "retry_pending");
      assert.equal(report.head_sha, null);
      assert.equal(report.tree_sha, null);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.equal(
        report.validation.checks.some(
          (item) => item.name === "validation-repair-trigger" && item.passed === true,
        ),
        true,
      );
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner blocks a validation repair that touches a protected path before revalidation",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-protected.json`;
    const attempts: number[] = [];
    const validationRuns: number[] = [];
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        protectedPaths: ["src/fix.ts"],
        invokeAgent: async ({ attempt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
            return { lastMessage: reportMessage(["src/extra.ts"]) };
          }
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const changed = true;\n");
          return { lastMessage: reportMessage(["src/extra.ts", "src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: () => {
          validationRuns.push(1);
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          });
        },
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 1);
      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /protected path/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, ["src/extra.ts", "src/fix.ts"]);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name:
    "matrix cell runner blocks a validation repair that changes a path outside the cell contract before revalidation",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-scope.json`;
    const attempts: number[] = [];
    const validationRuns: number[] = [];
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async ({ attempt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            return { lastMessage: reportMessage(["src/fix.ts"]) };
          }
          await Deno.writeTextFile(`${fixture.checkout}/src/outside.ts`, "export const outside = true;\n");
          return { lastMessage: reportMessage(["src/outside.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: () => {
          validationRuns.push(1);
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          });
        },
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 1);
      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /outside the cell contract/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, []);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name:
    "matrix cell runner blocks a validation repair whose changed_files omit a still-changed permitted path before revalidation",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-aggregate.json`;
    const attempts: number[] = [];
    const validationRuns: number[] = [];
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async ({ attempt }) => {
          attempts.push(attempt);
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            return { lastMessage: reportMessage(["src/fix.ts"]) };
          }
          await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
          // The repair report omits src/fix.ts, which remains changed in the
          // aggregate: a missing permitted path is still a changed_files
          // mismatch and must block before any revalidation.
          return { lastMessage: reportMessage(["src/extra.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: () => {
          validationRuns.push(1);
          return Promise.resolve({
            passed: false,
            checks: [{ name: "fixture", passed: false, detail: "fixture validation failed" }],
          });
        },
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(validationRuns.length, 1);
      assert.equal(report.status, "failed");
      assert.match(report.failure_reason!, /did not attribute every changed path to a finding/u);
      assert.equal(report.finding_dispositions[0]!.status, "blocked");
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, ["src/extra.ts", "src/fix.ts"]);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner excludes sensitive values from validation repair feedback and labels it untrusted data",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-secret.json`;
    const repairPrompts: string[] = [];
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async ({ attempt, prompt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            return { lastMessage: reportMessage(["src/fix.ts"]) };
          }
          repairPrompts.push(prompt);
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: async () => {
          const candidate = await Deno.readTextFile(`${fixture.checkout}/src/fix.ts`);
          if (candidate.includes("fixed = true")) return successfulValidation();
          return Promise.resolve({
            passed: false,
            checks: [{
              name: "fixture",
              passed: false,
              detail: "fixture validation failed: fixture-secret-never-log leaked",
            }],
          });
        },
      });

      assert.equal(report.status, "succeeded");
      assert.equal(repairPrompts.length, 1);
      assert.match(repairPrompts[0]!, /UNTRUSTED DATA/u);
      assert.equal(repairPrompts[0]!.includes("fixture-secret-never-log"), false);
      assert.match(repairPrompts[0]!, /\[REDACTED\]/u);
      const trigger = report.validation.checks.find((item) => item.name === "validation-repair-trigger");
      assert.ok(trigger);
      assert.equal(trigger.passed, true);
      assert.equal(trigger.detail.includes("fixture-secret-never-log"), false);
      assert.equal(JSON.stringify(report).includes("fixture-secret-never-log"), false);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner redacts a secret crossing the report-text cutoff without leaking a partial prefix",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/repair-cutoff.json`;
    const repairPrompts: string[] = [];
    const secret = "fixture-secret-never-log";
    const detailPrefix = "x".repeat(4_084);
    const detail = `${detailPrefix}${secret}${"y".repeat(200)}`;
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async ({ attempt, prompt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
            return { lastMessage: reportMessage(["src/fix.ts"]) };
          }
          repairPrompts.push(prompt);
          await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        secretScan: () => Promise.resolve(),
        validate: async () => {
          const candidate = await Deno.readTextFile(`${fixture.checkout}/src/fix.ts`);
          if (candidate.includes("fixed = true")) return successfulValidation();
          // The secret starts before the 4 096-character report-text cutoff and
          // extends past it: truncation alone would keep a partial prefix.
          return Promise.resolve({
            passed: false,
            checks: [{
              name: "fixture",
              passed: false,
              detail,
            }],
          });
        },
      });

      assert.equal(report.status, "succeeded");
      assert.equal(repairPrompts.length, 1);
      assert.equal(repairPrompts[0]!.includes(secret), false);
      assert.equal(repairPrompts[0]!.includes(secret.slice(0, 12)), false);
      assert.match(repairPrompts[0]!, /\[REDACTED\]/u);
      assert.equal(JSON.stringify(report).includes(secret), false);
      assert.equal(JSON.stringify(report).includes(secret.slice(0, 12)), false);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell runner blocks credential material before the trusted commit",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/secret.json`;
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async () => {
          await Deno.mkdir(`${fixture.checkout}/src`, { recursive: true });
          await Deno.writeTextFile(
            `${fixture.checkout}/src/fix.ts`,
            'export const leaked = "fixture-secret-never-log";\n',
          );
          return { lastMessage: reportMessage(["src/fix.ts"]) };
        },
        validate: successfulValidation,
      });

      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /Credential material/u);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.deepEqual(report.changed_paths, ["src/fix.ts"]);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry_pending matrix cell persists durable retry identity and recoverable candidate evidence",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: async ({ attempt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
          } else {
            await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
            await Deno.remove(`${fixture.checkout}/src/fix.ts`);
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      const fixtureSecret = "fixture-secret-never-log";
      assert.equal(report.status, "retry_pending");
      assert.equal(report.head_sha, null);
      assert.equal(report.tree_sha, null);
      assert.deepEqual(report.changed_paths, ["src/extra.ts", "src/fix.ts"]);
      assert.equal(report.finding_dispositions[0]!.status, "blocked");
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      assert.match(await git(fixture.checkout, ["status", "--porcelain=v1"]), /src\/fix\.ts/u);
      assertMatrixCellReportV1(report, fixture.plan);
      await assertMatrixCellReportDigest(report);

      const record = JSON.parse(await Deno.readTextFile(`${evidenceDirectory}/recovery-record.json`));
      const cell = fixture.plan.cells[0]!;
      assert.equal(record.schema_version, 1);
      assert.equal(record.identity.repository, "ubiquity/ai.ubq.fi");
      assert.equal(record.identity.source_kind, "triage");
      assert.equal(record.identity.source_id, cell.cell_id);
      assert.equal(record.identity.candidate_generation, 1);
      assert.equal(
        record.identity.source_revision,
        await matrixCellRecoverySourceRevision(fixture.baseSha, cell, null),
      );
      assert.equal(record.run_id, fixture.plan.run_id);
      assert.equal(record.attempt, fixture.plan.run_attempt);
      assert.equal(record.base_sha, fixture.baseSha);
      assert.equal(record.phase, "recovery_pending");
      assert.equal(record.disposition, "active");
      assert.equal(record.state_version, 1);
      assert.deepEqual(record.changed_files, ["src/extra.ts", "src/fix.ts"]);
      assert.equal(record.candidate_sha, null);
      const classification = classifySentinelFailure(new CodexInvocationError("invocation_timeout"), {
        phase: "implementation_running",
        code: "invocation_timeout",
        signature: report.failure_reason,
      });
      assert.equal(record.failure_class, classification.failure_class);
      assert.equal(classification.failure_class, "transient_transport");
      assert.equal(
        record.failure_fingerprint,
        await stableSentinelFailureFingerprint({
          identity: record.identity,
          failure_class: classification.failure_class,
          code: classification.code,
          phase: classification.phase,
          signature: classification.signature,
        }),
      );
      assert.equal(JSON.stringify(record).includes(fixtureSecret), false);

      const manifest = JSON.parse(await Deno.readTextFile(`${evidenceDirectory}/manifest.json`));
      assert.equal(manifest.schema_version, 1);
      assert.equal(manifest.base_sha, fixture.baseSha);
      assert.equal(manifest.plan_digest, fixture.plan.manifest_digest);
      assert.equal(manifest.run_id, fixture.plan.run_id);
      assert.equal(manifest.run_attempt, fixture.plan.run_attempt);
      assert.equal(manifest.file_count, 2);
      assert.equal(JSON.stringify(manifest.cell_contract), JSON.stringify(JSON.parse(canonicalMatrixJson(cell))));
      const expectedContent = "export const extra = true;\n";
      assert.equal(manifest.total_bytes, expectedContent.length);
      assert.deepEqual(
        Object.keys(manifest.files[0]!).sort(),
        [
          "kind",
          "mode",
          "path",
          "payload",
          "sha256",
          "size",
          "source",
        ].sort(),
      );
      assert.equal(manifest.files[0].path, "src/extra.ts");
      assert.equal(manifest.files[0].source, "untracked");
      assert.equal(manifest.files[0].kind, "file");
      assert.equal(manifest.files[0].payload, "files/0000.bin");
      assert.equal(manifest.files[0].size, expectedContent.length);
      assert.equal(manifest.files[0].sha256, await sha256(expectedContent));
      assert.deepEqual(manifest.files[1], { kind: "deleted", path: "src/fix.ts", source: "tracked" });
      assert.equal(await Deno.readTextFile(`${evidenceDirectory}/files/0000.bin`), expectedContent);
      assert.equal(JSON.stringify(manifest).includes(fixtureSecret), false);
      // The capture attestation documents the trusted cell-capture gates; with
      // no scan report available the digest is null but the status is binding.
      assert.deepEqual(manifest.capture_attestation, {
        schema_version: 1,
        cell_status: "retry_pending",
        secret_scan_path: null,
        secret_scan_sha256: null,
      });
      // Evidence is written atomically: no staging sibling survives a complete run.
      await assert.rejects(Deno.stat(`${evidenceDirectory}.staging`), Deno.errors.NotFound);
      await assert.rejects(Deno.stat(`${evidenceDirectory}/work-selection.json`), Deno.errors.NotFound);
      await assert.rejects(Deno.stat(`${evidenceDirectory}/cell.json`), Deno.errors.NotFound);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry_pending matrix cell without edits persists identity but no candidate snapshot",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry-empty.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: () => {
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      assert.equal(report.status, "retry_pending");
      assert.deepEqual(report.changed_paths, []);
      assert.equal(await git(fixture.checkout, ["rev-parse", "HEAD"]), fixture.baseSha);
      const record = JSON.parse(await Deno.readTextFile(`${evidenceDirectory}/recovery-record.json`));
      assert.deepEqual(record.changed_files, []);
      assert.equal(record.phase, "recovery_pending");
      const manifest = JSON.parse(await Deno.readTextFile(`${evidenceDirectory}/manifest.json`));
      assert.equal(manifest.file_count, 0);
      assert.deepEqual(manifest.files, []);
      assert.equal(manifest.total_bytes, 0);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry_pending matrix cell never captures paths outside the immutable cell contract",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry-blocked-scope.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: async ({ attempt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = true;\n");
          } else {
            await Deno.remove(`${fixture.checkout}/README.md`);
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      // A post-failure integrity violation fails closed: the cell is blocked
      // and no evidence directory can ever become an uploadable artifact.
      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /outside the cell contract/u);
      assert.deepEqual(report.changed_paths, []);
      await assert.rejects(Deno.stat(evidenceDirectory), Deno.errors.NotFound);
      await assert.rejects(Deno.stat(`${evidenceDirectory}.staging`), Deno.errors.NotFound);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry evidence identity preserves the authoritative issue work selection and binds the development base",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const otherFixture = await createFixture("other matrix cell fixture base\n");
    const reportPath = `${fixture.root}/reports/retry-issue.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    const workSelection = {
      source_kind: "github_issue" as const,
      source_id: "208",
      source_revision: "a".repeat(64),
    };
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        workSelection,
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: async ({ attempt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
          } else {
            await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      assert.equal(report.status, "retry_pending");
      const record = JSON.parse(await Deno.readTextFile(`${evidenceDirectory}/recovery-record.json`));
      const cell = fixture.plan.cells[0]!;
      assert.equal(record.identity.source_kind, "github_issue");
      assert.equal(record.identity.source_id, "208");
      assert.equal(
        record.identity.source_revision,
        await matrixCellRecoverySourceRevision(fixture.baseSha, cell, workSelection),
      );
      assert.notEqual(
        record.identity.source_revision,
        await matrixCellRecoverySourceRevision(otherFixture.baseSha, cell, workSelection),
      );
      const persistedSelection = JSON.parse(
        await Deno.readTextFile(`${evidenceDirectory}/work-selection.json`),
      );
      assert.deepEqual(persistedSelection, { schema_version: 1, ...workSelection });
      assert.equal(record.run_id, fixture.plan.run_id);
      assert.equal(record.base_sha, fixture.baseSha);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
      await Deno.remove(otherFixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry_pending matrix cell fails closed without evidence when credentials leak after failure",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry-secret.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: async ({ attempt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const fixed = 1;\n");
          } else {
            await Deno.writeTextFile(
              `${fixture.checkout}/src/extra.ts`,
              'export const leaked = "fixture-secret-never-log";\n',
            );
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        validate: successfulValidation,
      });

      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /Credential material/u);
      assert.deepEqual(report.changed_paths, ["src/extra.ts", "src/fix.ts"]);
      await assert.rejects(Deno.stat(evidenceDirectory), Deno.errors.NotFound);
      await assert.rejects(Deno.stat(`${evidenceDirectory}.staging`), Deno.errors.NotFound);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry_pending matrix cell fails closed without evidence when a protected path changed after failure",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry-protected.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        protectedPaths: ["src/fix.ts"],
        invokeAgent: async ({ attempt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
          } else {
            await Deno.writeTextFile(`${fixture.checkout}/src/fix.ts`, "export const changed = true;\n");
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });

      assert.equal(report.status, "blocked");
      assert.match(report.failure_reason!, /protected path/u);
      assert.deepEqual(report.changed_paths, ["src/extra.ts", "src/fix.ts"]);
      await assert.rejects(Deno.stat(evidenceDirectory), Deno.errors.NotFound);
      await assert.rejects(Deno.stat(`${evidenceDirectory}.staging`), Deno.errors.NotFound);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry evidence replacement never deletes a complete prior set before the new one is durable",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry-safe.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    const changedEntries = [{ path: "src/extra.ts", source: "untracked" as const }];
    await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
    const record = await buildMatrixCellRetryRecoveryRecord({
      repository: "ubiquity/ai.ubq.fi",
      plan: fixture.plan,
      cell: fixture.plan.cells[0]!,
      workSelection: null,
      changedPaths: ["src/extra.ts"],
      error: new CodexInvocationError("invocation_timeout"),
      failureCode: "invocation_timeout",
      failureReason: null,
      now: "2026-08-28T19:00:00.000Z",
    });
    assert(record);
    const write = (recoveryRecord: typeof record) =>
      writeMatrixCellRetryEvidence({
        checkoutPath: fixture.checkout,
        plan: fixture.plan,
        cell: fixture.plan.cells[0]!,
        changedEntries,
        evidenceDirectory,
        capturedAt: recoveryRecord.updated_at,
        recoveryRecord,
        workSelection: null,
      });
    try {
      await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
      await write(record);
      const firstManifest = await Deno.readTextFile(`${evidenceDirectory}/manifest.json`);
      await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = second;\n");
      const secondRecord = await buildMatrixCellRetryRecoveryRecord({
        repository: "ubiquity/ai.ubq.fi",
        plan: fixture.plan,
        cell: fixture.plan.cells[0]!,
        workSelection: null,
        changedPaths: ["src/extra.ts"],
        error: new CodexInvocationError("invocation_timeout"),
        failureCode: "invocation_timeout",
        failureReason: null,
        now: "2026-08-28T19:01:00.000Z",
      });
      assert(secondRecord);
      // A replacement that fails during staged validation never touches the
      // complete prior evidence set.
      await assert.rejects(
        writeMatrixCellRetryEvidence({
          checkoutPath: fixture.checkout,
          plan: fixture.plan,
          cell: fixture.plan.cells[0]!,
          changedEntries,
          evidenceDirectory,
          capturedAt: "2026-08-28T19:02:00.000Z",
          recoveryRecord: {
            ...secondRecord,
            identity: { ...secondRecord.identity, source_revision: "" },
          } as never,
          workSelection: null,
        }),
      );
      assert.equal(await Deno.readTextFile(`${evidenceDirectory}/manifest.json`), firstManifest);
      await assert.rejects(Deno.stat(`${evidenceDirectory}.previous`), Deno.errors.NotFound);
      // A fully validated replacement swaps atomically and leaves no debris.
      await write(secondRecord);
      assert.notEqual(await Deno.readTextFile(`${evidenceDirectory}/manifest.json`), firstManifest);
      await assert.rejects(Deno.stat(`${evidenceDirectory}.staging`), Deno.errors.NotFound);
      await assert.rejects(Deno.stat(`${evidenceDirectory}.previous`), Deno.errors.NotFound);
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry evidence attestation binds the exact secret-scan report digest",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/retry-attest.json`;
    const evidenceDirectory = matrixCellRetryEvidenceDirectory(reportPath);
    const scanReportPath = `${fixture.root}/sentinel-cell-secret-scan.json`;
    const scanReport = JSON.stringify({
      gitleaks: { version: "fixture", findings: [] },
    });
    await Deno.writeTextFile(scanReportPath, scanReport);
    try {
      const report = await runMatrixCell({
        ...cellOptions(fixture, reportPath),
        repository: "ubiquity/ai.ubq.fi",
        secretScanReportPath: scanReportPath,
        initialTimeoutMs: 25,
        continuationTimeoutMs: 25,
      }, {
        invokeAgent: async ({ attempt }) => {
          if (attempt === 1) {
            await Deno.writeTextFile(`${fixture.checkout}/src/extra.ts`, "export const extra = true;\n");
          }
          throw new CodexInvocationError("invocation_timeout");
        },
        secretScan: () => Promise.resolve(),
        validate: successfulValidation,
      });
      assert.equal(report.status, "retry_pending");
      const manifest = JSON.parse(await Deno.readTextFile(`${evidenceDirectory}/manifest.json`));
      assert.deepEqual(manifest.capture_attestation, {
        schema_version: 1,
        cell_status: "retry_pending",
        secret_scan_path: "sentinel-cell-secret-scan.json",
        secret_scan_sha256: await sha256(scanReport),
      });
    } finally {
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});
