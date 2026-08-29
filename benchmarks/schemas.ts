/**
 * Shared benchmark contracts for the Cerebras GPT-OSS Harmony agent foundation.
 *
 * This module is the single source of truth for the benchmark data model:
 * task manifests, trajectory JSONL events, per-run result records, and the
 * aggregated summary. Adapters A/B/C/D (m03), the canonical tool layer (m04)
 * and the reliability layer (m05) all consume these shapes, so keep this file
 * versioned (see BENCHMARK_SCHEMA_VERSION) and additive across minor bumps.
 *
 * Conventions:
 * - One JSON object per line for JSONL files (trajectory.jsonl, result.jsonl).
 * - Every trajectory event carries an `at` ISO-8601 timestamp.
 * - Tool calls are recorded as a `tool_call` event followed by exactly one
 *   `tool_result` event with the same `id`. Adapters must record every
 *   attempted tool invocation, including invalid ones.
 * - Adapters are passive: they emit events through the record sink. The
 *   runner derives metrics from the event stream; it never trusts model text
 *   or adapter prose for pass/fail decisions. Only declared oracles decide.
 */

export const BENCHMARK_SCHEMA_VERSION = "1.0";

/** Root directory (relative to the repository) that owns the benchmark subsystem. */
export const BENCHMARK_ROOT = "benchmarks";

/** Default directory for benchmark run artifacts. Git-ignored. */
export const DEFAULT_RUNS_ROOT = "benchmark-runs";

// ---------------------------------------------------------------------------
// Task manifest
// ---------------------------------------------------------------------------

export const TASK_CATEGORIES = ["navigation", "coding", "sequential", "failure", "long"] as const;
export type TaskCategory = (typeof TASK_CATEGORIES)[number];

/** A single step of a recorded (scripted) trail executed by the `reference` adapter. */
export interface TrailStep {
  /** Canonical tool name, e.g. filesystem.read, shell.exec, editor.apply_patch. */
  tool: string;
  /** Arguments passed to the tool. */
  args: Record<string, unknown>;
  /**
   * Deterministic assertion on the tool result. When absent the step only
   * records its events without asserting.
   */
  expect?: {
    /** Expected ok flag; defaults to true when `error_contains` is absent. */
    ok?: boolean;
    /** Every string must appear in the result output (only when ok). */
    output_contains?: string[];
    /** Must appear in the result error text (only when not ok). */
    error_contains?: string;
  };
  /** Fake deterministic failure injected instead of executing the tool. */
  inject?: { error: string; error_code?: string };
  /** Mark the call as a wrong-tool choice; the tool is not executed. */
  wrong?: boolean;
  /** Mark the call as an explicit duplicate of the previous call. */
  repeat?: boolean;
  /** Mark the step as a recovery attempt following an earlier tool error. */
  recovery?: boolean;
}

export type FileCheckKind = "exists" | "equals" | "contains" | "regex";

export interface FileCheck {
  path: string;
  kind: FileCheckKind;
  /** Required for equals/contains/regex. */
  value?: string;
  /** Invert the pass/fail decision. */
  invert?: boolean;
}

export type GitCheckKind = "commit_count" | "head_message" | "worktree_clean" | "file_committed";

export interface GitCheck {
  kind: GitCheckKind;
  /** commit_count: minimum commit count; head_message/file_committed: expected string. */
  value?: string;
}

export interface TaskOracle {
  file_checks?: FileCheck[];
  git_checks?: GitCheck[];
}

