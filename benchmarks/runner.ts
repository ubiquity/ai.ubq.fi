/**
 * Benchmark runner CLI.
 *
 * Usage (from the repository root):
 *   deno task benchmark:run -- --configs=reference            # hermetic default
 *   deno task benchmark:run -- --configs=reference --tasks=nav-*,fail-*
 *   deno task benchmark:run -- --configs=reference --tasks=category:long --limit=3
 *   deno task benchmark:run -- --runs=benchmark-runs --configs=all
 *
 * The runner only executes adapters registered in defaultAdapters(). The
 * built-in `reference` adapter is deterministic and calls no external model;
 * adapters flagged requiresExternalInference are refused until an approved
 * live-inference gate exists (m03/m05). Selection flags are the only
 * configuration surface; no environment variables are read.
 */

import {
  AdapterRunContext,
  BenchmarkAdapter,
  defaultAdapters,
  TaskTimeoutError,
  ToolCallLimitExceededError,
} from "./adapter.ts";
import { computeFixtureRevision, FixtureRevisionMismatchError, FixtureWorkspace } from "./fixture.ts";
import { loadTasks, selectTasks } from "./manifest.ts";
import { aggregateResults, deriveMetrics, formatSummary } from "./metrics.ts";
import { evaluateOracle, runVerification } from "./oracle.ts";
import { deriveReliability } from "./reliability.ts";
import {
  BENCHMARK_ROOT,
  BENCHMARK_SCHEMA_VERSION,
  BenchmarkResult,
  DEFAULT_RUNS_ROOT,
  FailureClass,
  TaskManifest,
  TrajectoryEvent,
  validateBenchmarkResult,
  validateTrajectoryEvent,
} from "./schemas.ts";

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

export interface RunOptions {
  /** Adapter config ids, or ["all"] for every registered adapter. */
  configs: string[];
  /** Task selectors: exact id, glob, `*`, or `category:<name>`. */
  taskSelectors: string[];
  /** Cap the number of (task × config) pairs executed. */
  limit?: number;
  /** Writable results root (git-ignored). */
  runsRoot: string;
  tasksDir: string;
  fixturesDir: string;
  /** Adapter registry; defaults to defaultAdapters() (m03 registers A/B/D here). */
  adapters?: BenchmarkAdapter[];
}

export function defaultRunOptions(): RunOptions {
  return {
    configs: ["reference"],
    taskSelectors: ["*"],
    runsRoot: DEFAULT_RUNS_ROOT,
    tasksDir: `${BENCHMARK_ROOT}/tasks`,
    fixturesDir: `${BENCHMARK_ROOT}/fixtures`,
  };
}

export function parseRunArgs(argv: string[]): RunOptions | { help: true } {
  // `deno task benchmark:run -- args` forwards a literal "--"; accept both forms.
  if (argv[0] === "--") argv = argv.slice(1);
  const opts = defaultRunOptions();
  for (const arg of argv) {
    if (arg === "--help" || arg === "-h") return { help: true };
    if (arg.startsWith("--configs=")) opts.configs = arg.slice("--configs=".length).split(",").filter(Boolean);
    else if (arg.startsWith("--tasks=")) opts.taskSelectors = arg.slice("--tasks=".length).split(",").filter(Boolean);
    else if (arg.startsWith("--limit=")) opts.limit = parseInt(arg.slice("--limit=".length), 10);
    else if (arg.startsWith("--runs=")) opts.runsRoot = arg.slice("--runs=".length);
    else throw new Error(`unknown argument ${arg} (see --help)`);
  }
  if (opts.limit !== undefined && (!Number.isInteger(opts.limit) || opts.limit < 1)) {
    throw new Error("--limit must be a positive integer");
  }
  return opts;
}

export const RUN_HELP =
  `usage: deno task benchmark:run -- [--configs=reference] [--tasks=*] [--limit=N] [--runs=benchmark-runs]

  --configs    comma-separated adapter config ids, or "all" (default: reference)
  --tasks      comma-separated selectors: exact id, glob (* pattern), "category:<name>"
  --limit      cap the number of (task x config) pairs
  --runs       results root directory (default: benchmark-runs)

registered configs: ${defaultAdapters().map((a) => a.configId).join(", ")}`;

// ---------------------------------------------------------------------------
// Single run
// ---------------------------------------------------------------------------

function isoNow(): string {
  return new Date().toISOString();
}

function uniqueRunId(runsRoot: string, configId: string, taskId: string): string {
  const stamp = isoNow().replace(/[^0-9]/g, "").slice(0, 14);
  const base = `${stamp}-${configId}-${taskId}`;
  let candidate = base;
  let n = 2;
  while (exists(`${runsRoot}/runs/${candidate}`)) {
    candidate = `${base}-${n++}`;
  }
  return candidate;
}

