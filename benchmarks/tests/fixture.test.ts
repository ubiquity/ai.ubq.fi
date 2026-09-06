import { referenceAdapter } from "../adapter.ts";
import { FixtureWorkspace, WriteScopeViolationError } from "../fixture.ts";
import { loadTasks } from "../manifest.ts";
import type { TaskManifest, TrajectoryEvent } from "../schemas.ts";

const TASKS_DIR = `${Deno.cwd()}/benchmarks/tasks`;
const FIXTURES_DIR = `${Deno.cwd()}/benchmarks/fixtures`;
const RUNS_ROOT = `${Deno.cwd()}/benchmark-runs`;

Deno.test({
  name: "fixture: shell writes cannot escape the disposable workspace",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
    await Deno.mkdir(RUNS_ROOT, { recursive: true });
    const tmpParent = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const outside = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const workspace = new FixtureWorkspace({
      fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
      runId: "workspace",
      tmpParent,
      task,
    });
    try {
      await workspace.prepare();
      const link = await new Deno.Command("sh", {
        args: ["-c", 'ln -s "$1" "$2"', "fixture-test", outside, `${workspace.root}/escape-link`],
      }).output();
      if (link.code !== 0) throw new Error("failed to create the escape-test symlink");
      const inside = await workspace.execShell("touch inside-workspace", 20_000);
      if (inside.code !== 0 || !await exists(`${workspace.root}/inside-workspace`)) {
        throw new Error(`sandbox rejected an in-workspace write: ${inside.stderr}`);
      }

      const absolute = await workspace.execShell(`touch ${outside}/absolute-escape`, 20_000);
      const symlink = await workspace.execShell("touch escape-link/symlink-escape", 20_000);

      if (absolute.code === 0 || symlink.code === 0) throw new Error("expected the attempted host writes to fail");
      if (await exists(`${outside}/absolute-escape`) || await exists(`${outside}/symlink-escape`)) {
        throw new Error("sandbox allowed a write outside the disposable workspace");
      }
    } finally {
      await workspace.remove().catch(() => {});
      await Deno.remove(tmpParent, { recursive: true }).catch(() => {});
      await Deno.remove(outside, { recursive: true }).catch(() => {});
    }
  },
});

async function exists(path: string): Promise<boolean> {
  try {
    await Deno.stat(path);
    return true;
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return false;
    throw error;
  }
}

const scopeTask = (allowed_write_scope: string[], scripted_trail?: TaskManifest["scripted_trail"]): TaskManifest => ({
  id: "fixture-scope-test",
  category: "coding",
  title: "fixture scope test",
  description: "fixture scope test",
  fixture: "fixture-scope-test",
  fixture_revision: "sha256:fixture-scope-test",
  timeout_ms: 20_000,
  max_tool_calls: 4,
  min_tool_calls: 0,
  min_model_calls: 0,
  allowed_write_scope,
  scripted_trail,
});

const initialScopeFiles: Record<string, string> = {
  "allowed/existing.txt": "allowed-before\n",
  "excluded/modified.txt": "excluded-before\n",
  "excluded/deleted.txt": "deleted-before\n",
  "excluded/replaced-file.txt": "file-before\n",
  "excluded/replaced-directory/child.txt": "child-before\n",
  "protected/keep.txt": "protected-before\n",
};

interface TestWorkspace {
  workspace: FixtureWorkspace;
  fixtureDir: string;
  tmpParent: string;
}

async function createTestWorkspace(
  allowed_write_scope: string[],
  files: Record<string, string> = initialScopeFiles,
): Promise<TestWorkspace> {
  await Deno.mkdir(RUNS_ROOT, { recursive: true });
  const fixtureDir = await Deno.makeTempDir({ dir: RUNS_ROOT, prefix: "fixture-" });
  for (const [rel, content] of Object.entries(files)) {
    const path = `${fixtureDir}/${rel}`;
    const slash = path.lastIndexOf("/");
    await Deno.mkdir(path.slice(0, slash), { recursive: true });
    await Deno.writeTextFile(path, content);
  }
  const tmpParent = await Deno.makeTempDir({ dir: RUNS_ROOT, prefix: "workspace-" });
  const workspace = new FixtureWorkspace({
    fixtureDir,
    runId: "run",
    tmpParent,
    task: scopeTask(allowed_write_scope),
  });
  await workspace.prepare();
  return { workspace, fixtureDir, tmpParent };
}

async function removeTestWorkspace(testWorkspace: TestWorkspace): Promise<void> {
  await testWorkspace.workspace.remove().catch(() => {});
  await Deno.remove(testWorkspace.tmpParent, { recursive: true }).catch(() => {});
  await Deno.remove(testWorkspace.fixtureDir, { recursive: true }).catch(() => {});
}

function requireEntry(path: string): Deno.FileInfo {
  return Deno.lstatSync(path);
}

