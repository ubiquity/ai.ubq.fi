import { FixtureWorkspace } from "../fixture.ts";
import { loadTasks } from "../manifest.ts";

const TASKS_DIR = `${Deno.cwd()}/benchmarks/tasks`;
const FIXTURES_DIR = `${Deno.cwd()}/benchmarks/fixtures`;

Deno.test({
  name: "fixture: shell writes cannot escape the disposable workspace",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
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

Deno.test({
  name: "fixture: shell writes honor allowed scope and roll back excluded mutations",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "fail-002")!;
    const tmpParent = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const workspace = new FixtureWorkspace({
      fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
      runId: "scope",
      tmpParent,
      task,
    });
    try {
      await workspace.prepare();
      const attempted = await workspace.execShell(
        "printf 'changed\\n' > data/target.txt; printf 'TAMPERED\\n' > protected/keep.txt",
        20_000,
      );
      if (
        attempted.code === 0 || attempted.error_code !== "write_scope" ||
        !attempted.stderr.includes("write scope violation: protected/keep.txt")
      ) {
        throw new Error(`expected deterministic write-scope rejection: ${attempted.stderr}`);
      }
      if (workspace.read("data/target.txt") !== "target\n" || workspace.read("protected/keep.txt") !== "ORIGINAL\n") {
        throw new Error("unauthorized shell mutation was not rolled back");
      }

      const allowed = await workspace.execShell("printf 'changed\\n' > data/target.txt", 20_000);
      if (allowed.code !== 0 || workspace.read("data/target.txt") !== "changed\n") {
        throw new Error(`expected in-scope shell write to succeed: ${allowed.stderr}`);
      }
    } finally {
      await workspace.remove().catch(() => {});
      await Deno.remove(tmpParent, { recursive: true }).catch(() => {});
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
