import assert from "node:assert/strict";
import {
  assertIntegrationDecisionDigest,
  assertIntegrationDecisionV1,
  assertMatrixCellReportDigest,
  assertMatrixCellReportV1,
  assertMatrixCycleReportDigest,
  assertMatrixCycleReportV1,
  assertMatrixPlanDigest,
  assertMatrixPlanV1,
  buildMatrixConflictGraph,
  buildMatrixPlan,
  canonicalMatrixJson,
  encodeMatrixPlanV1,
  integrationDecisionDigest,
  type IntegrationDecisionV1,
  isMatrixPlanV1,
  matrixCellReportDigest,
  type MatrixCellReportV1,
  matrixCycleReportDigest,
  type MatrixCycleReportV1,
  matrixPlanDigest,
  type MatrixPlanV1,
  parseMatrixPlanV1,
} from "../scripts/sentinel/matrix.ts";

const baseSha = "a".repeat(40);
const digestA = "b".repeat(64);
const digestB = "c".repeat(64);

const finding = (
  id: string,
  fingerprint: string,
  allowed_paths: readonly string[],
  overrides: Readonly<Record<string, unknown>> = {},
) => ({
  id,
  fingerprint,
  allowed_paths,
  validation_requirements: [`validate ${id}`],
  ...overrides,
});

Deno.test("matrix planner deterministically groups transitive conflicts and preserves full coverage", async () => {
  const input = {
    run_id: "123456789",
    run_attempt: 2,
    base_sha: baseSha,
    evidence_digests: [
      { name: "triage.json", sha256: digestA },
      { name: "raw-logs/triage.jsonl", sha256: digestB },
    ],
    findings: [
      finding("third", "3".repeat(64), ["src/third.ts"], { depends_on: ["second"] }),
      finding("first", "1".repeat(64), ["src/shared.ts"]),
      finding("second", "2".repeat(64), ["src/shared.ts", "src/second.ts"]),
      finding("independent", "4".repeat(64), ["src/independent.ts"]),
    ],
  } as const;

  const first = await buildMatrixPlan(input);
  const second = await buildMatrixPlan({ ...input, findings: [...input.findings].reverse() });
  assert.deepEqual(first, second);
  assert.equal(first.maximum_parallelism, 4);
  assert.equal(first.cells.length, 2);
  assert.deepEqual(first.cells.flatMap((cell) => cell.finding_ids).sort(), ["first", "independent", "second", "third"]);
  assert.equal(first.cells[0]!.finding_ids.includes("first") || first.cells[1]!.finding_ids.includes("first"), true);
  assert.ok(first.cells.every((cell) => cell.branch.startsWith("sentinel/candidate-123456789-2-cell-")));
  assert.ok(first.cells.every((cell) => cell.report_path.endsWith(`${cell.cell_id}.json`)));
  assert.ok(first.cells.every((cell) => cell.artifact_name.endsWith(cell.cell_id)));
  await assertMatrixPlanDigest(first);
  assert.equal(await matrixPlanDigest(first), first.manifest_digest);
  assert.equal(isMatrixPlanV1(first), true);
});

Deno.test("matrix conflict graph uses path boundaries, shared contracts, and dependencies", () => {
  const graph = buildMatrixConflictGraph([
    finding("directory", "1".repeat(64), ["src/api"]),
    finding("child", "2".repeat(64), ["src/api/client.ts"]),
    finding("prefix-lookalike", "3".repeat(64), ["src/apix.ts"]),
    finding("shared", "4".repeat(64), ["docs/one.md"], { shared_paths: ["src/config.ts"] }),
    finding("shared-peer", "5".repeat(64), ["docs/two.md"], { shared_paths: ["src/config.ts"] }),
    finding("dependent", "6".repeat(64), ["docs/three.md"], { depends_on: ["prefix-lookalike"] }),
  ]);
  assert.equal(graph.edges.length, 3);
  assert.deepEqual(
    graph.edges.map((edge) => edge.reasons),
    [["allowed_path_overlap"], ["dependency"], ["shared_contract_overlap"]],
  );
  assert.equal(graph.components.length, 3);
  assert.ok(graph.components.some((component) => component.includes("directory") && component.includes("child")));
  assert.ok(graph.components.some((component) => component.includes("shared") && component.includes("shared-peer")));
  assert.ok(graph.components.some((component) => component.includes("prefix-lookalike")));
});