export interface TaskManifest {
  /** Stable identifier, e.g. `nav-001`. */
  id: string;
  category: TaskCategory;
  title: string;
  description: string;
  /** Fixture snapshot directory name under benchmarks/fixtures. */
  fixture: string;
  /** Content-addressed fixture revision: `sha256:<hex>`. */
  fixture_revision: string;
  /** Whole-run timeout (adapter + verification), milliseconds. */
  timeout_ms: number;
  /** Hard cap on recorded tool_call events; exceeding it fails the run. */
  max_tool_calls: number;
  /** Minimum recorded tool_call events required for success. */
  min_tool_calls: number;
  /** Minimum model requests required for success (0 for deterministic runs). */
  min_model_calls: number;
  /** Glob patterns for paths the adapter may write; `!` prefix negates. */
  allowed_write_scope: string[];
  /** Optional disposable git repository setup inside the workspace. */
  git?: {
    /** Initialize git and create a base commit from the fixture tree. */
    init: boolean;
    /**
     * Ordered list of full-tree snapshot directories (relative to the fixture
     * dir). When present the fixture root is not used as a working tree; each
     * snapshot is committed in order as the repository history.
     */
    history?: string[];
  };
  /** Declared verification command; must run successfully (exit 0) for success. */
  verify?: { command: string; timeout_ms?: number };
  /** Declared success oracle; additional deterministic checks must pass. */
  oracle?: TaskOracle;
  /** Optional recorded trail interpreted by the `reference` adapter. */
  scripted_trail?: TrailStep[];
}

// ---------------------------------------------------------------------------
// Trajectory events
// ---------------------------------------------------------------------------

export interface RunEvent {
  type: "run";
  at: string;
  run_id: string;
  task_id: string;
  category: TaskCategory;
  config_id: string;
  adapter_id: string;
  fixture: string;
  fixture_revision: string;
}

export interface ModelRequestEvent {
  type: "model_request";
  at: string;
  /** Monotonic request sequence number within the run. */
  id: number;
  model: string;
  message_count: number;
  input_tokens: number;
  output_tokens: number;
  /** Number of tools offered to the model. */
  tool_count: number;
}

export interface ModelResponseEvent {
  type: "model_response";
  at: string;
  request_id: number;
  content?: string;
  tool_calls?: { id: string; name: string; arguments: Record<string, unknown> }[];
  finish_reason?: string;
}

export interface ToolCallEvent {
  type: "tool_call";
  at: string;
  id: string;
  tool: string;
  arguments: Record<string, unknown>;
  /** False when the tool layer rejected the arguments before execution. */
  valid: boolean;
  invalid_reason?: string;
  /** True when the adapter flagged the call as the wrong tool choice. */
  is_wrong_tool?: boolean;
  /** True when the call duplicates the previous call (explicit or detected). */
  is_repeated?: boolean;
}

export interface ToolResultEvent {
  type: "tool_result";
  at: string;
  id: string;
  ok: boolean;
  /** stdout / file content on success. */
  output?: string;
  /** human-readable error text. */
  error?: string;
  /** Machine-readable error code: invalid_args, write_scope, exec_failed, unavailable, ... */
  error_code?: string;
  duration_ms?: number;
}

export interface VerifyEvent {
  type: "verify";
  at: string;
  command: string;
  passed: boolean;
  exit_code?: number;
  timed_out: boolean;
  output?: string;
}

/**
 * m05 additive event: a deterministic reliability-guard rejection (final
 * answer blocked by unmet verification/loop requirements, or a loop warning).
 * Readers that do not know this type must ignore the event; derivation always
 * tolerates unknown event types.
 */
export interface GuardEvent {
  type: "guard";
  at: string;
  /** Machine-readable guard kind. */
  kind: string;
  /** Deterministic one-line reason (model-facing text). */
  reason: string;
  /** Final-attempt number this guard rejected (0 for loop warnings). */
  attempt: number;
  /** Structured phase when the guard fired. */
  phase: string;
}

export type TrajectoryEvent =
  | RunEvent
  | ModelRequestEvent
  | ModelResponseEvent
  | ToolCallEvent
  | ToolResultEvent
  | VerifyEvent
  | GuardEvent;

// ---------------------------------------------------------------------------
// Result record (one line in result.jsonl)
// ---------------------------------------------------------------------------

