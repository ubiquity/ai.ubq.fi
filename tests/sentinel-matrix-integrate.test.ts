import assert from "node:assert/strict";
import {
  assertIntegrationCorrectionScope,
  assertMatrixCellReportsDoNotOverlap,
  executeMatrixIntegration,
  type MatrixGitExecutor,
  provePatchEquivalentOursMerge,
  runMatrixIntegrationAgent,
  validateMatrixIntegrationInputs,
  verifyMatrixCellReportHead,
} from "../scripts/sentinel/matrix-integrate.ts";
import {
  assertIntegrationDecisionDigest,
  assertIntegrationDecisionV1,
  buildMatrixPlan,
  integrationDecisionDigest,
  type IntegrationDecisionV1,
  matrixCellReportDigest,
  type MatrixCellReportV1,
  type MatrixPlanV1,
} from "../scripts/sentinel/matrix.ts";
import { runMatrixCell } from "../scripts/sentinel/matrix-cell.ts";
import { parseRollingReviewResult } from "../scripts/sentinel/rolling-review.ts";
import { verifyMatrixConvergenceAdvance } from "../scripts/sentinel/main.ts";

const baseCommit = "1".repeat(40);
const reportDigest = "2".repeat(64);
const artifactDigest = "3".repeat(64);

const runGit = async (
  cwd: string,
  args: readonly string[],
): Promise<{ code: number; stdout: string; stderr: string }> => {
  const output = await new Deno.Command("git", {
    args: [...args],
    cwd,
    stdout: "piped",
    stderr: "piped",
  }).output();
  return {
    code: output.code,
    stdout: new TextDecoder().decode(output.stdout),
    stderr: new TextDecoder().decode(output.stderr),
  };
};

const git = async (cwd: string, args: readonly string[]): Promise<string> => {
  const result = await runGit(cwd, args);
  if (result.code !== 0) throw new Error(result.stderr);
  return result.stdout.trim();
};

const makePlan = async (baseSha: string): Promise<MatrixPlanV1> =>
  await buildMatrixPlan({
    run_id: "m03-test",
    run_attempt: 1,
    base_sha: baseSha,
    evidence_digests: [],
    findings: [
      {
        id: "one",
        fingerprint: "a".repeat(64),
        allowed_paths: ["src/one.ts"],
        validation_requirements: ["validate one"],
      },
      {
        id: "two",
        fingerprint: "b".repeat(64),
        allowed_paths: ["src/two.ts"],
        validation_requirements: ["validate two"],
      },
    ],
  });

const reportFor = async (
  plan: MatrixPlanV1,
  cellIndex: number,
  headSha: string,
  treeSha: string,
): Promise<MatrixCellReportV1> => {
  const cell = plan.cells[cellIndex]!;
  const unsigned = {
    schema_version: 1 as const,
    run_id: plan.run_id,
    run_attempt: plan.run_attempt,
    plan_digest: plan.manifest_digest,
    cell_id: cell.cell_id,
    base_sha: plan.base_sha,
    branch: cell.branch,
    head_sha: headSha,
    tree_sha: treeSha,
    changed_paths: [...cell.allowed_paths],
    finding_dispositions: cell.finding_ids.map((finding_id) => ({
      finding_id,
      fingerprint: plan.ownership.find((finding) => finding.finding_id === finding_id)!.fingerprint,
      status: "implemented" as const,
      summary: "fixed",
      changed_files: [...cell.allowed_paths],
      validation: ["focused"],
    })),
    validation: { passed: true, checks: [{ name: "focused", passed: true, detail: "passed" }] },
    replay: { attempted: false, passed: true, results: [] },
    status: "succeeded" as const,
    failure_reason: null,
    artifact_sha256: artifactDigest,
    report_digest: reportDigest,
  } satisfies MatrixCellReportV1;
  return { ...unsigned, report_digest: await matrixCellReportDigest(unsigned) };
};