Deno.test("matrix planner fails closed for ambiguous ownership and the four-cell limit", async () => {
  await assert.rejects(
    () =>
      buildMatrixPlan({
        run_id: "1",
        run_attempt: 1,
        base_sha: baseSha,
        evidence_digests: [],
        findings: [finding("missing-scope", "1".repeat(64), [])],
      }),
    /no allowed paths/,
  );

  const findings = Array.from(
    { length: 5 },
    (_, index) => finding(`finding-${index + 1}`, (index + 1).toString(16).repeat(64), [`src/${index + 1}.ts`]),
  );
  await assert.rejects(
    () => buildMatrixPlan({ run_id: "2", run_attempt: 1, base_sha: baseSha, evidence_digests: [], findings }),
    /exceeding maximum 4/,
  );
  assert.throws(
    () =>
      buildMatrixConflictGraph([
        finding("duplicate-a", "d".repeat(64), ["src/a.ts"]),
        finding("duplicate-b", "d".repeat(64), ["src/b.ts"]),
      ]),
    /Duplicate matrix finding fingerprint/,
  );
});

Deno.test("matrix plan encoding is canonical, digest-bound, and rejects malformed input", async () => {
  const plan = await buildMatrixPlan({
    run_id: "42",
    run_attempt: 1,
    base_sha: baseSha,
    evidence_digests: [{ name: "triage.json", sha256: digestA }],
    findings: [finding("one", "1".repeat(64), ["src/one.ts"])],
  });
  const encoded = await encodeMatrixPlanV1(plan);
  const decoded = await parseMatrixPlanV1(encoded);
  assert.deepEqual(decoded, plan);
  assert.equal(canonicalMatrixJson({ b: 2, a: { d: 4, c: 3 } }), '{"a":{"c":3,"d":4},"b":2}');

  const tampered = JSON.parse(encoded) as Record<string, unknown>;
  (tampered.evidence_digests as Array<Record<string, unknown>>)[0]!.sha256 = digestB;
  await assert.rejects(() => parseMatrixPlanV1(JSON.stringify(tampered)), /manifest digest/);
  assert.equal(isMatrixPlanV1({ ...plan, schema_version: 2 }), false);
  assert.throws(() => assertMatrixPlanV1({ ...plan, maximum_parallelism: 5 }), /maximum_parallelism/);
});

const reportFor = async (plan: MatrixPlanV1): Promise<MatrixCellReportV1> => {
  const cell = plan.cells[0]!;
  const unsigned = {
    schema_version: 1 as const,
    run_id: plan.run_id,
    run_attempt: plan.run_attempt,
    plan_digest: plan.manifest_digest,
    cell_id: cell.cell_id,
    base_sha: plan.base_sha,
    branch: cell.branch,
    head_sha: "d".repeat(40),
    tree_sha: "e".repeat(40),
    changed_paths: [...cell.allowed_paths],
    finding_dispositions: cell.finding_ids.map((finding_id) => ({
      finding_id,
      fingerprint: plan.ownership.find((finding) => finding.finding_id === finding_id)!.fingerprint,
      status: "implemented" as const,
      summary: "fixed",
      changed_files: [cell.allowed_paths[0]!],
      validation: ["focused test"],
    })),
    validation: { passed: true, checks: [{ name: "focused", passed: true, detail: "passed" }] },
    replay: { attempted: false, passed: true, results: [] },
    status: "succeeded" as const,
    failure_reason: null,
    artifact_sha256: digestB,
    report_digest: "0".repeat(64),
  } satisfies MatrixCellReportV1;
  return { ...unsigned, report_digest: await matrixCellReportDigest(unsigned) };
};

