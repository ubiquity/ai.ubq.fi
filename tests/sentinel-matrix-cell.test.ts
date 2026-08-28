import assert from "node:assert/strict";
import {
  assertMatrixCellReportDigest,
  assertMatrixCellReportV1,
  buildMatrixPlan,
  type MatrixPlanV1,
} from "../scripts/sentinel/matrix.ts";
import {
  assertSentinelImplementationPolicy,
  CodexInvocationError,
  SENTINEL_AGENT_POLICIES,
} from "../scripts/sentinel/codex.ts";
import { runMatrixCell } from "../scripts/sentinel/matrix-cell.ts";

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

const createFixture = async (): Promise<Fixture> => {
  const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-cell-" });
  const checkout = `${root}/checkout`;
  await Deno.mkdir(checkout);
  await git(checkout, ["init", "-b", "development"]);
  await git(checkout, ["config", "user.name", "Matrix Cell Fixture"]);
  await git(checkout, ["config", "user.email", "matrix-cell-fixture@example.invalid"]);
  await Deno.writeTextFile(`${checkout}/README.md`, "matrix cell fixture\n");
  await git(checkout, ["add", "README.md"]);
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
      allowed_paths: ["src/fix.ts"],
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
          await Deno.mkdir(`${fixture.checkout}/src`);
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
            await Deno.mkdir(`${fixture.checkout}/src`);
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
          await Deno.mkdir(`${fixture.checkout}/src`);
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
          await Deno.mkdir(`${fixture.checkout}/src`);
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
      assert.equal(await git(fixture.checkout, ["status", "--porcelain=v1"]), "?? src/");
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
          await Deno.mkdir(`${fixture.checkout}/src`);
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
  name: "matrix cell runner blocks credential material before the trusted commit",
  ignore: !canRun,
  async fn() {
    const fixture = await createFixture();
    const reportPath = `${fixture.root}/reports/secret.json`;
    try {
      const report = await runMatrixCell(cellOptions(fixture, reportPath), {
        invokeAgent: async () => {
          await Deno.mkdir(`${fixture.checkout}/src`);
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