Deno.test({
  name: "fixture: shell scope restores unauthorized mutations and keeps authorized writes",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const testWorkspace = await createTestWorkspace(["**", "!excluded/**", "!protected/**"]);
    const { workspace } = testWorkspace;
    try {
      let caught: unknown;
      try {
        await workspace.execShell(
          [
            "printf 'allowed-after\\n' > allowed/created.txt",
            "printf 'changed\\n' > excluded/modified.txt",
            "rm excluded/deleted.txt",
            "rm excluded/replaced-file.txt",
            "mkdir excluded/replaced-file.txt",
            "rm -rf excluded/replaced-directory",
            "printf 'replacement\\n' > excluded/replaced-directory",
            "printf 'changed\\n' > protected/keep.txt",
            "printf 'created\\n' > protected/created.txt",
            "exit 7",
          ].join("\n"),
          20_000,
        );
      } catch (err) {
        caught = err;
      }

      if (!(caught instanceof WriteScopeViolationError)) {
        throw new Error(`expected a write-scope error, got ${String(caught)}`);
      }
      const expectedPaths = [
        "excluded/deleted.txt",
        "excluded/modified.txt",
        "excluded/replaced-directory",
        "excluded/replaced-directory/child.txt",
        "excluded/replaced-file.txt",
        "protected/created.txt",
        "protected/keep.txt",
      ];
      if (JSON.stringify(caught.paths) !== JSON.stringify(expectedPaths)) {
        throw new Error(`unexpected sorted violation paths: ${JSON.stringify(caught.paths)}`);
      }
      const expectedError =
        "write scope violation: excluded/deleted.txt, excluded/modified.txt, excluded/replaced-directory, " +
        "excluded/replaced-directory/child.txt, excluded/replaced-file.txt, protected/created.txt, " +
        "protected/keep.txt are not writable (scope: **, !excluded/**, !protected/**)";
      if (caught.message !== expectedError) throw new Error(`unexpected violation text: ${caught.message}`);

      if (workspace.read("allowed/created.txt") !== "allowed-after\n") {
        throw new Error("authorized shell creation was not retained");
      }
      if (workspace.read("excluded/modified.txt") !== "excluded-before\n") {
        throw new Error("unauthorized modification was not restored");
      }
      if (workspace.read("excluded/deleted.txt") !== "deleted-before\n") {
        throw new Error("unauthorized deletion was not restored");
      }
      if (workspace.read("excluded/replaced-file.txt") !== "file-before\n") {
        throw new Error("file-to-directory replacement was not restored");
      }
      if (!requireEntry(`${workspace.root}/excluded/replaced-directory`).isDirectory) {
        throw new Error("directory-to-file replacement did not restore the directory");
      }
      if (workspace.read("excluded/replaced-directory/child.txt") !== "child-before\n") {
        throw new Error("nested content of the replaced directory was not restored");
      }
      if (workspace.read("protected/keep.txt") !== "protected-before\n") {
        throw new Error("protected modification was not restored");
      }
      if (await exists(`${workspace.root}/protected/created.txt`)) {
        throw new Error("unauthorized creation was not removed");
      }
    } finally {
      await removeTestWorkspace(testWorkspace);
    }
  },
});

async function runReferenceShell(
  command: string,
): Promise<{ ok: boolean; error?: string; error_code?: string }> {
  const testWorkspace = await createTestWorkspace(["**", "!excluded/**"], {
    "excluded/modified.txt": "before\n",
  });
  const events: TrajectoryEvent[] = [];
  try {
    const task = scopeTask(["**", "!excluded/**"], [{
      tool: "shell.exec",
      args: { command },
    }]);
    await referenceAdapter.run({
      runId: "reference-run",
      task,
      workspace: testWorkspace.workspace,
      record: (event) => events.push(event),
      checkToolLimit: () => {},
      signal: new AbortController().signal,
      time: () => "2026-01-01T00:00:00.000Z",
    });
    const result = events.find((event) => event.type === "tool_result");
    if (result === undefined || result.type !== "tool_result") {
      throw new Error("reference adapter emitted no tool result");
    }
    return { ok: result.ok, error: result.error, error_code: result.error_code };
  } finally {
    await removeTestWorkspace(testWorkspace);
  }
}

Deno.test({
  name: "fixture: equivalent shell violations have stable adapter trajectory errors",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const first = await runReferenceShell(
      "printf 'changed\\n' > excluded/modified.txt; printf 'created\\n' > excluded/created.txt",
    );
    const second = await runReferenceShell(
      "printf 'created\\n' > excluded/created.txt; printf 'changed\\n' > excluded/modified.txt",
    );
    if (first.ok || second.ok) throw new Error("equivalent violations produced different ok results");
    if (first.error_code !== "write_scope" || second.error_code !== "write_scope") {
      throw new Error(`expected write_scope tool results: ${first.error_code}, ${second.error_code}`);
    }
    if (first.error !== second.error) {
      throw new Error(`equivalent violations produced different errors: ${first.error} / ${second.error}`);
    }
    const expected = "write scope violation: excluded/created.txt, excluded/modified.txt are not writable " +
      "(scope: **, !excluded/**)";
    if (first.error !== expected) throw new Error(`unexpected stable violation text: ${first.error}`);
  },
});