function exists(path: string): boolean {
  try {
    Deno.statSync(path);
    return true;
  } catch {
    return false;
  }
}

function writeJsonl(path: string, records: unknown[]): void {
  const body = records.map((r) => JSON.stringify(r)).join("\n") + "\n";
  Deno.writeTextFileSync(path, body);
}

export interface RunOneOutcome {
  result: BenchmarkResult;
  events: TrajectoryEvent[];
}

/** Execute one task against one adapter configuration. */
export async function runOne(
  task: TaskManifest,
  adapter: BenchmarkAdapter,
  opts: RunOptions,
  runIdHint = "",
): Promise<RunOneOutcome> {
  const runId = runIdHint || uniqueRunId(opts.runsRoot, adapter.configId, task.id);
  const runDir = `${opts.runsRoot}/runs/${runId}`;
  await Deno.mkdir(runDir, { recursive: true });

  const events: TrajectoryEvent[] = [];
  const started = isoNow();
  let prepared = false;
  let failureClass: FailureClass | null = null;
  let failureDetail: string | null = null;

  const record = (event: TrajectoryEvent): void => {
    try {
      validateTrajectoryEvent(event);
    } catch (err) {
      throw new Error(`invalid trajectory event: ${(err as Error).message}`);
    }
    events.push(event);
  };

  const workspace = new FixtureWorkspace({
    fixtureDir: `${opts.fixturesDir}/${task.fixture}`,
    runId,
    tmpParent: `${opts.runsRoot}/tmp`,
    task,
  });

  const signal = AbortSignal.timeout(task.timeout_ms);
  const ctx: AdapterRunContext = {
    runId,
    task,
    workspace,
    signal,
    record,
    time: isoNow,
    checkToolLimit: () => {
      const calls = events.filter((e) => e.type === "tool_call").length;
      if (calls >= task.max_tool_calls) throw new ToolCallLimitExceededError(task.max_tool_calls);
    },
  };

  record({
    type: "run",
    at: started,
    run_id: runId,
    task_id: task.id,
    category: task.category,
    config_id: adapter.configId,
    adapter_id: adapter.configId,
    fixture: task.fixture,
    fixture_revision: task.fixture_revision,
  });

  try {
    await workspace.prepare();
    prepared = true;
    const actualRevision = await computeFixtureRevision(`${opts.fixturesDir}/${task.fixture}`);
    if (actualRevision !== task.fixture_revision) {
      throw new FixtureRevisionMismatchError(task.id, task.fixture_revision, actualRevision);
    }
  } catch (err) {
    if (err instanceof FixtureRevisionMismatchError) {
      failureClass = "fixture_revision_mismatch";
      failureDetail = err.message;
    } else {
      failureClass = "adapter_error";
      failureDetail = `fixture preparation failed: ${(err as Error).message}`;
    }
  }

  if (failureClass === null) {
    try {
      await adapter.run(ctx);
    } catch (err) {
      if (err instanceof TaskTimeoutError) {
        failureClass = "timeout";
        failureDetail = err.message;
      } else if (err instanceof ToolCallLimitExceededError) {
        failureClass = "tool_call_limit";
        failureDetail = err.message;
      } else {
        failureClass = "adapter_error";
        failureDetail = (err as Error).message;
      }
    }
  }

  // Verification and oracle evaluation against the final workspace state.
  const verification = prepared
    ? await runVerification(task, workspace)
    : { ran: false, passed: false, command: null, exit_code: null, timed_out: false, output: null };
  record({
    type: "verify",
    at: isoNow(),
    command: verification.command ?? "(no verification command)",
    passed: verification.passed,
    exit_code: verification.exit_code ?? undefined,
    timed_out: verification.timed_out,
    output: verification.output ?? undefined,
  });

  const oracle = prepared
    ? await evaluateOracle(task, workspace)
    : { passed: false, checks: [{ kind: "file" as const, detail: "fixture workspace not prepared", passed: false }] };

  const metrics = deriveMetrics(events);
  const reliability = deriveReliability(events, { verificationCommand: task.verify?.command ?? null });
  const requiredCalls = {
    min_tool_calls: task.min_tool_calls,
    tool_calls: metrics.tool_calls,
    min_model_calls: task.min_model_calls,
    model_calls: metrics.model_calls,
    met: metrics.tool_calls >= task.min_tool_calls && metrics.model_calls >= task.min_model_calls,
  };

  if (failureClass === null) {
    if (!requiredCalls.met) {
      failureClass = "min_calls_not_met";
      failureDetail =
        `recorded ${metrics.tool_calls} tool calls (min ${task.min_tool_calls}) and ${metrics.model_calls} model calls (min ${task.min_model_calls})`;
    } else if (!verification.passed) {
      failureClass = "verification_failed";
      failureDetail = verification.timed_out
        ? `verification command timed out: ${verification.command}`
        : `verification command exited ${verification.exit_code}: ${verification.command}`;
    } else if (!oracle.passed) {
      failureClass = "verification_failed";
      failureDetail = `oracle checks failed: ${oracle.checks.filter((c) => !c.passed).map((c) => c.detail).join("; ")}`;
    }
  }

  const ended = isoNow();
  const result: BenchmarkResult = {
    schema_version: BENCHMARK_SCHEMA_VERSION,
    run_id: runId,
    task_id: task.id,
    config_id: adapter.configId,
    adapter_id: adapter.configId,
    fixture: task.fixture,
    fixture_revision: task.fixture_revision,
    started_at: started,
    ended_at: ended,
    wall_time_ms: Date.parse(ended) - Date.parse(started),
    success: failureClass === null,
    failure_class: failureClass,
    failure_detail: failureDetail,
    metrics,
    reliability: reliability.summary,
    verification,
    oracle,
    required_calls: requiredCalls,
    trajectory: `runs/${runId}/trajectory.jsonl`,
    created_at: ended,
  };
  validateBenchmarkResult(result);
  writeJsonl(`${runDir}/trajectory.jsonl`, events);
  writeJsonl(`${runDir}/result.jsonl`, [result]);

  try {
    await workspace.remove();
  } catch {
    // The disposable workspace may not exist when preparation failed early.
  }
  return { result, events };
}

