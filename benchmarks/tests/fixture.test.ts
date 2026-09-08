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
  name: "fixture: shell commands cannot read host files outside the disposable workspace",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
    const tmpParent = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const outside = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const secretFile = `${outside}/host-secret.txt`;
    Deno.writeTextFileSync(secretFile, "SUPER_SECRET_HOST_TOKEN_789");

    const workspace = new FixtureWorkspace({
      fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
      runId: "read-isolation-workspace",
      tmpParent,
      task,
    });
    try {
      await workspace.prepare();
      // Attempt 1: Direct absolute read of host file outside workspace.
      const directRead = await workspace.execShell(`cat ${secretFile}`, 20_000);
      if (directRead.code === 0 && directRead.stdout.includes("SUPER_SECRET_HOST_TOKEN_789")) {
        throw new Error("sandbox allowed direct read of a host file outside the workspace");
      }

      // Attempt 2: Read through a symlink to outside.
      await new Deno.Command("sh", {
        args: ["-c", 'ln -s "$1" "$2"', "symlink-test", secretFile, `${workspace.root}/secret-link`],
      }).output();
      const symlinkRead = await workspace.execShell("cat secret-link", 20_000);
      if (symlinkRead.code === 0 && symlinkRead.stdout.includes("SUPER_SECRET_HOST_TOKEN_789")) {
        throw new Error("sandbox allowed symlinked read of a host file outside the workspace");
      }
    } finally {
      await workspace.remove().catch(() => {});
      await Deno.remove(tmpParent, { recursive: true }).catch(() => {});
      await Deno.remove(outside, { recursive: true }).catch(() => {});
    }
  },
});

Deno.test({
  name: "fixture: shell commands cannot read inherited environment variables or API keys",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
    const tmpParent = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const secretKey = "MOCK_SECRET_API_KEY_ABCD_1234";
    Deno.env.set("BENCHMARK_MOCK_API_KEY", secretKey);

    const workspace = new FixtureWorkspace({
      fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
      runId: "env-isolation-workspace",
      tmpParent,
      task,
    });
    try {
      await workspace.prepare();
      const readEnv = await workspace.execShell('echo "KEY=$BENCHMARK_MOCK_API_KEY"', 20_000);
      if (readEnv.stdout.includes(secretKey)) {
        throw new Error("sandbox exposed inherited API key environment variable to shell");
      }
      if (readEnv.stdout.trim() !== "KEY=") {
        throw new Error(`expected empty env var, got: ${readEnv.stdout}`);
      }
    } finally {
      Deno.env.delete("BENCHMARK_MOCK_API_KEY");
      await workspace.remove().catch(() => {});
      await Deno.remove(tmpParent, { recursive: true }).catch(() => {});
    }
  },
});

Deno.test({
  name: "fixture: shell.exec enforces task allowed_write_scope and restores unauthorized writes",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    const baseTask = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
    // Configure a task with explicit allowed_write_scope including an exclusion rule.
    const task = {
      ...baseTask,
      allowed_write_scope: ["src/**", "!src/protected.ts"],
    };
    const tmpParent = Deno.makeTempDirSync({ dir: `${Deno.cwd()}/benchmark-runs` });
    const workspace = new FixtureWorkspace({
      fixtureDir: `${FIXTURES_DIR}/${task.fixture}`,
      runId: "scope-enforcement-workspace",
      tmpParent,
      task,
    });
    try {
      await workspace.prepare();
      await Deno.mkdir(`${workspace.root}/src`, { recursive: true });

      // Case 1: Allowed write inside allowed_write_scope.
      const allowedResult = await workspace.execShell("echo 'allowed content' > src/allowed.txt", 20_000);
      if (allowedResult.code !== 0 || !await exists(`${workspace.root}/src/allowed.txt`)) {
        throw new Error(`expected allowed write to succeed: ${allowedResult.stderr}`);
      }

      // Prepare a protected file inside the excluded path.
      await Deno.mkdir(`${workspace.root}/src`, { recursive: true });
      await Deno.writeTextFile(`${workspace.root}/src/protected.ts`, "ORIGINAL_PROTECTED_CONTENT");

      // Case 2: Attempted write to excluded path (!src/protected.ts).
      let caughtExcludedError = false;
      try {
        await workspace.execShell("echo 'MUTATED' > src/protected.ts", 20_000);
      } catch (err) {
        if (err instanceof Error && err.name === "WriteScopeViolationError") {
          caughtExcludedError = true;
        }
      }
      if (!caughtExcludedError) throw new Error("expected WriteScopeViolationError on excluded path write");
      // Verify the excluded file was restored to its original content.
      const protectedContentAfter = await Deno.readTextFile(`${workspace.root}/src/protected.ts`);
      if (protectedContentAfter !== "ORIGINAL_PROTECTED_CONTENT") {
        throw new Error(`expected protected file to be restored, got: ${protectedContentAfter}`);
      }

      // Case 3: Attempted creation of a file outside allowed_write_scope (e.g. root level unauthorized.txt).
      let caughtOutsideError = false;
      try {
        await workspace.execShell("echo 'MUTATED' > unauthorized.txt", 20_000);
      } catch (err) {
        if (err instanceof Error && err.name === "WriteScopeViolationError") {
          caughtOutsideError = true;
        }
      }
      if (!caughtOutsideError) throw new Error("expected WriteScopeViolationError on outside path write");
      // Verify the unauthorized file was deleted and not persisted.
      if (await exists(`${workspace.root}/unauthorized.txt`)) {
        throw new Error("unauthorized file was persisted despite write scope violation");
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
