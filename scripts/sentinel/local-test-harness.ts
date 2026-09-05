// Canonical local Provider Sentinel verification harness.
//
// This is the single source of truth for local Sentinel verification and the
// exact command CI repeats (`deno task sentinel:test-local`). It runs the fixed
// Sentinel test order below in fail-fast sequence, then the same fmt, lint, and
// build checks CI relies on, prints one concise timing/status line per stage,
// preserves full child output when a stage fails, and writes a machine-readable
// report to the ignored `.sentinel/local-test/result.json`.
//
// Isolation boundary — exactly what each mechanism establishes for every stage
// child process:
//   - `clearEnv: true` drops every inherited variable: the child receives
//     exactly the supplied environment map. The scrubber removes the recognized
//     credential names (exact list, the GITHUB_ / DENO_DEPLOY_TOKEN_ prefixes,
//     and the _TOKEN / _KEY / _SECRET / _PASSWORD / _PASS suffixes); it is a
//     name-based filter and does not detect every possible secret name.
//   - `--cached-only` (on the stages that carry it) refuses to fetch modules
//     that are not already cached, so nothing new is downloaded; `--frozen`
//     refuses lockfile changes but does not by itself prevent downloads, and
//     the build stage additionally denies the configured registries.
//   - omitted network permission (no `--allow-net`): the Deno permission model
//     denies network APIs to the Deno stage process itself, and the static
//     stage check below rejects any stage argv granting network or
//     all-permission access.
// These mechanisms bound the Deno stage process and its environment. They are
// NOT an operating-system sandbox for arbitrary child processes: fixture
// stages run with `--allow-run` and may spawn child executables (Deno, git,
// sha256sum, ...) that are not governed by the parent's Deno permission flags.

export const SENTINEL_LOCAL_TEST_COMMAND = "deno task sentinel:test-local";
export const SENTINEL_LOCAL_TEST_REPORT_PATH = ".sentinel/local-test/result.json";

export interface SentinelLocalTestStage {
  readonly name: string;
  readonly description: string;
  readonly argv: readonly string[];
}

// The fixed fail-fast order is an immutable contract: workflow-contract,
// rolling-review, artifact-recovery (with the read/write/run permissions its
// fixtures require so they do not self-ignore), recovery/controller, matrix,
// Luna policy/orchestrator, rollback, then fmt check, lint, and build.
export const SENTINEL_LOCAL_TEST_STAGES: readonly SentinelLocalTestStage[] = [
  {
    name: "workflow-contract",
    description: "Sentinel workflow contract tests",
    argv: ["test", "--cached-only", "--frozen", "tests/sentinel-workflow-contract.test.ts"],
  },
  {
    name: "rolling-review",
    description: "rolling review tests",
    argv: ["test", "--cached-only", "--frozen", "tests/sentinel-rolling-review.test.ts"],
  },
  {
    name: "artifact-recovery",
    description: "artifact-recovery fixture tests",
    argv: [
      "test",
      "--cached-only",
      "--frozen",
      // The fixture tests gate on bare read/write/run permission; a scoped
      // run grant reports `prompt` and makes every fixture self-ignore.
      "--allow-env",
      "--allow-read",
      "--allow-write",
      "--allow-run",
      "tests/sentinel-artifact-recovery.test.ts",
    ],
  },
  {
    name: "recovery",
    description: "recovery-controller, bootstrap, and local-harness tests",
    argv: [
      "test",
      "--cached-only",
      "--frozen",
      // The bootstrap source-inspection, isolation, and local-harness
      // regression tests read the tree, build temporary trees/fixtures, and
      // spawn Deno children; a scoped run grant would make them self-ignore.
      "--allow-read",
      "--allow-write",
      "--allow-run",
      "tests/sentinel-recovery.test.ts",
      "tests/sentinel-bootstrap.test.ts",
      "tests/sentinel-bootstrap-isolation.test.ts",
      "tests/sentinel-provider-state.test.ts",
      "tests/sentinel-local-test-harness.test.ts",
    ],
  },
  {
    name: "matrix",
    description: "matrix plan, cell, and integration tests",
    argv: [
      "test",
      "--cached-only",
      "--frozen",
      "--allow-read",
      "--allow-write",
      "--allow-run",
      "tests/sentinel-matrix.test.ts",
      "tests/sentinel-matrix-cell.test.ts",
      "tests/sentinel-matrix-integrate.test.ts",
    ],
  },
  {
    name: "luna-orchestrator",
    description: "Luna policy and orchestrator tests",
    argv: [
      "test",
      "--cached-only",
      "--frozen",
      "--allow-read",
      "--allow-write",
      "--allow-run",
      "tests/sentinel-orchestrator.test.ts",
    ],
  },
  {
    name: "rollback",
    description: "automatic rollback controller and Deno route ownership tests",
    argv: [
      "test",
      "--cached-only",
      "--frozen",
      "tests/sentinel-rollback-controller.test.ts",
      "tests/sentinel-deploy.test.ts",
    ],
  },
  {
    name: "fmt",
    description: "formatting check",
    argv: ["fmt", "--check", "serve.ts", "src", "tests", "scripts", "docs", "benchmarks"],
  },
  {
    name: "lint",
    description: "lint",
    argv: ["lint", "serve.ts", "src", "tests", "scripts", "benchmarks"],
  },
  {
    name: "build",
    description: "type-check build",
    argv: [
      "check",
      "--frozen",
      // The pinned CI Deno (2.9.5) does not offer --cached-only on `deno
      // check`, so the build denies the registries the project can reach:
      // cached resolutions still work, uncached ones fail closed.
      "--deny-import=jsr.io,registry.npmjs.org,deno.land,esm.sh",
      "serve.ts",
      "scripts/setup-instance.ts",
      "scripts/sentinel/main.ts",
      "scripts/sentinel/rolling-review-worker.ts",
      "scripts/sentinel/revision-control.ts",
      "scripts/sentinel/rollback-controller.ts",
      "scripts/sentinel/bootstrap/main.ts",
      "scripts/sentinel/bootstrap/revision-control.ts",
    ],
  },
];

