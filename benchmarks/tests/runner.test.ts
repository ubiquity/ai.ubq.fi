import { BenchmarkAdapter, referenceAdapter } from "../adapter.ts";
import { loadTasks } from "../manifest.ts";
import { runBenchmarks, runOne, RunOptions } from "../runner.ts";
import { TaskManifest, validateTrajectoryEvent } from "../schemas.ts";

const TASKS_DIR = `${Deno.cwd()}/benchmarks/tasks`;
const FIXTURES_DIR = `${Deno.cwd()}/benchmarks/fixtures`;

function freshOptions(): RunOptions & { runsRoot: string } {
  Deno.mkdirSync(`${Deno.cwd()}/benchmark-runs`, { recursive: true });
  const runsRoot = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
  return {
    configs: ["reference"],
    taskSelectors: [],
    runsRoot,
    tasksDir: TASKS_DIR,
    fixturesDir: FIXTURES_DIR,
  };
}

function nav001(): TaskManifest {
  return loadTasks(TASKS_DIR).find((t) => t.id === "nav-001")!;
}

Deno.test("runner: reference run succeeds with recorded trajectory and metrics", async () => {
  const opts = freshOptions();
  try {
    const { result, events } = await runOne(nav001(), referenceAdapter, opts);
    if (!result.success || result.failure_class !== null) {
      throw new Error(`expected success, got ${result.failure_class}: ${result.failure_detail}`);
    }
    if (result.metrics.tool_calls !== 4) throw new Error(`expected 4 tool calls, got ${result.metrics.tool_calls}`);
    if (!result.verification.passed || !result.oracle.passed) {
      throw new Error("expected verification and oracle to pass");
    }
    if (events.length !== result.metrics.tool_calls * 2 + 2) throw new Error("unexpected event count"); // run + verify + call/result pairs
    for (const event of events) validateTrajectoryEvent(event);
    const traj = Deno.readTextFileSync(`${opts.runsRoot}/${result.trajectory}`);
    const lines = traj.trimEnd().split("\n");
    if (lines.length !== events.length) throw new Error("trajectory.jsonl row count mismatch");
    const summary = Deno.readTextFileSync(`${opts.runsRoot}/runs/${result.run_id}/result.jsonl`);
    if (summary.trimEnd().split("\n").length !== 1) throw new Error("result.jsonl must be one record");
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: verification failure is classified", async () => {
  const opts = freshOptions();
  try {
    const task = { ...nav001(), verify: { command: "false" } };
    const { result } = await runOne(task, referenceAdapter, opts);
    if (result.success || result.failure_class !== "verification_failed") {
      throw new Error(`expected verification_failed, got ${result.failure_class}: ${result.failure_detail}`);
    }
    if (!result.verification.ran || result.verification.exit_code !== 1) throw new Error("verify evidence missing");
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: min_tool_calls not met is classified", async () => {
  const opts = freshOptions();
  try {
    const task = { ...nav001(), min_tool_calls: 10 };
    const { result } = await runOne(task, referenceAdapter, opts);
    if (result.success || result.failure_class !== "min_calls_not_met") {
      throw new Error(`expected min_calls_not_met, got ${result.failure_class}: ${result.failure_detail}`);
    }
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: tool_call_limit is classified", async () => {
  const opts = freshOptions();
  try {
    const task = { ...nav001(), max_tool_calls: 2 };
    const { result } = await runOne(task, referenceAdapter, opts);
    if (result.success || result.failure_class !== "tool_call_limit") {
      throw new Error(`expected tool_call_limit, got ${result.failure_class}: ${result.failure_detail}`);
    }
    if (result.metrics.tool_calls !== 3) throw new Error(`expected 3 recorded calls, got ${result.metrics.tool_calls}`);
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: whole-run timeout cancels in-flight shell and classifies", async () => {
  const opts = freshOptions();
  try {
    const task: TaskManifest = {
      ...nav001(),
      timeout_ms: 400,
      scripted_trail: [
        { tool: "shell.exec", args: { command: "sleep 5" } },
        { tool: "filesystem.read", args: { path: "docs/spec.txt" } },
      ],
    };
    const { result } = await runOne(task, referenceAdapter, opts);
    if (result.success || result.failure_class !== "timeout") {
      throw new Error(`expected timeout, got ${result.failure_class}: ${result.failure_detail}`);
    }
    if (result.wall_time_ms > 4000) throw new Error(`timeout did not cancel work: ${result.wall_time_ms}ms`);
    if (result.metrics.tool_errors < 1) throw new Error("expected the interrupted tool error to be recorded");
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: fixture revision mismatch fails before the adapter runs", async () => {
  const opts = freshOptions();
  try {
    const task = { ...nav001(), fixture_revision: "sha256:" + "0".repeat(64) };
    const { result, events } = await runOne(task, referenceAdapter, opts);
    if (result.success || result.failure_class !== "fixture_revision_mismatch") {
      throw new Error(`expected fixture_revision_mismatch, got ${result.failure_class}: ${result.failure_detail}`);
    }
    if (events.some((e) => e.type === "tool_call")) throw new Error("adapter must not run on revision mismatch");
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: refuses unused configs and external-inference adapters", async () => {
  const opts = freshOptions();
  try {
    let threw = false;
    try {
      await runBenchmarks({ ...opts, configs: ["nope"], taskSelectors: ["nav-001"] });
    } catch {
      threw = true;
    }
    if (!threw) throw new Error("expected unknown-config error");
    const live: BenchmarkAdapter = {
      configId: "live",
      name: "live",
      description: "external",
      requiresExternalInference: true,
      run: async () => {},
    };
    let refused = false;
    try {
      await runBenchmarks({ ...opts, configs: ["live"], taskSelectors: ["nav-001"], adapters: [live] });
    } catch (err) {
      refused = (err as Error).message.includes("external-inference");
    }
    if (!refused) throw new Error("expected external-inference refusal");
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: selection and limit flags compose", async () => {
  const opts = freshOptions();
  try {
    const results = await runBenchmarks({
      ...opts,
      configs: ["reference"],
      taskSelectors: ["category:long"],
      limit: 2,
    });
    if (results.length !== 2) throw new Error(`expected 2 limited runs, got ${results.length}`);
    if (!results.every((r) => r.config_id === "reference" && r.task_id.startsWith("long-"))) {
      throw new Error("unexpected selection");
    }
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("runner: parseRunArgs accepts the deno-task '--' separator", async () => {
  const { parseRunArgs } = await import("../runner.ts");
  const opts = parseRunArgs(["--", "--configs=reference", "--tasks=nav-001,code-*", "--limit=3"]);
  if ("help" in opts) throw new Error("unexpected help");
  if (opts.configs[0] !== "reference" || opts.taskSelectors.join(",") !== "nav-001,code-*" || opts.limit !== 3) {
    throw new Error("arg parsing failed");
  }
  let threw = false;
  try {
    parseRunArgs(["--bogus=1"]);
  } catch {
    threw = true;
  }
  if (!threw) throw new Error("expected unknown argument rejection");
});