// ---------------------------------------------------------------------------
// Full matrix
// ---------------------------------------------------------------------------

export async function runBenchmarks(opts: RunOptions): Promise<BenchmarkResult[]> {
  const tasks = loadTasks(opts.tasksDir);
  const selected = selectTasks(tasks, opts.taskSelectors);
  const adapters = opts.adapters ?? defaultAdapters();
  const chosen = opts.configs.includes("all") ? adapters : adapters.filter((a) => opts.configs.includes(a.configId));
  const missing = opts.configs.filter((c) => c !== "all" && !adapters.some((a) => a.configId === c));
  if (missing.length > 0) {
    throw new Error(
      `unknown configs: ${missing.join(", ")} (registered: ${adapters.map((a) => a.configId).join(", ")})`,
    );
  }
  const external = chosen.filter((a) => a.requiresExternalInference);
  if (external.length > 0) {
    throw new Error(
      `refusing to run external-inference adapters (${external.map((a) => a.configId).join(", ")}): ` +
        `the hermetic runner only executes deterministic adapters; live runs are staged and gated by m03/m05`,
    );
  }

  await Deno.mkdir(`${opts.runsRoot}/runs`, { recursive: true });
  await Deno.mkdir(`${opts.runsRoot}/tmp`, { recursive: true });

  let pairs = 0;
  const results: BenchmarkResult[] = [];
  for (const task of selected) {
    for (const adapter of chosen) {
      if (opts.limit !== undefined && pairs >= opts.limit) break;
      pairs++;
      const { result } = await runOne(task, adapter, opts);
      results.push(result);
      const icon = result.success ? "ok " : "FAIL";
      console.log(
        `  ${icon} ${result.config_id} ${result.task_id} ${result.wall_time_ms}ms ` +
          `tools=${result.metrics.tool_calls} errs=${result.metrics.tool_errors}` +
          (result.failure_class ? ` -> ${result.failure_class}: ${result.failure_detail}` : ""),
      );
    }
    if (opts.limit !== undefined && pairs >= opts.limit) break;
  }
  return results;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

if (import.meta.main) {
  try {
    const parsed = parseRunArgs(Deno.args);
    if ("help" in parsed) {
      console.log(RUN_HELP);
      Deno.exit(0);
    }
    console.log(
      `benchmark run: configs=[${parsed.configs.join(",")}] tasks=[${
        parsed.taskSelectors.join(",")
      }] runs=${parsed.runsRoot}`,
    );
    const results = await runBenchmarks(parsed);
    const summary = aggregateResults(results, parsed.runsRoot);
    console.log("");
    console.log(formatSummary(summary, true));
    if (results.some((r) => !r.success)) Deno.exit(1);
  } catch (err) {
    console.error(`error: ${(err as Error).message}`);
    console.error(RUN_HELP);
    Deno.exit(2);
  }
}
