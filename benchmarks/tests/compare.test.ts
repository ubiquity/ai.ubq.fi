import assert from "node:assert/strict";

import { broadToolSurface, compactToolSurface, surfaceTokenCost } from "../../src/harmony/reliability/surfaces.ts";
import { buildContextEvidence } from "../compare.ts";
import { loadTasks } from "../manifest.ts";

const TASKS_DIR = `${Deno.cwd()}/benchmarks/tasks`;

function freshOptions() {
  Deno.mkdirSync(`${Deno.cwd()}/benchmark-runs`, { recursive: true });
  const runsRoot = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
  return { runsRoot };
}

Deno.test("compare: evidence runs deterministically without inference", async () => {
  const { runsRoot } = freshOptions();
  try {
    const tasks = loadTasks(TASKS_DIR).filter((t) => ["nav-001", "fail-001", "long-003"].includes(t.id));
    assert.equal(tasks.length, 3);
    const evidence = await buildContextEvidence(tasks, runsRoot);
    assert.equal(evidence.schema_version, "1.0");
    assert.equal(evidence.mode, "deterministic");
    assert.equal(evidence.tasks.length, 3);
    const compactSurface = evidence.surfaces.find((s) => s.id === "compact")!;
    const broadSurface = evidence.surfaces.find((s) => s.id === "broad")!;
    assert.equal(compactSurface.tools, 9);
    assert.equal(broadSurface.tools, 13);
    assert.ok(broadSurface.tokens > compactSurface.tokens);
    for (const task of evidence.tasks) {
      assert.ok(task.full_transcript_tokens > 0);
      assert.equal(task.budgets.length, 3);
      for (const budget of task.budgets) {
        assert.equal(budget.contract_preserved, true, `${task.task_id}@${budget.budget}`);
        assert.ok(budget.full_transcript_tokens >= budget.compaction_tokens);
      }
    }
    // Structured context costs strictly less than a full replay in the
    // long-horizon task, where the compaction/state-summary payoff is real.
    const long = evidence.tasks.find((t) => t.task_id === "long-003")!;
    const medium = long.budgets.find((b) => b.budget === "medium")!;
    assert.ok(
      medium.structured_tokens < medium.full_transcript_tokens,
      `structured ${medium.structured_tokens} < full ${medium.full_transcript_tokens}`,
    );
  } finally {
    Deno.removeSync(runsRoot, { recursive: true });
  }
});

Deno.test("compare: surfaces report compact always cheaper than broad", () => {
  const compact = compactToolSurface();
  const broad = broadToolSurface();
  assert.equal(compact.definitions.length, 9);
  assert.equal(broad.definitions.length, 13);
  assert.ok(surfaceTokenCost(broad) > surfaceTokenCost(compact));
  assert.equal(surfaceTokenCost(compact), surfaceTokenCost(compactToolSurface()));
});

Deno.test("compare: aggregation carries per-budget evidence", async () => {
  const { runsRoot } = freshOptions();
  try {
    const tasks = loadTasks(TASKS_DIR).filter((t) => t.id === "nav-001");
    const evidence = await buildContextEvidence(tasks, runsRoot);
    for (const kind of ["short", "medium", "large"] as const) {
      const agg = evidence.aggregate[kind];
      assert.equal(agg.tasks, 1);
      assert.equal(agg.contract_preserved, 1);
      assert.ok(agg.structured_ratio > 0 && agg.structured_ratio <= 1);
    }
  } finally {
    Deno.removeSync(runsRoot, { recursive: true });
  }
});