const decisionFor = async (
  plan: MatrixPlanV1,
  decisions: IntegrationDecisionV1["decisions"],
  correctionPaths: readonly string[] = [],
): Promise<IntegrationDecisionV1> => {
  const unsigned = {
    schema_version: 1 as const,
    run_id: plan.run_id,
    run_attempt: plan.run_attempt,
    plan_digest: plan.manifest_digest,
    base_sha: plan.base_sha,
    decisions,
    combined_validation_requirements: ["deno check"],
    correction_paths: [...correctionPaths],
    summary: "test decision",
    decision_digest: reportDigest,
  } satisfies IntegrationDecisionV1;
  return { ...unsigned, decision_digest: await integrationDecisionDigest(unsigned) };
};

const permissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run" }),
]);

Deno.test({
  name: "matrix integration validates immutable reports and rejects missing or altered heads",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-integrate-reports-" });
    try {
      await git(root, ["init", "-b", "development"]);
      await git(root, ["config", "user.name", "Sentinel Test"]);
      await git(root, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.mkdir(`${root}/src`, { recursive: true });
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'base';\n");
      await Deno.writeTextFile(`${root}/src/two.ts`, "export const two = 'base';\n");
      await git(root, ["add", "src"]);
      await git(root, ["commit", "-m", "base"]);
      const baseSha = await git(root, ["rev-parse", "HEAD"]);
      const plan = await buildMatrixPlan({
        run_id: "m03-test",
        run_attempt: 1,
        base_sha: baseSha,
        evidence_digests: [],
        findings: [{
          id: "one",
          fingerprint: "a".repeat(64),
          allowed_paths: ["src/one.ts"],
          validation_requirements: ["validate one"],
        }],
      });
      const cell = plan.cells[0]!;
      await git(root, ["switch", "-c", cell.branch]);
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'fixed';\n");
      await git(root, ["add", "src/one.ts"]);
      await git(root, ["commit", "-m", "cell one"]);
      const headSha = await git(root, ["rev-parse", "HEAD"]);
      const treeSha = await git(root, ["rev-parse", "HEAD^{tree}"]);
      const report = await reportFor(plan, plan.cells.indexOf(cell), headSha, treeSha);
      await git(root, ["switch", "-c", "sentinel/integrated-m03", baseSha]);
      const executor: MatrixGitExecutor = (command) => runGit(command.cwd, command.args);
      const validation = await validateMatrixIntegrationInputs({
        plan,
        reports: [report],
        checkoutPath: root,
        git: executor,
      });
      assert.equal(validation.reports_by_cell.get(cell.cell_id)?.head_sha, headSha);
      await assert.rejects(
        () =>
          verifyMatrixCellReportHead({
            checkoutPath: root,
            plan,
            report: { ...report, head_sha: "f".repeat(40) },
            git: executor,
          }),
        /digest|head|branch/u,
      );
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test("matrix integration rejects accepted cell overlap before merge", () => {
  const first = {
    cell_id: "cell-" + "a".repeat(64),
    changed_paths: ["src/shared/file.ts"],
  } as unknown as MatrixCellReportV1;
  const second = {
    cell_id: "cell-" + "b".repeat(64),
    changed_paths: ["src/shared"],
  } as unknown as MatrixCellReportV1;
  assert.throws(() => assertMatrixCellReportsDoNotOverlap([first, second]), /overlap/u);
});

Deno.test({
  name: "matrix convergence integrates cells from the verified effective base while retaining plan base ownership",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-effective-base-" });
    try {
      await git(root, ["init", "-b", "development"]);
      await git(root, ["config", "user.name", "Sentinel Test"]);
      await git(root, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.mkdir(`${root}/src`, { recursive: true });
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'base';\n");
      await git(root, ["add", "src/one.ts"]);
      await git(root, ["commit", "-m", "matrix effective-base plan base"]);
      const planBaseSha = await git(root, ["rev-parse", "HEAD"]);
      const plan = await buildMatrixPlan({
        run_id: "effective-base",
        run_attempt: 1,
        base_sha: planBaseSha,
        evidence_digests: [],
        findings: [{
          id: "one",
          fingerprint: "a".repeat(64),
          allowed_paths: ["src/one.ts"],
          validation_requirements: ["focused"],
        }],
      });
      const cell = plan.cells[0]!;

      await git(root, ["switch", "-c", cell.branch, planBaseSha]);
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'fixed';\n");
      await git(root, ["add", "src/one.ts"]);
      await git(root, ["commit", "-m", "matrix cell repair"]);
      const cellHeadSha = await git(root, ["rev-parse", "HEAD"]);
      const report = await reportFor(plan, 0, cellHeadSha, await git(root, ["rev-parse", "HEAD^{tree}"]));

      await git(root, ["switch", "development"]);
      const reviewHeadSha = "c".repeat(40);
      const reviewFileName = `123-${reviewHeadSha}.json`;
      const reviewResult = {
        schema_version: 1,
        request_id: `123-${reviewHeadSha}`,
        pr_number: 123,
        pr_url: "https://github.com/ubiquity/ai.ubq.fi/pull/123",
        head_sha: reviewHeadSha,
        base_sha: planBaseSha,
        head_branch: "sentinel/candidate-123",
        status: "completed",
        reviewed_at: "2026-08-31T00:00:00.000Z",
        parse_status: "no_findings",
        raw_review_text: "",
        review_stderr: "",
        structured_review: null,
        findings: [],
        failure: null,
      };
      parseRollingReviewResult(reviewResult, { prNumber: 123, headSha: reviewHeadSha, fileName: reviewFileName });
      await Deno.mkdir(`${root}/docs/sentinel-review-results`, { recursive: true });
      await Deno.writeTextFile(
        `${root}/docs/sentinel-review-results/${reviewFileName}`,
        JSON.stringify(reviewResult),
      );
      await git(root, ["add", "docs/sentinel-review-results"]);
      await git(root, ["commit", "-m", "record rolling review result"]);
      const effectiveBaseSha = await git(root, ["rev-parse", "HEAD"]);
      await verifyMatrixConvergenceAdvance(root, planBaseSha, effectiveBaseSha);

      const integrationBranch = "sentinel/integrated-effective-base";
      await git(root, ["switch", "-c", integrationBranch, effectiveBaseSha]);
      const decision = await decisionFor(plan, [{
        cell_id: cell.cell_id,
        decision: "accept",
        reason: "validated",
        required_combined_checks: ["focused"],
        correction_paths: [],
      }]);
      const result = await executeMatrixIntegration({
        plan,
        reports: [report],
        decision,
        effectiveBaseSha,
        checkoutPath: root,
        integrationBranch,
        git: (command) => runGit(command.cwd, command.args),
      });
      const integratedCandidate = result.cycle_report.integrated_candidate;
      assert.ok(integratedCandidate);
      assert.equal(result.cycle_report.base_sha, planBaseSha);
      assert.equal(integratedCandidate.base_sha, effectiveBaseSha);
      assert.equal(result.merge_receipts[0]?.before_head_sha, effectiveBaseSha);
      assert.equal(
        await git(root, ["merge-base", "--is-ancestor", effectiveBaseSha, integratedCandidate.head_sha]).then(() => 0),
        0,
      );
      const mergeParents = (await git(root, ["show", "-s", "--format=%P", integratedCandidate.head_sha])).split(" ");
      assert.equal(mergeParents.length, 2);
      assert.equal(mergeParents[0], effectiveBaseSha);
      assert.equal(mergeParents[1], cellHeadSha);
      assert.equal(await git(root, ["status", "--porcelain"]), "");
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "faithful matrix harness overlaps isolated repair cells before final convergence",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-concurrent-acceptance-" });
    const repository = `${root}/repository`;
    const worktrees = `${root}/worktrees`;
    try {
      await Deno.mkdir(repository);
      await Deno.mkdir(worktrees);
      await Deno.mkdir(`${root}/reports`);
      await git(repository, ["init", "-b", "development"]);
      await git(repository, ["config", "user.name", "Sentinel Acceptance"]);
      await git(repository, ["config", "user.email", "sentinel-acceptance@example.invalid"]);
      await Deno.mkdir(`${repository}/src`);
      await Deno.writeTextFile(`${repository}/src/one.ts`, "export const one = 'base';\n");
      await Deno.writeTextFile(`${repository}/src/two.ts`, "export const two = 'base';\n");
      await git(repository, ["add", "src"]);
      await git(repository, ["commit", "--no-gpg-sign", "-m", "acceptance base"]);
      const baseSha = await git(repository, ["rev-parse", "HEAD"]);
      const plan = await buildMatrixPlan({
        run_id: "m06-concurrent-acceptance",
        run_attempt: 1,
        base_sha: baseSha,
        evidence_digests: [],
        findings: ["one", "two"].map((name, index) => ({
          id: name,
          fingerprint: String.fromCharCode(97 + index).repeat(64),
          allowed_paths: [`src/${name}.ts`],
          validation_requirements: [`validate ${name}`],
        })),
      });
      assert.equal(plan.cells.length, 2);

      const timing = new Map<string, { started_at: string; finished_at: string | null }>();
      let activeCells = 0;
      let maximumActiveCells = 0;
      let arrivals = 0;
      let releaseOverlap!: () => void;
      const overlapBarrier = new Promise<void>((resolve) => {
        releaseOverlap = resolve;
      });
      const reports = await Promise.all(plan.cells.map(async (cell) => {
        const checkout = `${worktrees}/${cell.cell_id}`;
        await git(repository, ["worktree", "add", "-b", cell.branch, checkout, baseSha]);
        const findingId = cell.finding_ids[0]!;
        const changedPath = cell.allowed_paths[0]!;
        return await runMatrixCell({
          plan,
          cell,
          checkoutPath: checkout,
          reportPath: `${root}/reports/${cell.cell_id}.json`,
          findings: [],
          sensitiveValues: [],
        }, {
          invokeAgent: async () => {
            timing.set(cell.cell_id, { started_at: new Date().toISOString(), finished_at: null });
            activeCells += 1;
            maximumActiveCells = Math.max(maximumActiveCells, activeCells);
            arrivals += 1;
            if (arrivals === plan.cells.length) releaseOverlap();
            await overlapBarrier;
            await Deno.writeTextFile(
              `${checkout}/${changedPath}`,
              `export const ${findingId} = 'fixed';\n`,
            );
            activeCells -= 1;
            timing.get(cell.cell_id)!.finished_at = new Date().toISOString();
            return {
              lastMessage: JSON.stringify({
                schema_version: 1,
                dispositions: [{
                  finding_id: findingId,
                  status: "implemented",
                  summary: `Implemented ${findingId}`,
                  changed_files: [changedPath],
                  validation: [`validate ${findingId}`],
                }],
                replay_acceptances: [],
                candidate_sha: null,
                summary: `Completed ${cell.cell_id}`,
              }),
            };
          },
          secretScan: () => Promise.resolve(),
          validate: () =>
            Promise.resolve({
              passed: true,
              checks: [{ name: "focused", passed: true, detail: `validated ${findingId}` }],
            }),
        });
      }));
      assert.equal(maximumActiveCells, 2);
      assert.ok([...timing.values()].every((entry) => entry.finished_at !== null));

      const integrationCheckout = `${worktrees}/integration`;
      await git(repository, [
        "worktree",
        "add",
        "-b",
        "sentinel/integrated-m06-concurrent",
        integrationCheckout,
        baseSha,
      ]);
      const decision = await decisionFor(
        plan,
        plan.cells.map((cell) => ({
          cell_id: cell.cell_id,
          decision: "accept" as const,
          reason: "concurrent cell validated",
          required_combined_checks: ["focused"],
          correction_paths: [],
        })),
      );
      const result = await executeMatrixIntegration({
        plan,
        reports,
        decision,
        checkoutPath: integrationCheckout,
        effectiveBaseSha: baseSha,
        integrationBranch: "sentinel/integrated-m06-concurrent",
        git: (command) => runGit(command.cwd, command.args),
      });
      assert.equal(result.merge_receipts.length, 2);
      assert.equal(result.cycle_report.accepted_ancestry.length, 2);
      assert.ok(result.merge_receipts.every((receipt) => receipt.is_ancestor));
      console.log(JSON.stringify({
        run_id: plan.run_id,
        maximum_active_cells: maximumActiveCells,
        cell_timing: Object.fromEntries(timing),
        integration_head: result.cycle_report.integrated_candidate?.head_sha,
        dispositions: result.cycle_report.cell_dispositions,
      }));
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test("matrix integration correction scope is bounded to declared cell paths", async () => {
  const plan = await makePlan(baseCommit);
  const decisions = plan.cells.map((cell) => ({
    cell_id: cell.cell_id,
    decision: "reject" as const,
    reason: "fixture",
    required_combined_checks: [],
    correction_paths: [],
  }));
  const decision = await decisionFor(plan, decisions);
  assert.deepEqual(assertIntegrationCorrectionScope({ plan, decision }), []);
  const changed = await decisionFor(plan, decisions, ["README.md"]);
  assert.throws(() => assertIntegrationCorrectionScope({ plan, decision: changed }), /outside|union/u);
});

Deno.test({
  name: "matrix integration merges accepted cells in sorted cell order and records ancestry",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-integrate-merge-" });
    try {
      await git(root, ["init", "-b", "development"]);
      await git(root, ["config", "user.name", "Sentinel Test"]);
      await git(root, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.mkdir(`${root}/src`, { recursive: true });
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'base';\n");
      await Deno.writeTextFile(`${root}/src/two.ts`, "export const two = 'base';\n");
      await git(root, ["add", "src"]);
      await git(root, ["commit", "-m", "base"]);
      const baseSha = await git(root, ["rev-parse", "HEAD"]);
      const plan = await makePlan(baseSha);
      const reports: MatrixCellReportV1[] = [];
      for (const cell of [...plan.cells].reverse()) {
        await git(root, ["switch", "-c", cell.branch, baseSha]);
        const path = cell.allowed_paths[0]!;
        await Deno.writeTextFile(
          `${root}/${path}`,
          `export const ${path.includes("one") ? "one" : "two"} = 'fixed';\n`,
        );
        await git(root, ["add", "--", path]);
        await git(root, ["commit", "-m", `cell ${cell.cell_id}`]);
        reports.push(
          await reportFor(
            plan,
            plan.cells.indexOf(cell),
            await git(root, ["rev-parse", "HEAD"]),
            await git(root, ["rev-parse", "HEAD^{tree}"]),
          ),
        );
      }
      await git(root, ["switch", "-c", "sentinel/integrated-m03", baseSha]);
      const decisions = [...plan.cells].reverse().map((cell) => ({
        cell_id: cell.cell_id,
        decision: "accept" as const,
        reason: "validated",
        required_combined_checks: ["deno check"],
        correction_paths: [],
      }));
      const decision = await decisionFor(plan, decisions);
      const executor: MatrixGitExecutor = (command) => runGit(command.cwd, command.args);
      const result = await executeMatrixIntegration({
        plan,
        reports,
        decision,
        checkoutPath: root,
        effectiveBaseSha: baseSha,
        integrationBranch: "sentinel/integrated-m03",
        git: executor,
      });
      assert.deepEqual(result.merge_order, [...result.merge_order].sort());
      assert.equal(result.merge_receipts.length, 2);
      assert.ok(result.merge_receipts.every((receipt) => receipt.strategy === "no_ff" && receipt.is_ancestor));
      assert.equal(result.cycle_report.accepted_ancestry.length, 2);
      assert.ok(result.cycle_report.integrated_candidate);
      assert.equal(await git(root, ["status", "--porcelain"]), "");
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "synthetic three-cell convergence integrates two heads and retains one rejected branch",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-three-cell-" });
    try {
      await git(root, ["init", "-b", "development"]);
      await git(root, ["config", "user.name", "Sentinel Test"]);
      await git(root, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.mkdir(`${root}/src`, { recursive: true });
      for (const name of ["one", "two", "three"]) {
        await Deno.writeTextFile(`${root}/src/${name}.ts`, `export const ${name} = 'base';\n`);
      }
      await git(root, ["add", "src"]);
      await git(root, ["commit", "-m", "base"]);
      const baseSha = await git(root, ["rev-parse", "HEAD"]);
      const plan = await buildMatrixPlan({
        run_id: "m06-acceptance",
        run_attempt: 1,
        base_sha: baseSha,
        evidence_digests: [],
        findings: ["one", "two", "three"].map((name, index) => ({
          id: name,
          fingerprint: String.fromCharCode(97 + index).repeat(64),
          allowed_paths: [`src/${name}.ts`],
          validation_requirements: [`validate ${name}`],
        })),
      });
      assert.equal(plan.cells.length, 3);
      const reports: MatrixCellReportV1[] = [];
      for (const cell of plan.cells) {
        await git(root, ["switch", "-c", cell.branch, baseSha]);
        const path = cell.allowed_paths[0]!;
        const name = path.slice("src/".length, -".ts".length);
        await Deno.writeTextFile(`${root}/${path}`, `export const ${name} = 'fixed';\n`);
        await git(root, ["add", "--", path]);
        await git(root, ["commit", "-m", `cell ${cell.cell_id}`]);
        reports.push(
          await reportFor(
            plan,
            plan.cells.indexOf(cell),
            await git(root, ["rev-parse", "HEAD"]),
            await git(root, ["rev-parse", "HEAD^{tree}"]),
          ),
        );
      }
      await git(root, ["switch", "-c", "sentinel/integrated-m06", baseSha]);
      const rejectedCell = plan.cells.find((cell) => cell.allowed_paths.includes("src/three.ts"))!;
      const decision = await decisionFor(
        plan,
        plan.cells.map((cell) => ({
          cell_id: cell.cell_id,
          decision: cell.cell_id === rejectedCell.cell_id ? "reject" as const : "accept" as const,
          reason: cell.cell_id === rejectedCell.cell_id ? "synthetic semantic rejection" : "validated",
          required_combined_checks: ["deno check"],
          correction_paths: [],
        })),
      );
      const result = await executeMatrixIntegration({
        plan,
        reports,
        decision,
        checkoutPath: root,
        effectiveBaseSha: baseSha,
        integrationBranch: "sentinel/integrated-m06",
        git: (command) => runGit(command.cwd, command.args),
      });
      assert.equal(result.merge_receipts.length, 2);
      assert.equal(result.cycle_report.accepted_ancestry.length, 2);
      assert.equal(result.cycle_report.rejected_branches.length, 1);
      assert.equal(result.cycle_report.rejected_branches[0]?.cell_id, rejectedCell.cell_id);
      assert.equal(result.cycle_report.delivery.status, "not_attempted");
      const integratedHead = result.cycle_report.integrated_candidate?.head_sha;
      assert.ok(integratedHead);
      for (const ancestry of result.cycle_report.accepted_ancestry) {
        assert.equal(ancestry.integrated_head_sha, integratedHead);
        assert.equal(
          await runGit(root, ["merge-base", "--is-ancestor", ancestry.cell_head_sha, integratedHead]).then(
            (output) => output.code,
          ),
          0,
        );
      }
      const rejectedHead = result.cycle_report.rejected_branches[0]?.head_sha;
      assert.ok(rejectedHead);
      assert.equal((await runGit(root, ["merge-base", "--is-ancestor", rejectedHead, integratedHead])).code, 1);
      assert.equal(await git(root, ["status", "--porcelain"]), "");
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix integration permits ours only after exact patch-equivalence proof",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-integrate-ours-" });
    try {
      await git(root, ["init", "-b", "development"]);
      await git(root, ["config", "user.name", "Sentinel Test"]);
      await git(root, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.writeTextFile(`${root}/ours.ts`, "export const value = 'base';\n");
      await git(root, ["add", "ours.ts"]);
      await git(root, ["commit", "-m", "base"]);
      const baseSha = await git(root, ["rev-parse", "HEAD"]);
      const plan = await buildMatrixPlan({
        run_id: "m03-ours",
        run_attempt: 1,
        base_sha: baseSha,
        evidence_digests: [],
        findings: [{
          id: "ours",
          fingerprint: "d".repeat(64),
          allowed_paths: ["ours.ts"],
          validation_requirements: ["focused"],
        }],
      });
      const cell = plan.cells[0]!;
      await git(root, ["switch", "-c", cell.branch, baseSha]);
      await Deno.writeTextFile(`${root}/ours.ts`, "export const value = 'fixed';\n");
      await git(root, ["add", "ours.ts"]);
      await git(root, ["commit", "-m", "cell repair"]);
      const headSha = await git(root, ["rev-parse", "HEAD"]);
      const report = await reportFor(plan, 0, headSha, await git(root, ["rev-parse", "HEAD^{tree}"]));
      await git(root, ["switch", "-c", "sentinel/integrated-ours", baseSha]);
      await Deno.writeTextFile(`${root}/ours.ts`, "export const value = 'fixed';\n");
      const executor: MatrixGitExecutor = (command) => runGit(command.cwd, command.args);
      assert.equal(
        await provePatchEquivalentOursMerge({
          checkoutPath: root,
          plan,
          report,
          integrationHeadSha: baseSha,
          correctionPaths: ["ours.ts"],
          git: executor,
        }),
        true,
      );
      const decision = await decisionFor(plan, [{
        cell_id: cell.cell_id,
        decision: "accept",
        reason: "validated",
        required_combined_checks: ["focused"],
        correction_paths: ["ours.ts"],
      }], ["ours.ts"]);
      const result = await executeMatrixIntegration({
        plan,
        reports: [report],
        decision,
        checkoutPath: root,
        effectiveBaseSha: baseSha,
        integrationBranch: "sentinel/integrated-ours",
        allowPatchEquivalentOurs: true,
        git: executor,
      });
      assert.equal(result.merge_receipts[0]?.strategy, "no_ff_ours");
      assert.equal(await git(root, ["status", "--porcelain"]), "");
      assert.equal(await git(root, ["merge-base", "--is-ancestor", headSha, "HEAD"]).then(() => true), true);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});
import type { CodexInvocationResult } from "../scripts/sentinel/codex.ts";
import type { CodexUsageProbe } from "../scripts/sentinel/quota.ts";

Deno.test({
  name: "matrix integration hard cutover seals the controller-owned decision digest",
  ignore: permissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-matrix-cutover-" });
    try {
      await git(root, ["init", "-b", "development"]);
      await git(root, ["config", "user.name", "Sentinel Test"]);
      await git(root, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.mkdir(`${root}/src`, { recursive: true });
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'base';\n");
      await git(root, ["add", "src"]);
      await git(root, ["commit", "-m", "base"]);
      const baseSha = await git(root, ["rev-parse", "HEAD"]);
      const plan = await buildMatrixPlan({
        run_id: "m09-cutover",
        run_attempt: 1,
        base_sha: baseSha,
        evidence_digests: [],
        findings: [{
          id: "one",
          fingerprint: "a".repeat(64),
          allowed_paths: ["src/one.ts"],
          validation_requirements: ["validate one"],
        }],
      });
      const cell = plan.cells[0]!;
      await git(root, ["switch", "-c", cell.branch]);
      await Deno.writeTextFile(`${root}/src/one.ts`, "export const one = 'fixed';\n");
      await git(root, ["add", "src/one.ts"]);
      await git(root, ["commit", "-m", "cell one"]);
      const headSha = await git(root, ["rev-parse", "HEAD"]);
      const treeSha = await git(root, ["rev-parse", "HEAD^{tree}"]);
      const report = await reportFor(plan, plan.cells.indexOf(cell), headSha, treeSha);
      await git(root, ["switch", "-c", "sentinel/integrated-cutover", baseSha]);
      const executor: MatrixGitExecutor = (command) => runGit(command.cwd, command.args);
      const probes: readonly [CodexUsageProbe, CodexUsageProbe] = [
        { kind: "available", slot: 1, headroomPercent: 0, observedAtMs: 0 },
        { kind: "available", slot: 1, headroomPercent: 0, observedAtMs: 0 },
      ];
      const invocationFor = (lastMessage: string): CodexInvocationResult => ({
        slot: 1,
        headroomPercent: 0,
        probes,
        stdout: "",
        stderr: "",
        lastMessage,
        nativeReviewOutput: null,
      });
      const draft = {
        schema_version: 1,
        run_id: plan.run_id,
        run_attempt: plan.run_attempt,
        plan_digest: plan.manifest_digest,
        base_sha: plan.base_sha,
        decisions: [{
          cell_id: cell.cell_id,
          decision: "accept",
          reason: "coherent",
          required_combined_checks: ["deno check"],
          correction_paths: [],
        }],
        combined_validation_requirements: ["deno check"],
        correction_paths: [],
        summary: "cutover decision",
      };
      const invokeAgent = (lastMessage: string) => () => Promise.resolve(invocationFor(lastMessage));
      // Valid digest-less draft is sealed by the controller: the returned
      // V1 decision passes the strict digest verifier.
      const sealedRun = await runMatrixIntegrationAgent({
        plan,
        reports: [report],
        checkoutPath: root,
        integrationBranch: "sentinel/integrated-cutover",
        git: executor,
        effectiveBaseSha: baseSha,
        outputSchemaPath: `${root}/integration.schema.json`,
        authSlots: {},
        agentInvoker: invokeAgent(JSON.stringify(draft)),
      });
      await assertIntegrationDecisionV1(sealedRun.decision, plan);
      await assertIntegrationDecisionDigest(sealedRun.decision);
      assert.equal(sealedRun.decision.decisions.length, 1);
      assert.match(sealedRun.decision.decision_digest, /^[0-9a-f]{64}$/u);
      // Wrong plan/run identity fails closed.
      for (
        const drifted of [
          { run_id: "other-run" },
          { plan_digest: "0".repeat(64) },
        ]
      ) {
        await assert.rejects(
          () =>
            runMatrixIntegrationAgent({
              plan,
              reports: [report],
              checkoutPath: root,
              integrationBranch: "sentinel/integrated-cutover",
              git: executor,
              effectiveBaseSha: baseSha,
              outputSchemaPath: `${root}/integration.schema.json`,
              authSlots: {},
              agentInvoker: invokeAgent(JSON.stringify({ ...draft, ...drifted })),
            }),
          /contract|identity/u,
        );
      }
      // An explicit model-supplied digest or a missing draft field is never
      // trusted or repaired: the producer boundary rejects the shape.
      for (
        const malformed of [
          { ...draft, decision_digest: "0".repeat(64) },
          { summary: undefined },
        ]
      ) {
        await assert.rejects(
          () =>
            runMatrixIntegrationAgent({
              plan,
              reports: [report],
              checkoutPath: root,
              integrationBranch: "sentinel/integrated-cutover",
              git: executor,
              effectiveBaseSha: baseSha,
              outputSchemaPath: `${root}/integration.schema.json`,
              authSlots: {},
              agentInvoker: invokeAgent(JSON.stringify(malformed)),
            }),
          /unexpected decision draft shape/u,
        );
      }
      // Malformed cell coverage fails closed.
      await assert.rejects(
        () =>
          runMatrixIntegrationAgent({
            plan,
            reports: [report],
            checkoutPath: root,
            integrationBranch: "sentinel/integrated-cutover",
            git: executor,
            effectiveBaseSha: baseSha,
            outputSchemaPath: `${root}/integration.schema.json`,
            authSlots: {},
            agentInvoker: invokeAgent(JSON.stringify({ ...draft, decisions: [] })),
          }),
        /contract|cell/u,
      );
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});
