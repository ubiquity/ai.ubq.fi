import assert from "node:assert/strict";
import {
  buildSentinelLocalTestReport,
  formatSentinelLocalTestDuration,
  isScrubbedEnvironmentKey,
  runSentinelLocalTestChild,
  runSentinelLocalTestStages,
  scrubSentinelLocalTestEnvironment,
  SENTINEL_LOCAL_TEST_COMMAND,
  SENTINEL_LOCAL_TEST_REPORT_PATH,
  SENTINEL_LOCAL_TEST_STAGES,
  type SentinelLocalTestStage,
  type SentinelLocalTestStageRunner,
  validateSentinelLocalTestStage,
} from "../scripts/sentinel/local-test-harness.ts";

const credentialEnvironment = (): Record<string, string> => ({
  PATH: "/usr/bin:/bin",
  HOME: "/home/test",
  DENO_DIR: "/home/test/.cache/deno",
  TMPDIR: "/tmp",
  LANG: "C.UTF-8",
  GITHUB_TOKEN: "ghs_test",
  GITHUB_TOKEN_SIMULANT: "ghs_simulant",
  GITHUB_ACTIONS: "true",
  GITHUB_REPOSITORY: "acme/repo",
  DENO_DEPLOY_TOKEN: "ddo_test",
  DENO_DEPLOY_TOKEN_UBIQUITY_DAO: "ddo_org",
  DENO_DEPLOY_TOKEN_UBIQUITY_OS: "ddo_os",
  DENO_KV_ACCESS_TOKEN: "kvt_test",
  DENO_AUTH_TOKENS: "jsr=secret",
  OPENAI_API_KEY: "sk-test",
  CEREBRAS_API_KEY: "cerebras-test",
  VOYAGEAI_API_KEY: "voyage-test",
  METERED_API_KEY: "metered-test",
  SURPLUS_API_KEY: "surplus-test",
  UOS_AI_TOKEN: "uos-test",
  PREVIEW_UOS_AI_USER_TOKEN: "preview-test",
  UBIQUITY_AI_USER_TOKEN: "production-test",
  CODEX_AUTH_JSON_B64: "eyJ0b2tlbnMiOnt9fQ==",
  SENTINEL_ARTIFACT_KEY: "sentinel-artifact-test",
  SENTINEL_CODEX_AUTH_STATE_KEY: "sentinel-auth-state-test",
  SENTINEL_REPLAY_KEY: "sentinel-replay-test",
  SENTINEL_CODEX_AUTH_SLOT_1_B64: "slot-1",
  SENTINEL_CODEX_AUTH_SLOT_2_B64: "slot-2",
  NODE_AUTH_TOKEN: "npm-test",
  BACKBLAZE_MASTER_KEY: "blz-test",
});

Deno.test("the local harness runs the Sentinel stages in the fixed fail-fast order", () => {
  assert.deepEqual(
    SENTINEL_LOCAL_TEST_STAGES.map((stage) => stage.name),
    [
      "workflow-contract",
      "rolling-review",
      "artifact-recovery",
      "recovery",
      "matrix",
      "luna-orchestrator",
      "rollback",
      "fmt",
      "lint",
      "build",
    ],
    "the stage order is an immutable verification contract",
  );
  // fmt check, lint, and build always follow every test group.
  const testStageNames = SENTINEL_LOCAL_TEST_STAGES.slice(0, -3).map((stage) => stage.name);
  const requirementOrder = [
    "workflow-contract",
    "rolling-review",
    "artifact-recovery",
    "recovery",
    "matrix",
    "luna-orchestrator",
    "rollback",
  ];
  assert.deepEqual(testStageNames, requirementOrder);
  assert.deepEqual(
    SENTINEL_LOCAL_TEST_STAGES.slice(-3).map((stage) => stage.name),
    ["fmt", "lint", "build"],
  );
  // No stage may grant network or all-permission access, or invoke model,
  // deployment, or secret-scan tooling.
  for (const stage of SENTINEL_LOCAL_TEST_STAGES) {
    assert.equal(validateSentinelLocalTestStage(stage), null, `stage "${stage.name}" is not hermetic`);
  }
  // The fixture stages run with the permissions their fixtures require.
  const artifactRecovery = SENTINEL_LOCAL_TEST_STAGES.find((stage) => stage.name === "artifact-recovery");
  assert.ok(artifactRecovery);
  for (const permission of ["--allow-env", "--allow-read", "--allow-write", "--allow-run"]) {
    assert.ok(artifactRecovery.argv.includes(permission), "artifact-recovery fixture tests must not self-ignore");
  }
  const lunaOrchestrator = SENTINEL_LOCAL_TEST_STAGES.find((stage) => stage.name === "luna-orchestrator");
  assert.ok(lunaOrchestrator);
  for (const permission of ["--allow-read", "--allow-write", "--allow-run"]) {
    assert.ok(lunaOrchestrator.argv.includes(permission), `luna-orchestrator stage must request ${permission}`);
  }
  assert.equal(
    lunaOrchestrator.argv.includes("--allow-env"),
    false,
    "luna-orchestrator tests do not need environment access",
  );
  // The recovery stage is the canonical home of the bootstrap package tests:
  // the source-inspection, isolation, and local-harness regression tests read
  // the tree, build temporary trees/fixtures, and spawn Deno children.
  const recovery = SENTINEL_LOCAL_TEST_STAGES.find((stage) => stage.name === "recovery");
  assert.ok(recovery);
  for (const permission of ["--allow-read", "--allow-write", "--allow-run"]) {
    assert.ok(recovery.argv.includes(permission), `recovery stage must request ${permission}`);
  }
  for (
    const testFile of [
      "tests/sentinel-recovery.test.ts",
      "tests/sentinel-bootstrap.test.ts",
      "tests/sentinel-bootstrap-isolation.test.ts",
      "tests/sentinel-local-test-harness.test.ts",
    ]
  ) {
    assert.ok(recovery.argv.includes(testFile), `recovery stage must run ${testFile}`);
  }
  // The build checks include the pinned bootstrap entrypoint and its closed
  // local import graph.
  const build = SENTINEL_LOCAL_TEST_STAGES.find((stage) => stage.name === "build");
  assert.ok(build);
  assert.ok(
    build.argv.includes("scripts/sentinel/bootstrap/main.ts"),
    "build stage must check the bootstrap entrypoint",
  );
});