// Environment keys that grant access to GitHub, Deno, or model/provider
// infrastructure and are never passed to child processes. The parent harness
// may run inside an agent or Actions environment that exports these. The
// filter is name-based: an unrecognized secret name is not detected.
const SCRUBBED_ENVIRONMENT_EXACT = [
  "GITHUB_TOKEN",
  "GH_TOKEN",
  "DENO_DEPLOY_TOKEN",
  "DENO_KV_ACCESS_TOKEN",
  "DENO_AUTH_TOKENS",
  "OPENAI_API_KEY",
  "CEREBRAS_API_KEY",
  "VOYAGEAI_API_KEY",
  "METERED_API_KEY",
  "SURPLUS_API_KEY",
  "OPENLUX_API_KEY",
  "UOS_AI_TOKEN",
  "PREVIEW_UOS_AI_USER_TOKEN",
  "UBIQUITY_AI_USER_TOKEN",
  "CODEX_AUTH_JSON_B64",
  "SENTINEL_ARTIFACT_KEY",
  "SENTINEL_CODEX_AUTH_STATE_KEY",
  "SENTINEL_REPLAY_KEY",
  "SENTINEL_CODEX_AUTH_SLOT_1_B64",
  "SENTINEL_CODEX_AUTH_SLOT_2_B64",
] as const;

const SCRUBBED_ENVIRONMENT_PREFIXES = ["GITHUB_", "DENO_DEPLOY_TOKEN_"] as const;
const SCRUBBED_ENVIRONMENT_SUFFIXES = ["_TOKEN", "_KEY", "_SECRET", "_PASSWORD", "_PASS"] as const;

export function isScrubbedEnvironmentKey(key: string): boolean {
  if ((SCRUBBED_ENVIRONMENT_EXACT as readonly string[]).includes(key)) return true;
  if (SCRUBBED_ENVIRONMENT_PREFIXES.some((prefix) => key.startsWith(prefix))) return true;
  return SCRUBBED_ENVIRONMENT_SUFFIXES.some((suffix) => key.endsWith(suffix));
}

export function scrubSentinelLocalTestEnvironment(
  environment: Readonly<Record<string, string>>,
): Record<string, string> {
  const scrubbed: Record<string, string> = {};
  for (const [key, value] of Object.entries(environment)) {
    if (!isScrubbedEnvironmentKey(key)) scrubbed[key] = value;
  }
  return scrubbed;
}

