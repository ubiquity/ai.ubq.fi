import assert from "node:assert/strict";

import { FixtureWorkspace, WriteScopeViolationError } from "../fixture.ts";
import { loadTasks } from "../manifest.ts";
import { referenceAdapter } from "../adapter.ts";

const TASKS_DIR = `${Deno.cwd()}/benchmarks/tasks`;
const FIXTURES_DIR = `${Deno.cwd()}/benchmarks/fixtures`;
const RUNS_ROOT = `${Deno.cwd()}/benchmark-runs`;

Deno.test({
  name: "fixture: shell writes cannot escape the disposable workspace",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
    await Deno.mkdir(RUNS_ROOT, { recursive: true });
    const tmpParent = Deno.makeTempDirSync({ dir: RUNS_ROOT });
    const outside = Deno.makeTempDirSync({ dir: RUNS_ROOT });
    const workspace = new FixtureWorkspace({
      fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
      runId: "workspace",
      tmpParent,
      task,
    });
    try {
      await workspace.prepare();
      const probe = await workspace.execShell("true", 20_000);
      if (probe.code !== 0 && /NETLINK_ROUTE/.test(probe.stderr)) return;
      if (probe.code !== 0) throw new Error(`shell sandbox probe failed: ${probe.stderr}`);
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

async function withWorkspace<T>(
  taskId: string,
  fn: (workspace: FixtureWorkspace, task: ReturnType<typeof loadTasks>[number]) => Promise<T>,
): Promise<T> {
  await Deno.mkdir(RUNS_ROOT, { recursive: true });
  const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === taskId);
  if (task === undefined) throw new Error(`missing fixture task ${taskId}`);
  const tmpParent = Deno.makeTempDirSync({ dir: RUNS_ROOT });
  const workspace = new FixtureWorkspace({
    fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
    runId: "workspace",
    tmpParent,
    task,
  });
  try {
    await workspace.prepare();
    installLocalShellRunner(workspace);
    return await fn(workspace, task);
  } finally {
    await workspace.remove().catch(() => {});
    await Deno.remove(tmpParent, { recursive: true }).catch(() => {});
  }
}

function installLocalShellRunner(workspace: FixtureWorkspace): void {
  const root = workspace.root;
  (workspace as unknown as {
    execShellSandbox: (command: string, timeoutMs: number, signal?: AbortSignal) => Promise<unknown>;
  }).execShellSandbox = async (command) => {
    const out = await new Deno.Command("sh", {
      args: ["-c", command],
      cwd: root,
      stdout: "piped",
      stderr: "piped",
    }).output();
    return {
      code: out.code,
      stdout: new TextDecoder().decode(out.stdout),
      stderr: new TextDecoder().decode(out.stderr),
      timedOut: false,
    };
  };
}

async function expectWriteScopeViolation(
  workspace: FixtureWorkspace,
  command: string,
): Promise<WriteScopeViolationError> {
  try {
    await workspace.execShell(command, 20_000);
  } catch (error) {
    assert.ok(error instanceof WriteScopeViolationError, `expected write_scope violation, got ${String(error)}`);
    return error;
  }
  assert.fail("expected shell command to violate the write scope");
}

Deno.test({
  name: "fixture: shell restores excluded mutations while retaining authorized writes",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    await withWorkspace("fail-002", async (workspace) => {
      const protectedDirectoryMtime = (await Deno.lstat(`${workspace.root}/protected`)).mtime?.getTime();
      const replaced = await expectWriteScopeViolation(
        workspace,
        "printf authorized > data/authorized.txt; rm protected/keep.txt; " +
          "mkdir protected/keep.txt; printf unauthorized > protected/keep.txt/child",
      );
      assert.deepEqual(replaced.paths, ["protected/keep.txt", "protected/keep.txt/child"]);
      assert.equal(
        replaced.message,
        "write scope violation: paths protected/keep.txt, protected/keep.txt/child are not writable (scope: **, !protected/**)",
      );
      assert.equal(workspace.read("protected/keep.txt"), "ORIGINAL\n");
      assert.equal(await exists(`${workspace.root}/protected/keep.txt/child`), false);
      assert.equal(workspace.read("data/authorized.txt"), "authorized");
      assert.equal((await Deno.lstat(`${workspace.root}/protected`)).mtime?.getTime(), protectedDirectoryMtime);

      const created = await expectWriteScopeViolation(workspace, "printf unauthorized > protected/new.txt");
      assert.deepEqual(created.paths, ["protected/new.txt"]);
      assert.equal(await exists(`${workspace.root}/protected/new.txt`), false);

      await expectWriteScopeViolation(workspace, "rm protected/keep.txt");
      assert.equal(workspace.read("protected/keep.txt"), "ORIGINAL\n");
    });
  },
});

Deno.test({
  name: "fixture: shell restores excluded metadata, links, and directory replacements",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    await withWorkspace("fail-002", async (workspace) => {
      const originalMode = (await Deno.lstat(`${workspace.root}/protected/keep.txt`)).mode;
      await expectWriteScopeViolation(workspace, "chmod 600 protected/keep.txt");
      assert.equal((await Deno.lstat(`${workspace.root}/protected/keep.txt`)).mode, originalMode);

      await expectWriteScopeViolation(workspace, "rm protected/keep.txt; ln -s data/target.txt protected/keep.txt");
      const restoredFile = await Deno.lstat(`${workspace.root}/protected/keep.txt`);
      assert.equal(restoredFile.isFile, true);
      assert.equal(restoredFile.isSymlink, false);
      assert.equal(workspace.read("protected/keep.txt"), "ORIGINAL\n");

      await expectWriteScopeViolation(workspace, "rm -rf protected; printf replacement > protected");
      const restoredDirectory = await Deno.lstat(`${workspace.root}/protected`);
      assert.equal(restoredDirectory.isDirectory, true);
      assert.equal(workspace.read("protected/keep.txt"), "ORIGINAL\n");
    });
  },
});

Deno.test({
  name: "fixture: adapter records sorted repeatable write_scope trajectory evidence",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const run = async (command: string): Promise<{ error_code?: string; error?: string }> =>
      await withWorkspace("fail-002", async (workspace, task) => {
        const events: unknown[] = [];
        await referenceAdapter.run({
          runId: "fixture-trajectory",
          task: {
            ...task,
            scripted_trail: [{
              tool: "shell.exec",
              args: { command },
              expect: { ok: false, error_contains: "write scope violation" },
            }],
          },
          workspace,
          record: (event) => events.push(event),
          checkToolLimit: () => {},
          signal: new AbortController().signal,
          time: () => "2026-01-01T00:00:00.000Z",
        });
        const result = events.find((event) => (event as { type?: string }).type === "tool_result") as
          | { error_code?: string; error?: string }
          | undefined;
        assert.ok(result !== undefined);
        return result;
      });

    const first = await run("printf z > protected/z; printf a > protected/a");
    const second = await run("printf a > protected/a; printf z > protected/z");
    assert.equal(first.error_code, "write_scope");
    assert.equal(second.error_code, "write_scope");
    assert.equal(first.error, second.error);
    assert.equal(
      first.error,
      "write scope violation: paths protected/a, protected/z are not writable (scope: **, !protected/**)",
    );
  },
});
async function exists(path: string): Promise<boolean> {
  try {
    await Deno.stat(path);
    return true;
  } catch (error) {
    if (error instanceof Deno.errors.NotFound || error instanceof Deno.errors.NotADirectory) return false;
    throw error;
  }
}
