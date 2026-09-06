import { FixtureWorkspace, SAFE_SHELL_ENVIRONMENT_KEYS } from "../fixture.ts";
import { loadTasks } from "../manifest.ts";

const TASKS_DIR = Deno.cwd() + "/benchmarks/tasks";
const FIXTURES_DIR = Deno.cwd() + "/benchmarks/fixtures";
const RUNS_ROOT = Deno.cwd() + "/benchmark-runs";

Deno.test({
  name: "fixture: shell confinement and shell/Git compatibility",
  ignore: Deno.build.os !== "darwin" && Deno.build.os !== "linux",
  async fn() {
    await Deno.mkdir(RUNS_ROOT, { recursive: true });
    const task = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "nav-001")!;
    const gitTask = loadTasks(TASKS_DIR).find((candidate) => candidate.id === "seq-004")!;
    const tmpParent = Deno.makeTempDirSync({ dir: RUNS_ROOT });
    const outside = Deno.makeTempDirSync({ dir: RUNS_ROOT });
    const hostMarker = "fixture-host-read-denied";
    const outsideFile = outside + "/host-only.txt";
    const repositoryFile = Deno.cwd() + "/benchmarks/fixture.ts";
    Deno.writeTextFileSync(outsideFile, hostMarker);
    const workspace = new FixtureWorkspace({
      fixtureDir: FIXTURES_DIR + "/" + task.fixture,
      runId: "workspace",
      tmpParent,
      task,
    });
    const gitWorkspace = new FixtureWorkspace({
      fixtureDir: FIXTURES_DIR + "/" + gitTask.fixture,
      runId: "git-workspace",
      tmpParent,
      task: gitTask,
    });
    try {
      await workspace.prepare();
      const link = await new Deno.Command("sh", {
        args: [
          "-c",
          'ln -s "$1" "$2" && ln -s "$3" "$4" && ln -s "$5" "$6"',
          "fixture-test",
          outsideFile,
          workspace.root + "/host-link.txt",
          repositoryFile,
          workspace.root + "/repository-link.ts",
          outside,
          workspace.root + "/escape-link",
        ],
      }).output();
      if (link.code !== 0) throw new Error("failed to create the escape-test symlinks");
      const env = await workspace.execShell("env | sort", 20_000);
      if (env.timedOut || env.code !== 0) throw new Error("sandbox environment probe failed");
      const visible = parseEnvironment(env.stdout);
      const expected = new Map<string, string>([
        ["GIT_CONFIG_GLOBAL", "/dev/null"],
        ["GIT_CONFIG_NOSYSTEM", "1"],
        ["GIT_CONFIG_SYSTEM", "/dev/null"],
        ["HOME", workspace.root + "/.home"],
        ["PATH", Deno.build.os === "linux" ? "/sandbox-bin:/usr/bin:/bin" : "/usr/bin:/bin"],
        ["PWD", workspace.root],
        ["TMPDIR", workspace.root + "/.tmp"],
      ]);
      const shellGenerated = new Set(["SHLVL", "_"]);
      const safeKeys = new Set([...SAFE_SHELL_ENVIRONMENT_KEYS, ...shellGenerated]);
      const unexpected = [...visible.keys()].filter((key) => !safeKeys.has(key));
      if (unexpected.length > 0) {
        throw new Error("sandbox exposed unexpected environment keys: " + unexpected.join(", "));
      }
      for (const [key, value] of expected) {
        if (visible.get(key) !== value) throw new Error("sandbox environment value drifted for " + key);
      }
      if (visible.has("SHLVL") && !/^[0-9]+$/.test(visible.get("SHLVL")!)) {
        throw new Error("sandbox exposed an invalid shell level");
      }
      if (visible.has("_") && !/(^|\/)env$/.test(visible.get("_")!)) {
        throw new Error("sandbox exposed an unexpected shell command path");
      }
      for (const key of ["API_KEY", "OPENAI_API_KEY", "UNRELATED_HOST_VARIABLE"]) {
        if (visible.has(key)) throw new Error("sandbox exposed forbidden environment key: " + key);
      }
      const outsideName = outside.slice(outside.lastIndexOf("/") + 1);
      const readAttempts: [string, string, string][] = [
        ["host absolute path", "cat " + shellQuote(outsideFile), hostMarker],
        ["repository absolute path", "cat " + shellQuote(repositoryFile), ""],
        ["host traversal path", "cat " + shellQuote("../../" + outsideName + "/host-only.txt"), hostMarker],
        ["repository traversal path", "cat ../../../benchmarks/fixture.ts", ""],
        ["host symlink", "cat host-link.txt", hostMarker],
        ["repository symlink", "cat repository-link.ts", ""],
      ];
      for (const [label, command, marker] of readAttempts) {
        await assertReadDenied(workspace, label, command, marker);
      }
      const inside = await workspace.execShell("touch inside-workspace && test -f inside-workspace", 20_000);
      if (inside.timedOut || inside.code !== 0 || !await exists(workspace.root + "/inside-workspace")) {
        throw new Error("sandbox rejected an in-workspace write");
      }
      const utility = await workspace.execShell(
        'printf "b\n" > utility.txt && printf "a\n" >> utility.txt && test "$(cat utility.txt | sort | tail -n 1)" = b && find docs -type f -print | sort | tail -n 1 && wc -c docs/spec.txt',
        20_000,
      );
      if (utility.timedOut || utility.code !== 0 || !utility.stdout.includes("docs/spec.txt")) {
        throw new Error("sandbox rejected a representative shell utility command");
      }
      const absolute = await workspace.execShell("touch " + shellQuote(outside + "/absolute-escape"), 20_000);
      const symlink = await workspace.execShell("touch escape-link/symlink-escape", 20_000);
      if (absolute.timedOut || symlink.timedOut || absolute.code === 0 || symlink.code === 0) {
        throw new Error("expected the attempted host writes to fail");
      }
      if (await exists(outside + "/absolute-escape") || await exists(outside + "/symlink-escape")) {
        throw new Error("sandbox allowed a write outside the disposable workspace");
      }
      await gitWorkspace.prepare();
      gitWorkspace.write("shell-git.txt", "sandbox Git smoke test\n");
      const git = await gitWorkspace.execShell(
        'git status --porcelain && git add shell-git.txt && git -c user.email=benchmark@invalid.invalid -c user.name=benchmark commit -qm "fixture shell git" && test -z "$(git status --porcelain)" && test "$(git log -1 --format=%s)" = "fixture shell git"',
        20_000,
      );
      if (git.timedOut || git.code !== 0 || !git.stdout.includes("?? shell-git.txt")) {
        throw new Error("sandbox rejected Git status/commit behavior");
      }
    } finally {
      await gitWorkspace.remove().catch(() => {});
      await workspace.remove().catch(() => {});
      await Deno.remove(tmpParent, { recursive: true }).catch(() => {});
      await Deno.remove(outside, { recursive: true }).catch(() => {});
    }
  },
});

async function assertReadDenied(
  workspace: FixtureWorkspace,
  label: string,
  command: string,
  marker: string,
): Promise<void> {
  const result = await workspace.execShell(command, 20_000);
  if (result.timedOut || result.code === 0 || (marker !== "" && result.stdout.includes(marker))) {
    throw new Error("sandbox allowed " + label);
  }
}

function parseEnvironment(output: string): Map<string, string> {
  const visible = new Map<string, string>();
  for (const line of output.trimEnd().split("\n")) {
    if (line === "") continue;
    const separator = line.indexOf("=");
    if (separator <= 0) throw new Error("sandbox environment output was malformed");
    visible.set(line.slice(0, separator), line.slice(separator + 1));
  }
  return visible;
}

function shellQuote(value: string): string {
  return "'" + value + "'";
}

async function exists(path: string): Promise<boolean> {
  try {
    await Deno.stat(path);
    return true;
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return false;
    throw error;
  }
}