export const FAILURE_CLASSES = [
  "timeout",
  "adapter_error",
  "tool_call_limit",
  "min_calls_not_met",
  "verification_failed",
  "fixture_revision_mismatch",
] as const;

export type FailureClass = (typeof FAILURE_CLASSES)[number];

export interface RunMetrics {
  model_calls: number;
  tool_calls: number;
  invalid_tool_calls: number;
  wrong_tool_calls: number;
  repeated_calls: number;
  tool_errors: number;
  recovery_attempts: number;
  input_tokens: number;
  output_tokens: number;
  /** Largest observed request context (input + output tokens of one request). */
  context_size: number;
}

export interface VerificationOutcome {
  ran: boolean;
  passed: boolean;
  command: string | null;
  exit_code: number | null;
  timed_out: boolean;
  output: string | null;
}

export interface OracleCheckOutcome {
  kind: "file" | "git" | "required_calls";
  detail: string;
  passed: boolean;
}

export interface OracleOutcome {
  passed: boolean;
  checks: OracleCheckOutcome[];
}

/**
 * m05 additive summary: the deterministic reliability evidence derived from
 * the event stream (never inferred from model text).  Optional so 1.0
 * artifacts written before m05 remain valid.
 */
export interface ReliabilitySummary {
  /** Structured task phase at the end of the run. */
  phase: string;
  final_accepted: boolean;
  /** Final answers rejected by the verification/loop guard. */
  guard_rejections: number;
  /** Final repetitions with no intervening action. */
  false_completions: number;
  /** Consecutive invalid tool calls at the end of the run. */
  invalid_call_streak: number;
  duplicate_calls: number;
  semantic_loops: number;
  retries: { attempts: number; allowed: number; rejected: number };
  unverified_writes: number;
  unresolved: { commands: number; edits: number };
  verification: { required: number; satisfied: number };
  /** m05 reliability failure class (advisory; the runner stays authoritative). */
  failure_class: string | null;
  /** Canonical structured-state contract (deterministic JSON). */
  state_contract: string;
}

export interface BenchmarkResult {
  schema_version: string;
  run_id: string;
  task_id: string;
  config_id: string;
  adapter_id: string;
  fixture: string;
  fixture_revision: string;
  started_at: string;
  ended_at: string;
  wall_time_ms: number;
  success: boolean;
  failure_class: FailureClass | null;
  failure_detail: string | null;
  metrics: RunMetrics;
  /** m05 additive reliability evidence (derived, never inferred from text). */
  reliability?: ReliabilitySummary;
  verification: VerificationOutcome;
  oracle: OracleOutcome;
  required_calls: {
    min_tool_calls: number;
    tool_calls: number;
    min_model_calls: number;
    model_calls: number;
    met: boolean;
  };
  /** Path of the trajectory JSONL file, relative to the runs root. */
  trajectory: string;
  created_at: string;
}

// ---------------------------------------------------------------------------
// Aggregated summary
// ---------------------------------------------------------------------------

export interface GroupMetrics {
  runs: number;
  successes: number;
  failures: number;
  success_rate: number;
  wall_time_ms: { median: number; p95: number };
  tool_calls: { median: number; max: number };
  model_calls: { median: number; max: number };
  total_tool_errors: number;
  total_recovery_attempts: number;
  total_invalid_tool_calls: number;
  total_repeated_calls: number;
  total_input_tokens: number;
  total_output_tokens: number;
  failure_classes: Record<string, number>;
}

export interface ConfigGroup extends GroupMetrics {
  config_id: string;
  task_count: number;
}

export interface TaskConfigGroup extends GroupMetrics {
  task_id: string;
  config_id: string;
}

export interface BenchmarkSummary {
  schema_version: string;
  generated_at: string;
  runs_root: string;
  run_count: number;
  success_count: number;
  failure_count: number;
  success_rate: number;
  by_config: ConfigGroup[];
  by_task_config: TaskConfigGroup[];
}

