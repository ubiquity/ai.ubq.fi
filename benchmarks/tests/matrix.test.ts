import { runBenchmarks } from "../runner.ts";

Deno.test("matrix: every manifest completes successfully with the hermetic reference adapter", async () => {
  Deno.mkdirSync(`${Deno.cwd()}/benchmark-runs`, { recursive: true });
  const runsRoot = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
  try {
    const results = await runBenchmarks({
      configs: ["reference"],
      taskSelectors: ["*"],
      runsRoot,
      tasksDir: `${Deno.cwd()}/benchmarks/tasks`,
      fixturesDir: `${Deno.cwd()}/benchmarks/fixtures`,
    });
    const failures = results.filter((r) => !r.success);
    if (failures.length > 0) {
      throw new Error(
        `${failures.length} task(s) failed: ` +
          failures.map((f) => `${f.task_id}: ${f.failure_class}: ${f.failure_detail}`).join("; "),
      );
    }
    if (results.length !== 25) throw new Error(`expected 25 results, got ${results.length}`);

    // Injected-failure tasks expose the expected deterministic metric profile.
    const byId = (id: string) => results.find((r) => r.task_id === id)!;
    const fail001 = byId("fail-001");
    if (fail001.metrics.tool_errors !== 1 || fail001.metrics.recovery_attempts !== 1) {
      throw new Error(`fail-001 metrics wrong: ${JSON.stringify(fail001.metrics)}`);
    }
    if (byId("fail-003").metrics.invalid_tool_calls !== 1) throw new Error("fail-003 invalid call not counted");
    if (byId("fail-004").metrics.wrong_tool_calls !== 1) throw new Error("fail-004 wrong tool not counted");
    const long003 = byId("long-003");
    if (long003.metrics.tool_errors !== 2 || long003.metrics.recovery_attempts !== 2) {
      throw new Error(`long-003 metrics wrong: ${JSON.stringify(long003.metrics)}`);
    }
    for (const id of ["long-002", "long-003", "long-004", "long-005"]) {
      if (byId(id).metrics.tool_calls < 21) throw new Error(`${id} did not exceed 20 tool calls`);
    }
    if (byId("long-001").metrics.tool_calls < 11) throw new Error("long-001 did not exceed 10 tool calls");
    if (byId("nav-001").verification.passed !== true) throw new Error("nav-001 verification evidence missing");

    // Trajectories for every run are persisted and parseable.
    for (const result of results) {
      const text = Deno.readTextFileSync(`${runsRoot}/${result.trajectory}`);
      const first = text.trimEnd().split("\n")[0];
      const parsed = JSON.parse(first);
      if (parsed.type !== "run" || parsed.run_id !== result.run_id) {
        throw new Error(`trajectory mismatch for ${result.task_id}`);
      }
    }
  } finally {
    Deno.removeSync(runsRoot, { recursive: true });
  }
});