/**
 * Runs one stage at the actual hermetic child boundary: the child receives
 * exactly the scrubbed environment and nothing else. `clearEnv: true` is
 * required here — Deno.Command would otherwise overlay `env` on the inherited
 * environment, so scrubbing alone does NOT remove credentials that exist in
 * the parent. The child keeps every non-credential (toolchain) variable from
 * the parent environment that survives scrubbing.
 */
export function runSentinelLocalTestChild(
  stage: SentinelLocalTestStage,
  environment: Readonly<Record<string, string>> = Deno.env.toObject(),
): Promise<Deno.CommandOutput> {
  const command = new Deno.Command(Deno.execPath(), {
    args: [...stage.argv],
    cwd: Deno.cwd(),
    env: scrubSentinelLocalTestEnvironment(environment),
    clearEnv: true,
    stdout: "piped",
    stderr: "piped",
  });
  return command.output();
}

// Tokens that would permit network access, an all-permission child, or paid
// model/deployment tooling. The stage list is static, so a violation of this
// contract is a programming error that fails before anything runs.
const FORBIDDEN_STAGE_OPTIONS = ["--allow-net", "--allow-all", "-A", "--allow-import", "--allow-ffi"] as const;
const FORBIDDEN_STAGE_TOKENS = ["codex", "deploy", "promote", "gitleaks", "bwrap"] as const;

export function validateSentinelLocalTestStage(stage: SentinelLocalTestStage): string | null {
  for (const argument of stage.argv) {
    if (FORBIDDEN_STAGE_OPTIONS.some((option) => argument === option || argument.startsWith(`${option}=`))) {
      return `stage "${stage.name}" permits network or all-permission access (${argument})`;
    }
    // The fixed `deno test` command consumes this one control-plane test
    // module; its name must not read as an invocation of the deployment CLI.
    const isDeployTestInput = stage.argv[0] === "test" && argument === "tests/sentinel-deploy.test.ts";
    if (!isDeployTestInput && FORBIDDEN_STAGE_TOKENS.some((token) => argument.includes(token))) {
      return `stage "${stage.name}" invokes paid, model, or deployment tooling (${argument})`;
    }
  }
  return null;
}

export type SentinelLocalTestStageStatus = "passed" | "failed" | "skipped";
export type SentinelLocalTestStatus = "passed" | "failed";

export interface SentinelLocalTestStageResult {
  readonly name: string;
  readonly status: SentinelLocalTestStageStatus;
  readonly durationMs: number;
  readonly exitCode: number | null;
}

export interface SentinelLocalTestRun {
  readonly status: SentinelLocalTestStatus;
  readonly stages: readonly SentinelLocalTestStageResult[];
}

export interface SentinelLocalTestStageOutcome {
  readonly exitCode: number;
}

export type SentinelLocalTestStageRunner = (
  stage: SentinelLocalTestStage,
) => Promise<SentinelLocalTestStageOutcome>;

export type SentinelLocalTestStageObserver = (
  result: SentinelLocalTestStageResult,
  stage: SentinelLocalTestStage,
) => void;

// Runs stages in declaration order and stops at the first failure. Every stage
// after a failure is reported as skipped so the report always covers the full
// contract.
export async function runSentinelLocalTestStages(
  stages: readonly SentinelLocalTestStage[],
  runStage: SentinelLocalTestStageRunner,
  observe?: SentinelLocalTestStageObserver,
): Promise<SentinelLocalTestRun> {
  const results: SentinelLocalTestStageResult[] = [];
  for (const stage of stages) {
    const startedMs = performance.now();
    let status: SentinelLocalTestStageStatus;
    let exitCode: number;
    try {
      const outcome = await runStage(stage);
      exitCode = outcome.exitCode;
      status = outcome.exitCode === 0 ? "passed" : "failed";
    } catch (error) {
      exitCode = 1;
      status = "failed";
      console.error(`[${SENTINEL_LOCAL_TEST_COMMAND}] stage "${stage.name}" runner failed: ${String(error)}`);
    }
    const result: SentinelLocalTestStageResult = {
      name: stage.name,
      status,
      durationMs: Math.round(performance.now() - startedMs),
      exitCode,
    };
    results.push(result);
    observe?.(result, stage);
    if (status === "failed") {
      for (const skipped of stages.slice(results.length)) {
        results.push({ name: skipped.name, status: "skipped", durationMs: 0, exitCode: null });
      }
      return { status: "failed", stages: results };
    }
  }
  return { status: "passed", stages: results };
}