Deno.test("matrix cell report is bound to plan identity and digest", async () => {
  const plan = await buildMatrixPlan({
    run_id: "55",
    run_attempt: 1,
    base_sha: baseSha,
    evidence_digests: [],
    findings: [finding("one", "1".repeat(64), ["src/one.ts"])],
  });
  const report = await reportFor(plan);
  assertMatrixCellReportV1(report, plan);
  await assertMatrixCellReportDigest(report);
  const altered = { ...report, base_sha: "f".repeat(40) };
  assert.throws(() => assertMatrixCellReportV1(altered, plan), /identity differs/);
  assert.throws(
    () => assertMatrixCellReportV1({ ...report, changed_paths: ["src/other.ts"] }, plan),
    /outside|changed paths|cell report/,
  );
});

Deno.test("matrix cell reports reject non-canonical changed path order", async () => {
  const plan = await buildMatrixPlan({
    run_id: "56",
    run_attempt: 1,
    base_sha: baseSha,
    evidence_digests: [],
    findings: [finding("one", "1".repeat(64), ["src/a.ts", "src/b.ts"])],
  });
  const report = await reportFor(plan);
  assert.throws(
    () => assertMatrixCellReportV1({ ...report, changed_paths: ["src/b.ts", "src/a.ts"] }, plan),
    /changed_paths are not canonical/,
  );
});

Deno.test("integration and cycle contracts require complete decisions and ancestry", async () => {
  const plan = await buildMatrixPlan({
    run_id: "66",
    run_attempt: 1,
    base_sha: baseSha,
    evidence_digests: [],
    findings: [
      finding("one", "1".repeat(64), ["src/one.ts"]),
      finding("two", "2".repeat(64), ["src/two.ts"]),
    ],
  });
  const decisions = plan.cells.map((cell) => ({
    cell_id: cell.cell_id,
    decision: "accept" as const,
    reason: "validated",
    required_combined_checks: ["deno check"],
    correction_paths: [],
  }));
  const unsignedDecision = {
    schema_version: 1 as const,
    run_id: plan.run_id,
    run_attempt: plan.run_attempt,
    plan_digest: plan.manifest_digest,
    base_sha: plan.base_sha,
    decisions,
    combined_validation_requirements: ["deno check"],
    correction_paths: [],
    summary: "all cells accepted",
    decision_digest: "0".repeat(64),
  } satisfies IntegrationDecisionV1;
  const decision = { ...unsignedDecision, decision_digest: await integrationDecisionDigest(unsignedDecision) };
  assertIntegrationDecisionV1(decision, plan);
  await assertIntegrationDecisionDigest(decision);
  assert.throws(
    () => assertIntegrationDecisionV1({ ...decision, decisions: decisions.slice(0, 1) }, plan),
    /cover every matrix cell/,
  );

  const accepted = plan.cells.map((cell) => ({
    cell_id: cell.cell_id,
    branch: cell.branch,
    finding_ids: [...cell.finding_ids],
    status: "accepted" as const,
    head_sha: "d".repeat(40),
    reason: null,
  }));
  const integratedHead = "e".repeat(40);
  const unsignedCycle = {
    schema_version: 1 as const,
    run_id: plan.run_id,
    run_attempt: plan.run_attempt,
    plan_digest: plan.manifest_digest,
    base_sha: plan.base_sha,
    cell_dispositions: accepted,
    accepted_ancestry: accepted.map((cell) => ({
      cell_id: cell.cell_id,
      cell_head_sha: cell.head_sha!,
      integrated_head_sha: integratedHead,
      is_ancestor: true,
    })),
    rejected_branches: [],
    blocked_branches: [],
    integrated_candidate: {
      base_sha: plan.base_sha,
      branch: "sentinel/integrated-66-1",
      head_sha: integratedHead,
      tree_sha: "f".repeat(40),
    },
    delivery: { status: "ready" as const, pr_number: null, merge_sha: null, reason: null },
    cycle_digest: "0".repeat(64),
  } satisfies MatrixCycleReportV1;
  const cycle = { ...unsignedCycle, cycle_digest: await matrixCycleReportDigest(unsignedCycle) };
  assertMatrixCycleReportV1(cycle, plan);
  await assertMatrixCycleReportDigest(cycle);
  assert.throws(
    () => assertMatrixCycleReportV1({ ...cycle, accepted_ancestry: [] }, plan),
    /ancestry evidence/,
  );
});
