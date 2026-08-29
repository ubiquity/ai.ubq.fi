import { aggregateResults, deriveMetrics, formatSummary } from "../metrics.ts";
import { loadResults } from "../summarize.ts";
import { BenchmarkResult, TrajectoryEvent } from "../schemas.ts";

function fakeResult(overrides: Partial<BenchmarkResult> = {}): BenchmarkResult {
  return {
    schema_version: "1.0",
    run_id: "r1",
    task_id: "t-001",
    config_id: "reference",
    adapter_id: "reference",
    fixture: "t-001",
    fixture_revision: "sha256:" + "0".repeat(64),
    started_at: "2026-01-01T00:00:00.000Z",
    ended_at: "2026-01-01T00:00:00.001Z",
    wall_time_ms: 1,
    success: true,
    failure_class: null,
    failure_detail: null,
    metrics: {
      model_calls: 0,
      tool_calls: 4,
      invalid_tool_calls: 0,
      wrong_tool_calls: 0,
      repeated_calls: 0,
      tool_errors: 0,
      recovery_attempts: 0,
      input_tokens: 0,
      output_tokens: 0,
      context_size: 0,
    },
    verification: { ran: true, passed: true, command: "true", exit_code: 0, timed_out: false, output: null },
    oracle: { passed: true, checks: [] },
    required_calls: { min_tool_calls: 0, tool_calls: 4, min_model_calls: 0, model_calls: 0, met: true },
    trajectory: "runs/r1/trajectory.jsonl",
    created_at: "2026-01-01T00:00:00.001Z",
    ...overrides,
  };
}

Deno.test("metrics: derives invalid, wrong, repeated, recovery, and tokens", () => {
  const at = "2026-01-01T00:00:00.000Z";
  const events: TrajectoryEvent[] = [
    {
      type: "model_request",
      at,
      id: 1,
      model: "gpt-oss-120b",
      message_count: 3,
      input_tokens: 100,
      output_tokens: 20,
      tool_count: 4,
    },
    {
      type: "model_request",
      at,
      id: 2,
      model: "gpt-oss-120b",
      message_count: 5,
      input_tokens: 50,
      output_tokens: 10,
      tool_count: 4,
    },
    {
      type: "tool_call",
      at,
      id: "t1",
      tool: "filesystem.read",
      arguments: { path: 42 },
      valid: false,
      invalid_reason: "must be string",
    },
    { type: "tool_result", at, id: "t1", ok: false, error: "invalid arguments", error_code: "invalid_args" },
    { type: "tool_call", at, id: "t2", tool: "filesystem.read", arguments: { path: "a.txt" }, valid: true },
    { type: "tool_result", at, id: "t2", ok: true, output: "a" },
    { type: "tool_call", at, id: "t3", tool: "filesystem.read", arguments: { path: "a.txt" }, valid: true }, // consecutive duplicate
    { type: "tool_result", at, id: "t3", ok: true, output: "a" },
    {
      type: "tool_call",
      at,
      id: "t4",
      tool: "shell.exec",
      arguments: { command: "cat x" },
      valid: true,
      is_wrong_tool: true,
    },
    { type: "tool_result", at, id: "t4", ok: false, error: "wrong tool", error_code: "wrong_tool" },
    { type: "tool_call", at, id: "t5", tool: "shell.exec", arguments: { command: "cat x" }, valid: true },
    { type: "tool_result", at, id: "t5", ok: true, output: "x" },
  ];
  const m = deriveMetrics(events);
  if (m.model_calls !== 2) throw new Error(`model_calls ${m.model_calls}`);
  if (m.tool_calls !== 5) throw new Error(`tool_calls ${m.tool_calls}`);
  if (m.invalid_tool_calls !== 1) throw new Error(`invalid ${m.invalid_tool_calls}`);
  if (m.wrong_tool_calls !== 1) throw new Error(`wrong ${m.wrong_tool_calls}`);
  if (m.repeated_calls !== 1) throw new Error(`repeated ${m.repeated_calls}`);
  if (m.tool_errors !== 2) throw new Error(`tool_errors ${m.tool_errors}`);
  // Both failed calls (invalid args and wrong tool) are recovered later.
  if (m.recovery_attempts !== 2) throw new Error(`recovery ${m.recovery_attempts}`);
  if (m.input_tokens !== 150 || m.output_tokens !== 30 || m.context_size !== 120) {
    throw new Error(`tokens ${m.input_tokens}/${m.output_tokens}/${m.context_size}`);
  }
});

Deno.test("metrics: aggregates results into config and task groups", () => {
  const runsRoot = "benchmark-runs";
  const results = [
    fakeResult({ run_id: "a", wall_time_ms: 10, task_id: "t-001" }),
    fakeResult({ run_id: "b", wall_time_ms: 20, task_id: "t-002" }),
    fakeResult({
      run_id: "c",
      wall_time_ms: 30,
      task_id: "t-001",
      success: false,
      failure_class: "verification_failed",
      failure_detail: "x",
      metrics: { ...fakeResult().metrics, tool_errors: 2 },
    }),
  ];
  const summary = aggregateResults(results, runsRoot);
  if (summary.run_count !== 3 || summary.success_count !== 2 || summary.success_rate !== 2 / 3) {
    throw new Error(`summary counts ${summary.run_count}/${summary.success_count}`);
  }
  const config = summary.by_config.find((g) => g.config_id === "reference")!;
  if (config.task_count !== 2 || config.failures !== 1 || config.success_rate !== 2 / 3) {
    throw new Error("config group wrong");
  }
  if (config.wall_time_ms.median !== 20 || config.wall_time_ms.p95 !== 30) throw new Error("wall stats wrong");
  if (config.failure_classes.verification_failed !== 1) throw new Error("failure class breakdown wrong");
  if (config.total_tool_errors !== 2) throw new Error("totals wrong");
  const task = summary.by_task_config.find((g) => g.task_id === "t-001")!;
  if (task.runs !== 2 || task.failures !== 1) throw new Error("task group wrong");
  if (!formatSummary(summary).includes("runs: 3")) throw new Error("summary formatting broken");
});

Deno.test("summary: loads result.jsonl from a runs root and parses deno-task '--'", async () => {
  const { parseSummarizeArgs } = await import("../summarize.ts");
  Deno.mkdirSync(`${Deno.cwd()}/benchmark-runs`, { recursive: true });
  const root = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
  try {
    Deno.mkdirSync(`${root}/runs/r1`, { recursive: true });
    Deno.writeTextFileSync(`${root}/runs/r1/result.jsonl`, JSON.stringify(fakeResult()) + "\n");
    const loaded = loadResults(root);
    if (loaded.length !== 1 || loaded[0].run_id !== "r1") throw new Error("loadResults failed");
    const options = parseSummarizeArgs(["--", "--runs=.", "--json"]);
    if ("help" in options) throw new Error("unexpected help");
    if (!options.jsonOnly || options.runsRoot !== ".") throw new Error("summary arg parse failed");
  } finally {
    Deno.removeSync(root, { recursive: true });
  }
});
