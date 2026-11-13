import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { createFixtureControl } from "./fixture-control.ts";

export const SYNTHETIC_TOKEN = `u_${"d".repeat(64)}`;
export const startCandidate = async (options: { executable: string; prefix: string[]; directory: string; stateDirectory: string; providerOrigin: string }) => {
  await mkdir(options.directory, { recursive: true, mode: 0o700 });
  await mkdir(options.stateDirectory, { recursive: true, mode: 0o700 });
  const control = createFixtureControl();
  const path = join(options.directory, "config.json");
  await writeFile(
    path,
    JSON.stringify({
      fixture_mode: true,
      short_deadlines: true,
      token: SYNTHETIC_TOKEN,
      upstream_key: "fixture-provider-secret",
      upstream_origin: options.providerOrigin,
      state_directory: options.stateDirectory,
      usage_limit: 50,
      ...control.config,
    }),
    { mode: 0o600 }
  );
  const argv = [...options.prefix, options.executable, "--config", path];
  const process = spawn(argv[0], argv.slice(1), {
    stdio: ["ignore", "pipe", "pipe", control.commandRead, control.replyWrite],
    env: {
      PATH: "/usr/bin:/bin",
      TMPDIR: options.directory,
      ASAN_OPTIONS: "detect_leaks=1:halt_on_error=1:abort_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    },
  });
  process.once("spawn", () => {
    control.started();
  });
  assert.ok(process.stdout && process.stderr);
  let stderr = "";
  process.stderr.on("data", (data: Buffer) => {
    stderr += data.toString();
    if (stderr.length > 2 * 1024 * 1024) process.kill("SIGTERM");
  });
  const exited = new Promise<{ code: number | null; signal: NodeJS.Signals | null }>((resolve) => {
    process.once("exit", (code, signal) => {
      resolve({ code, signal });
    });
  });
  const lines = createInterface({ input: process.stdout });
  const endpoint = await new Promise<string>((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error("Candidate readiness exceeded its bound"));
    }, 5000);
    process.once("error", (error) => {
      clearTimeout(timer);
      reject(error);
    });
    process.once("exit", () => {
      clearTimeout(timer);
      reject(new Error("Candidate exited before readiness"));
    });
    lines.on("line", (line) => {
      const value: unknown = JSON.parse(line);
      if (typeof value === "object" && value !== null && "endpoint" in value && typeof value.endpoint === "string") {
        clearTimeout(timer);
        resolve(value.endpoint);
      }
    });
  });
  const finish = async (signal: NodeJS.Signals) => {
    if (process.exitCode === null && process.signalCode === null) process.kill(signal);
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      return await Promise.race([
        exited,
        new Promise<never>((_, reject) => {
          timer = setTimeout(() => {
            process.kill("SIGKILL");
            reject(new Error("Candidate exit deadline exceeded"));
          }, 5000);
        }),
      ]);
    } finally {
      if (timer) clearTimeout(timer);
      control.close();
      lines.close();
      await writeFile(join(options.directory, "candidate.stderr"), stderr);
    }
  };
  return { endpoint, control, exited, argv, finish };
};
