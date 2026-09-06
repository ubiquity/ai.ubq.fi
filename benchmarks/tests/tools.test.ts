import { referenceAdapter } from "../adapter.ts";
import { loadTasks } from "../manifest.ts";
import { runOne, RunOptions } from "../runner.ts";
import { TaskManifest, ToolResultEvent } from "../schemas.ts";

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

function fail002(): TaskManifest {
  return loadTasks(TASKS_DIR).find((t) => t.id === "fail-002")!;
}

Deno.test("tools: canonical browser fakes resolve inside the reference adapter", async () => {
  const opts = freshOptions();
  try {
    const task: TaskManifest = {
      ...nav001(),
      scripted_trail: [
        {
          tool: "browser.search",
          args: { query: "example" },
          expect: { ok: true, output_contains: ["https://example.com/index.html", "Welcome to the example site."] },
        },
        {
          tool: "browser.open",
          args: { url: "https://example.com/index.html" },
          expect: { ok: true, output_contains: ["Example", "Welcome to the example site."] },
        },
        {
          tool: "browser.find",
          args: { query: "find content" },
          expect: { ok: true, output_contains: ["Find content"] },
        },
        { tool: "editor.apply_patch", args: { path: "answer.txt", add: true, new: "done" } },
      ],
      verify: { command: "test -f answer.txt", timeout_ms: 20000 },
      oracle: { file_checks: [{ path: "answer.txt", kind: "equals", value: "done" }] },
    };
    const { result } = await runOne(task, referenceAdapter, opts);
    if (!result.success || result.failure_class !== null) {
      throw new Error(`expected success, got ${result.failure_class}: ${result.failure_detail}`);
    }
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("tools: canonical boundaries and error codes flow through the adapter", async () => {
  const opts = freshOptions();
  try {
    const task: TaskManifest = {
      ...nav001(),
      scripted_trail: [
        {
          tool: "filesystem.read",
          args: { path: "../etc/passwd" },
          expect: { ok: false, error_contains: "path escapes workspace root" },
        },
        {
          tool: "filesystem.read",
          args: { path: "docs/spec.txt" },
          expect: { ok: true, output_contains: ["Section 1"] },
        },
        {
          tool: "filesystem.read",
          args: { path: "docs/missing.txt" },
          expect: { ok: false, error_contains: "not found" },
        },
        { tool: "editor.apply_patch", args: { path: "answer.txt", add: true, new: "ok" } },
      ],
      verify: { command: "test -f answer.txt", timeout_ms: 20000 },
      oracle: { file_checks: [{ path: "answer.txt", kind: "equals", value: "ok" }] },
    };
    const { result, events } = await runOne(task, referenceAdapter, opts);
    if (!result.success || result.failure_class !== null) {
      throw new Error(`expected success, got ${result.failure_class}: ${result.failure_detail}`);
    }
    const toolResults = events.filter((e): e is ToolResultEvent => e.type === "tool_result");
    if (toolResults.length !== 4) throw new Error(`expected 4 tool results, got ${toolResults.length}`);
    if (toolResults[0].error_code !== "path_escape") {
      throw new Error(`expected path_escape, got ${toolResults[0].error_code}`);
    }
    if (toolResults[1].ok !== true) throw new Error("expected the in-scope read to succeed");
    if (toolResults[2].error_code !== "not_found") {
      throw new Error(`expected not_found, got ${toolResults[2].error_code}`);
    }
    if (toolResults[3].error_code !== undefined || toolResults[3].ok !== true) {
      throw new Error("expected the final patch to be a clean success");
    }
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});

Deno.test("tools: shell write-scope violations are deterministic and rolled back", async () => {
  const opts = freshOptions();
  try {
    const task: TaskManifest = {
      ...fail002(),
      scripted_trail: [
        {
          tool: "shell.exec",
          args: {
            command: "printf 'changed\\n' > data/target.txt; printf 'TAMPERED\\n' > protected/keep.txt",
          },
          expect: { ok: false, error_contains: "write scope violation: protected/keep.txt" },
        },
        {
          tool: "editor.apply_patch",
          args: { path: "data/target.txt", old: "target", new: "changed" },
        },
        { tool: "shell.exec", args: { command: "sh tests/run.sh" }, expect: { ok: true } },
        {
          tool: "filesystem.read",
          args: { path: "protected/keep.txt" },
          expect: { ok: true, output_contains: ["ORIGINAL"] },
        },
      ],
    };
    const { result, events } = await runOne(task, referenceAdapter, opts);
    if (!result.success || result.failure_class !== null) {
      throw new Error(`expected success, got ${result.failure_class}: ${result.failure_detail}`);
    }
    const toolResults = events.filter((e): e is ToolResultEvent => e.type === "tool_result");
    if (toolResults[0].error_code !== "write_scope") {
      throw new Error(`expected write_scope, got ${toolResults[0].error_code}`);
    }
    if (toolResults[0].ok || toolResults[1].ok !== true || toolResults[2].ok !== true) {
      throw new Error("expected the rejected shell write followed by successful recovery");
    }
  } finally {
    Deno.removeSync(opts.runsRoot, { recursive: true });
  }
});