Deno.test("credential scrubbing keeps GitHub, Deno, and model credentials out of child environments", () => {
  const environment = credentialEnvironment();
  assert.ok(isScrubbedEnvironmentKey("GITHUB_TOKEN"));
  assert.ok(isScrubbedEnvironmentKey("OPENAI_API_KEY"));
  assert.ok(isScrubbedEnvironmentKey("DENO_DEPLOY_TOKEN"));
  assert.ok(!isScrubbedEnvironmentKey("PATH"));
  assert.ok(!isScrubbedEnvironmentKey("DENO_DIR"));
  const scrubbed = scrubSentinelLocalTestEnvironment(environment);
  for (const key of Object.keys(environment)) {
    if (
      [
        "PATH",
        "HOME",
        "DENO_DIR",
        "TMPDIR",
        "LANG",
      ].includes(key)
    ) continue;
    assert.equal(scrubbed[key], undefined, `credential ${key} leaked into the child environment`);
  }
  assert.equal(scrubbed.PATH, environment.PATH);
  assert.equal(scrubbed.HOME, environment.HOME);
  assert.equal(scrubbed.DENO_DIR, environment.DENO_DIR);
  assert.equal(scrubbed.TMPDIR, environment.TMPDIR);
  assert.equal(scrubbed.LANG, environment.LANG);
});

Deno.test("fail-fast stops at the first failed stage and skips the remaining contract", async () => {
  const stages: readonly SentinelLocalTestStage[] = [
    { name: "first", description: "first stage", argv: ["test", "first.test.ts"] },
    { name: "second", description: "second stage", argv: ["test", "second.test.ts"] },
    { name: "third", description: "third stage", argv: ["test", "third.test.ts"] },
    { name: "fourth", description: "fourth stage", argv: ["test", "fourth.test.ts"] },
  ];
  const attempted: string[] = [];
  const runner: SentinelLocalTestStageRunner = (stage) => {
    attempted.push(stage.name);
    return Promise.resolve({ exitCode: stage.name === "second" ? 1 : 0 });
  };
  const failed = await runSentinelLocalTestStages(stages, runner);
  assert.equal(failed.status, "failed");
  assert.deepEqual(attempted, ["first", "second"], "stages after the failure must not run");
  assert.deepEqual(
    failed.stages.map((result) => [result.name, result.status]),
    [["first", "passed"], ["second", "failed"], ["third", "skipped"], ["fourth", "skipped"]],
  );
  assert.equal(failed.stages[1]!.exitCode, 1);
  assert.ok(failed.stages[1]!.durationMs >= 0);

  const succeededAttempts: string[] = [];
  const succeeded = await runSentinelLocalTestStages(stages, (stage) => {
    succeededAttempts.push(stage.name);
    return Promise.resolve({ exitCode: 0 });
  });
  assert.equal(succeeded.status, "passed");
  assert.deepEqual(succeededAttempts, ["first", "second", "third", "fourth"]);
  assert.ok(succeeded.stages.every((result) => result.status === "passed"));
});