// ---------------------------------------------------------------------------
// Validation helpers (dependency-free; fail fast with actionable messages)
// ---------------------------------------------------------------------------

export class SchemaError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "SchemaError";
  }
}

function fail(path: string, message: string): never {
  throw new SchemaError(`${path}: ${message}`);
}

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

function expectInt(v: unknown, path: string, min = 0): number {
  if (typeof v !== "number" || !Number.isInteger(v) || v < min) {
    fail(path, `expected integer >= ${min}, got ${JSON.stringify(v)}`);
  }
  return v;
}

function expectString(v: unknown, path: string): string {
  if (typeof v !== "string" || v.length === 0) fail(path, `expected non-empty string, got ${JSON.stringify(v)}`);
  return v;
}

function expectStringArray(v: unknown, path: string): string[] {
  if (!Array.isArray(v) || v.some((x) => typeof x !== "string" || x.length === 0)) {
    fail(path, `expected array of non-empty strings, got ${JSON.stringify(v)}`);
  }
  return v as string[];
}

function expectOptionalString(v: unknown, path: string): string | undefined {
  if (v === undefined) return undefined;
  return expectString(v, path);
}

export function validateTaskManifest(m: unknown): TaskManifest {
  if (!isRecord(m)) fail("task", "expected object");
  const id = expectString(m.id, "id");
  expectString(m.title, "title");
  expectString(m.description, "description");
  if (!TASK_CATEGORIES.includes(m.category as TaskCategory)) {
    fail("category", `expected one of ${TASK_CATEGORIES.join(", ")}, got ${JSON.stringify(m.category)}`);
  }
  const fixture = expectString(m.fixture, "fixture");
  if (!/^sha256:[0-9a-f]{64}$/.test(String(m.fixture_revision ?? ""))) {
    fail("fixture_revision", `expected sha256:<hex>, got ${JSON.stringify(m.fixture_revision)}`);
  }
  expectInt(m.timeout_ms, "timeout_ms", 1);
  expectInt(m.max_tool_calls, "max_tool_calls", 1);
  expectInt(m.min_tool_calls, "min_tool_calls", 0);
  expectInt(m.min_model_calls, "min_model_calls", 0);
  if ((m.min_tool_calls as number) > (m.max_tool_calls as number)) {
    fail("min_tool_calls", "must not exceed max_tool_calls");
  }
  const allowedWriteScope = Array.isArray(m.allowed_write_scope) && m.allowed_write_scope.length > 0
    ? expectStringArray(m.allowed_write_scope, "allowed_write_scope")
    : (fail("allowed_write_scope", "must be a non-empty array of glob patterns") as never);

  // verify / oracle
  let verify: TaskManifest["verify"];
  if (m.verify !== undefined) {
    if (!isRecord(m.verify)) fail("verify", "expected object");
    verify = {
      command: expectString(m.verify.command, "verify.command"),
      timeout_ms: m.verify.timeout_ms === undefined
        ? undefined
        : expectInt(m.verify.timeout_ms, "verify.timeout_ms", 1),
    };
  }

  let oracle: TaskOracle | undefined;
  if (m.oracle !== undefined) {
    if (!isRecord(m.oracle)) fail("oracle", "expected object");
    oracle = {};
    if (m.oracle.file_checks !== undefined) {
      if (!Array.isArray(m.oracle.file_checks)) fail("oracle.file_checks", "expected array");
      oracle.file_checks = m.oracle.file_checks.map((c, i) => {
        if (!isRecord(c)) fail(`oracle.file_checks[${i}]`, "expected object");
        const kind = expectString(c.kind, `oracle.file_checks[${i}].kind`);
        if (!["exists", "equals", "contains", "regex"].includes(kind)) {
          fail(`oracle.file_checks[${i}].kind`, `unknown kind ${kind}`);
        }
        if (kind !== "exists" && c.value === undefined) {
          fail(`oracle.file_checks[${i}].value`, `required for kind ${kind}`);
        }
        if (c.invert !== undefined && typeof c.invert !== "boolean") {
          fail(`oracle.file_checks[${i}].invert`, "expected boolean");
        }
        return {
          path: expectString(c.path, `oracle.file_checks[${i}].path`),
          kind: kind as FileCheckKind,
          value: expectOptionalString(c.value, `oracle.file_checks[${i}].value`),
          invert: c.invert === undefined ? undefined : (c.invert as boolean),
        };
      });
    }
    if (m.oracle.git_checks !== undefined) {
      if (!Array.isArray(m.oracle.git_checks)) fail("oracle.git_checks", "expected array");
      oracle.git_checks = m.oracle.git_checks.map((c, i) => {
        if (!isRecord(c)) fail(`oracle.git_checks[${i}]`, "expected object");
        const kind = expectString(c.kind, `oracle.git_checks[${i}].kind`);
        if (!["commit_count", "head_message", "worktree_clean", "file_committed"].includes(kind)) {
          fail(`oracle.git_checks[${i}].kind`, `unknown kind ${kind}`);
        }
        return {
          kind: kind as GitCheckKind,
          value: expectOptionalString(c.value, `oracle.git_checks[${i}].value`),
        };
      });
    }
  }

  // git
  let git: TaskManifest["git"];
  if (m.git !== undefined) {
    if (!isRecord(m.git)) fail("git", "expected object");
    git = {
      init: Boolean(m.git.init),
      history: m.git.history === undefined ? undefined : expectStringArray(m.git.history, "git.history"),
    };
  }

  // trail
  let trail: TrailStep[] | undefined;
  if (m.scripted_trail !== undefined) {
    if (!Array.isArray(m.scripted_trail)) fail("scripted_trail", "expected array");
    trail = m.scripted_trail.map((s, i) => {
      if (!isRecord(s)) fail(`scripted_trail[${i}]`, "expected object");
      const step: TrailStep = {
        tool: expectString(s.tool, `scripted_trail[${i}].tool`),
        args: isRecord(s.args) ? s.args : fail(`scripted_trail[${i}].args`, "expected object"),
      };
      if (s.expect !== undefined) {
        if (!isRecord(s.expect)) fail(`scripted_trail[${i}].expect`, "expected object");
        step.expect = {
          ok: s.expect.ok === undefined ? undefined : Boolean(s.expect.ok),
          output_contains: s.expect.output_contains === undefined
            ? undefined
            : expectStringArray(s.expect.output_contains, `scripted_trail[${i}].expect.output_contains`),
          error_contains: expectOptionalString(s.expect.error_contains, `scripted_trail[${i}].expect.error_contains`),
        };
      }
      if (s.inject !== undefined) {
        if (!isRecord(s.inject)) fail(`scripted_trail[${i}].inject`, "expected object");
        step.inject = {
          error: expectString(s.inject.error, `scripted_trail[${i}].inject.error`),
          error_code: expectOptionalString(s.inject.error_code, `scripted_trail[${i}].inject.error_code`),
        };
      }
      if (s.wrong !== undefined) step.wrong = Boolean(s.wrong);
      if (s.repeat !== undefined) step.repeat = Boolean(s.repeat);
      if (s.recovery !== undefined) step.recovery = Boolean(s.recovery);
      return step;
    });
  }

  return {
    id,
    category: m.category as TaskCategory,
    title: m.title as string,
    description: m.description as string,
    fixture,
    fixture_revision: m.fixture_revision as string,
    timeout_ms: m.timeout_ms as number,
    max_tool_calls: m.max_tool_calls as number,
    min_tool_calls: m.min_tool_calls as number,
    min_model_calls: m.min_model_calls as number,
    allowed_write_scope: allowedWriteScope,
    git,
    verify,
    oracle,
    scripted_trail: trail,
  };
}