export interface SentinelLocalTestReportV1 {
  readonly schema_version: 1;
  readonly command: string;
  readonly started_at: string;
  readonly finished_at: string;
  readonly duration_ms: number;
  readonly status: SentinelLocalTestStatus;
  readonly stages: readonly SentinelLocalTestStageResult[];
}

export function buildSentinelLocalTestReport(
  run: SentinelLocalTestRun,
  startedAtMs: number,
  finishedAtMs: number,
): SentinelLocalTestReportV1 {
  return {
    schema_version: 1,
    command: SENTINEL_LOCAL_TEST_COMMAND,
    started_at: new Date(startedAtMs).toISOString(),
    finished_at: new Date(finishedAtMs).toISOString(),
    duration_ms: finishedAtMs - startedAtMs,
    status: run.status,
    stages: run.stages,
  };
}

export function formatSentinelLocalTestDuration(durationMs: number): string {
  return `${(durationMs / 1000).toFixed(1)}s`;
}

async function main(): Promise<void> {
  for (const stage of SENTINEL_LOCAL_TEST_STAGES) {
    const violation = validateSentinelLocalTestStage(stage);
    if (violation !== null) throw new Error(violation);
  }

  const startedAtMs = Date.now();
  const childOutput = new Map<string, string>();
  const decoder = new TextDecoder();

  const runStage: SentinelLocalTestStageRunner = async (stage) => {
    try {
      const outcome = await runSentinelLocalTestChild(stage);
      if (outcome.code !== 0) {
        const output = decoder.decode(outcome.stdout).replace(/\s+$/u, "");
        const error = decoder.decode(outcome.stderr).replace(/\s+$/u, "");
        childOutput.set(stage.name, [output, error].filter((part) => part.length > 0).join("\n\n"));
      }
      return { exitCode: outcome.code };
    } catch (error) {
      childOutput.set(stage.name, `child process could not start: ${String(error)}`);
      return { exitCode: 1 };
    }
  };

  const run = await runSentinelLocalTestStages(
    SENTINEL_LOCAL_TEST_STAGES,
    runStage,
    (result, stage) => {
      const index = SENTINEL_LOCAL_TEST_STAGES.findIndex((candidate) => candidate.name === result.name) + 1;
      const status = result.status === "passed" ? "ok" : result.status.toUpperCase();
      console.log(
        `[${SENTINEL_LOCAL_TEST_COMMAND}] ${index}/${SENTINEL_LOCAL_TEST_STAGES.length} ` +
          `${stage.description} [${result.name}] ${status} (${formatSentinelLocalTestDuration(result.durationMs)})`,
      );
      if (result.status === "failed") {
        const output = childOutput.get(result.name);
        if (output !== undefined) {
          console.log(`[${SENTINEL_LOCAL_TEST_COMMAND}] preserving child output for "${result.name}":`);
          console.log(output);
        }
      }
    },
  );

  const finishedAtMs = Date.now();
  const report = buildSentinelLocalTestReport(run, startedAtMs, finishedAtMs);
  await Deno.mkdir(".sentinel/local-test", { recursive: true });
  await Deno.writeTextFile(SENTINEL_LOCAL_TEST_REPORT_PATH, `${JSON.stringify(report, null, 2)}\n`);
  console.log(
    `[${SENTINEL_LOCAL_TEST_COMMAND}] ${run.status} after ${run.stages.length} stages ` +
      `in ${formatSentinelLocalTestDuration(finishedAtMs - startedAtMs)} ` +
      `(report: ${SENTINEL_LOCAL_TEST_REPORT_PATH})`,
  );
  if (run.status === "failed") Deno.exit(1);
}

if (import.meta.main) {
  await main();
}
