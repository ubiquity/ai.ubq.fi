import { FixtureWorkspace } from "../fixture.ts";
import { loadTasks } from "../manifest.ts";
import { evaluateOracle, runVerification } from "../oracle.ts";
import { TaskManifest, TaskOracle } from "../schemas.ts";

const TASKS_DIR = `${Deno.cwd()}/benchmarks/tasks`;
const FIXTURES_DIR = `${Deno.cwd()}/benchmarks/fixtures`;

async function withWorkspace(task: TaskManifest, fn: (ws: FixtureWorkspace) => Promise<void>): Promise<void> {
  const ws = new FixtureWorkspace({
    fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
    runId: `oracle-test-${task.id}-${Date.now()}-${Math.floor(Math.random() * 1e6)}`,
    tmpParent: `${Deno.cwd()}/benchmark-runs/tmp`,
    task,
  });
  try {
    await ws.prepare();
    await fn(ws);
  } finally {
    await ws.remove().catch(() => {});
  }
}

Deno.test("oracle: file checks pass, fail, and invert", async () => {
  const nav = loadTasks(TASKS_DIR).find((t) => t.id === "nav-001")!;
  await withWorkspace(nav, async (ws) => {
    ws.write("answer.txt", "docs/spec.txt");
    const oracle: TaskOracle = {
      file_checks: [
        { path: "answer.txt", kind: "equals", value: "docs/spec.txt" },
        { path: "docs/spec.txt", kind: "contains", value: "Section 1" },
        { path: "docs/spec.txt", kind: "regex", value: "^# Spec" },
        { path: "missing.txt", kind: "exists", invert: true },
      ],
    };
    const out = await evaluateOracle({ ...nav, oracle }, ws);
    if (!out.passed || out.checks.length !== 4) throw new Error(`expected all checks to pass: ${JSON.stringify(out)}`);
    const failing = await evaluateOracle(
      { ...nav, oracle: { file_checks: [{ path: "answer.txt", kind: "equals", value: "wrong" }] } },
      ws,
    );
    if (failing.passed || failing.checks[0].passed) throw new Error("expected equals mismatch to fail");
    const missing = await evaluateOracle(
      { ...nav, oracle: { file_checks: [{ path: "nope.txt", kind: "exists" }] } },
      ws,
    );
    if (missing.passed) throw new Error("expected missing file check to fail");
  });
});

Deno.test("oracle: verification passes, fails, and times out", async () => {
  const nav = loadTasks(TASKS_DIR).find((t) => t.id === "nav-001")!;
  await withWorkspace(nav, async (ws) => {
    ws.write("answer.txt", "docs/spec.txt");
    const ok = await runVerification({ ...nav, verify: { command: "true" } }, ws);
    if (!ok.passed || ok.exit_code !== 0) throw new Error(`expected verify pass, got ${JSON.stringify(ok)}`);
    const bad = await runVerification({ ...nav, verify: { command: "false" } }, ws);
    if (bad.passed || bad.exit_code !== 1) throw new Error(`expected verify fail, got ${JSON.stringify(bad)}`);
    const slow = await runVerification({ ...nav, verify: { command: "sleep 2", timeout_ms: 100 } }, ws);
    if (!slow.timed_out || slow.passed) throw new Error(`expected verify timeout, got ${JSON.stringify(slow)}`);
    const noVerify = await runVerification({ ...nav, verify: undefined, oracle: {} }, ws);
    if (noVerify.ran || !noVerify.passed) throw new Error("expected skipped verification to be neutral");
  });
});

Deno.test("oracle: git checks on a disposable repository", async () => {
  const seq004 = loadTasks(TASKS_DIR).find((t) => t.id === "seq-004")!;
  await withWorkspace(seq004, async (ws) => {
    const oracle: TaskOracle = {
      git_checks: [
        { kind: "commit_count", value: "1" },
        { kind: "head_message", value: "base" },
        { kind: "worktree_clean" },
        { kind: "file_committed", value: "src/app.txt" },
      ],
    };
    const out = await evaluateOracle({ ...seq004, oracle }, ws);
    if (!out.passed || out.checks.length !== 4) throw new Error(`expected git checks to pass: ${JSON.stringify(out)}`);
  });
});

Deno.test("oracle: git checks without a repository fail closed", async () => {
  const nav = loadTasks(TASKS_DIR).find((t) => t.id === "nav-001")!;
  await withWorkspace(nav, async (ws) => {
    const out = await evaluateOracle(
      { ...nav, oracle: { git_checks: [{ kind: "worktree_clean" }] } },
      ws,
    );
    if (out.passed) throw new Error("expected git check to fail when task has no git repository");
  });
});