export function validateTrajectoryEvent(e: unknown): TrajectoryEvent {
  if (!isRecord(e)) fail("event", "expected object");
  const type = expectString(e.type, "type");
  expectString(e.at, "at");
  switch (type) {
    case "run":
      expectString(e.run_id, "run_id");
      expectString(e.task_id, "task_id");
      expectString(e.config_id, "config_id");
      expectString(e.adapter_id, "adapter_id");
      expectString(e.fixture, "fixture");
      expectString(e.fixture_revision, "fixture_revision");
      return e as unknown as RunEvent;
    case "model_request":
      expectInt(e.id, "id", 1);
      expectString(e.model, "model");
      expectInt(e.message_count, "message_count", 0);
      expectInt(e.input_tokens, "input_tokens", 0);
      expectInt(e.output_tokens, "output_tokens", 0);
      expectInt(e.tool_count, "tool_count", 0);
      return e as unknown as ModelRequestEvent;
    case "model_response":
      expectInt(e.request_id, "request_id", 1);
      return e as unknown as ModelResponseEvent;
    case "tool_call": {
      expectString(e.id, "id");
      expectString(e.tool, "tool");
      if (!isRecord(e.arguments)) fail("arguments", "expected object");
      if (typeof e.valid !== "boolean") fail("valid", "expected boolean");
      return e as unknown as ToolCallEvent;
    }
    case "tool_result":
      expectString(e.id, "id");
      if (typeof e.ok !== "boolean") fail("ok", "expected boolean");
      return e as unknown as ToolResultEvent;
    case "verify":
      expectString(e.command, "command");
      if (typeof e.passed !== "boolean") fail("passed", "expected boolean");
      if (typeof e.timed_out !== "boolean") fail("timed_out", "expected boolean");
      return e as unknown as VerifyEvent;
    case "guard":
      expectString(e.kind, "kind");
      expectString(e.reason, "reason");
      expectInt(e.attempt, "attempt", 0);
      expectString(e.phase, "phase");
      return e as unknown as GuardEvent;
    default:
      fail("type", `unknown event type ${type}`);
  }
}