Deno.test("the harness writes one machine-readable JSON report for every run", () => {
  assert.equal(SENTINEL_LOCAL_TEST_COMMAND, "deno task sentinel:test-local");
  assert.equal(SENTINEL_LOCAL_TEST_REPORT_PATH, ".sentinel/local-test/result.json");
  const run = {
    status: "failed" as const,
    stages: [
      { name: "workflow-contract", status: "passed" as const, durationMs: 1200, exitCode: 0 },
      { name: "rolling-review", status: "failed" as const, durationMs: 500, exitCode: 1 },
      { name: "artifact-recovery", status: "skipped" as const, durationMs: 0, exitCode: null },
    ],
  };
  const report = buildSentinelLocalTestReport(run, Date.UTC(2026, 0, 1, 0, 0, 0), Date.UTC(2026, 0, 1, 0, 0, 2));
  const parsed = JSON.parse(JSON.stringify(report)) as Record<string, unknown>;
  assert.equal(parsed.schema_version, 1);
  assert.equal(parsed.command, "deno task sentinel:test-local");
  assert.equal(parsed.status, "failed");
  assert.equal(parsed.duration_ms, 2000);
  assert.equal((parsed.stages as Array<Record<string, unknown>>).length, 3);
  assert.equal((parsed.stages as Array<Record<string, unknown>>)[2]!.status, "skipped");
  assert.equal(formatSentinelLocalTestDuration(2000), "2.0s");
});

const runPermission = await Deno.permissions.query({ name: "run" });
const childProcessUnavailable = runPermission.state !== "granted";

Deno.test({
  name: "the real harness child boundary drops credentials and preserves toolchain variables",
  ignore: childProcessUnavailable,
  async fn() {
    const decoder = new TextDecoder();
    const denoDir = await Deno.makeTempDir({ prefix: "sentinel-harness-child-env-" });
    try {
      // Synthetic credentials only: never use or log a real credential value.
      const environment: Record<string, string> = { ...credentialEnvironment(), DENO_DIR: denoDir };
      const stage: SentinelLocalTestStage = {
        name: "child-env-probe",
        description: "synthetic credential probe",
        argv: ["eval", "--allow-env", "console.log(JSON.stringify(Deno.env.toObject()))"],
      };
      const outcome = await runSentinelLocalTestChild(stage, environment);
      assert.equal(outcome.code, 0, decoder.decode(outcome.stderr));
      const childEnvironment = JSON.parse(decoder.decode(outcome.stdout)) as Record<string, string>;
      const expected = scrubSentinelLocalTestEnvironment(environment);
      // clearEnv means the child receives exactly the scrubbed map plus the
      // Deno runtime's own injected node-compat shim keys; nothing from the
      // parent process environment can leak in.
      for (const key of Object.keys(expected)) {
        const value = childEnvironment[key];
        assert.ok(value !== undefined, `toolchain variable ${key} was dropped from the child`);
        if (key === "PATH") {
          // The Deno runtime prepends its node-compat shim directory to PATH.
          assert.ok(value.endsWith(expected[key]), `toolchain variable ${key} was not preserved`);
        } else {
          assert.equal(value, expected[key], `toolchain variable ${key} was not preserved`);
        }
      }
      for (const key of Object.keys(environment)) {
        if (isScrubbedEnvironmentKey(key)) {
          assert.equal(childEnvironment[key], undefined, `synthetic credential ${key} leaked into the child`);
        }
      }
      for (const key of ["HOME", "DENO_DIR", "TMPDIR", "LANG"]) {
        assert.equal(childEnvironment[key], environment[key], `required toolchain variable ${key} changed`);
      }
      // Nothing may be inherited from the parent process environment: every
      // child key must be a scrubbed toolchain variable or a key the Deno
      // runtime itself injects for node compatibility.
      const scrubbedKeys = new Set(Object.keys(expected));
      const denoRuntimeInjected = new Set(["DENO_NODE_SHIM_ACTIVE"]);
      for (const key of Object.keys(childEnvironment)) {
        assert.ok(
          scrubbedKeys.has(key) || denoRuntimeInjected.has(key),
          `unexpected parent-only key ${key} leaked into the child`,
        );
      }
      // When this test process has environment access, verify the same
      // property against its real environment too.
      const envPermission = await Deno.permissions.query({ name: "env" });
      if (envPermission.state === "granted") {
        for (const key of Object.keys(Deno.env.toObject())) {
          if (!scrubbedKeys.has(key) && !denoRuntimeInjected.has(key)) {
            assert.equal(
              childEnvironment[key],
              undefined,
              `parent-only environment key ${key} leaked into the child`,
            );
          }
        }
      }
    } finally {
      await Deno.remove(denoDir, { recursive: true }).catch(() => undefined);
    }
  },
});