export function validateBenchmarkResult(r: unknown): BenchmarkResult {
  if (!isRecord(r)) fail("result", "expected object");
  if (r.schema_version !== BENCHMARK_SCHEMA_VERSION) {
    fail("schema_version", `expected ${BENCHMARK_SCHEMA_VERSION}, got ${JSON.stringify(r.schema_version)}`);
  }
  expectString(r.run_id, "run_id");
  expectString(r.task_id, "task_id");
  expectString(r.config_id, "config_id");
  expectString(r.adapter_id, "adapter_id");
  expectString(r.fixture, "fixture");
  expectString(r.fixture_revision, "fixture_revision");
  expectString(r.started_at, "started_at");
  expectString(r.ended_at, "ended_at");
  expectInt(r.wall_time_ms, "wall_time_ms", 0);
  if (typeof r.success !== "boolean") fail("success", "expected boolean");
  if (r.failure_class !== null) {
    if (!FAILURE_CLASSES.includes(r.failure_class as FailureClass)) {
      fail("failure_class", `unknown class ${JSON.stringify(r.failure_class)}`);
    }
  }
  if (!isRecord(r.metrics)) fail("metrics", "expected object");
  if (!isRecord(r.verification)) fail("verification", "expected object");
  if (!isRecord(r.oracle)) fail("oracle", "expected object");
  if (!isRecord(r.required_calls)) fail("required_calls", "expected object");
  if (r.reliability !== undefined && !isRecord(r.reliability)) fail("reliability", "expected object");
  expectString(r.trajectory, "trajectory");
  expectString(r.created_at, "created_at");
  return r as unknown as BenchmarkResult;
}
