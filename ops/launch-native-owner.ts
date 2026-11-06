import { lstat, readFile, realpath, readdir } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { loadEnvFile } from "node:process";
import { nativeOwnerStdin, parseNativeOwnerCarrier, parseNativeOwnerConfig } from "./native-owner-config.ts";
import { nativeOwnerDigest, nativeOwnerRuntime, NATIVE_OWNER_BUN_SHA256, readNativeOwnerFile, verifyNativeOwnerPayload } from "./native-owner-release.ts";

const loadedSetting = (name: string): string | undefined => process.env[name];

const privateConfig = async (root: string): Promise<ReturnType<typeof parseNativeOwnerConfig>> => {
  const path = join(root, ".data/native-owner/config.json");
  const before = await lstat(path);
  if (
    !before.isFile() ||
    before.isSymbolicLink() ||
    before.nlink !== 1 ||
    before.uid !== process.getuid?.() ||
    (before.mode & 0o077) !== 0 ||
    before.size > 65_536
  )
    throw new Error("Native owner private config is unsafe");
  const bytes = await readFile(path, "utf8");
  const after = await lstat(path);
  if (before.dev !== after.dev || before.ino !== after.ino || before.mtimeMs !== after.mtimeMs || before.size !== after.size)
    throw new Error("Native owner private config changed");
  return parseNativeOwnerConfig(JSON.parse(bytes));
};

export const verifyNativeOwnerLimits = (limits: Readonly<Record<string, string>>): void => {
  const [quota, period] = (limits["cpu.max"] ?? "").split(" ").map(Number);
  if (
    limits["memory.max"] !== "268435456" ||
    limits["memory.high"] !== "134217728" ||
    limits["memory.swap.max"] !== "67108864" ||
    limits["pids.max"] !== "96" ||
    quota / period !== 0.5
  )
    throw new Error("Native owner cgroup resource limits are unproved");
};
const verifyCgroup = async (): Promise<void> => {
  const membership = (await readFile("/proc/self/cgroup", "utf8")).trim();
  if (!membership.startsWith("0::/") || membership.includes("\n")) throw new Error("Native owner cgroup membership is invalid");
  const group = "/sys/fs/cgroup" + membership.slice(3);
  const limits = Object.fromEntries(
    await Promise.all(
      ["memory.max", "memory.high", "memory.swap.max", "pids.max", "cpu.max"].map(async (name) => [name, (await readFile(join(group, name), "utf8")).trim()])
    )
  );
  verifyNativeOwnerLimits(limits);
};
export const superviseNativeOwner = async (binary: string, root: string, input: string): Promise<number> => {
  const child = Bun.spawn([binary], { cwd: root, env: { PATH: "/usr/bin:/bin", TZ: "UTC" }, stdin: "pipe", stdout: "inherit", stderr: "inherit" });
  const terminate = (): void => {
    child.kill("SIGTERM");
  };
  const interrupt = (): void => {
    child.kill("SIGINT");
  };
  process.once("SIGTERM", terminate);
  process.once("SIGINT", interrupt);
  try {
    await child.stdin.write(input);
    await child.stdin.end();
    return await child.exited;
  } finally {
    process.removeListener("SIGTERM", terminate);
    process.removeListener("SIGINT", interrupt);
    if (child.exitCode === null) {
      child.kill("SIGKILL");
      await child.exited;
    }
  }
};
export const runNativeOwner = async (root: string, payload: string): Promise<number> => {
  if (Bun.version !== "1.4.0" || process.platform !== "linux" || process.arch !== "arm64") throw new Error("Native owner launcher platform/runtime mismatch");
  root = await realpath(root);
  const uid = process.getuid?.();
  if (uid === undefined) throw new Error("Native owner UID is unavailable");
  await verifyCgroup();
  const config = await privateConfig(root);
  await verifyNativeOwnerPayload(payload, config);
  const runtime = nativeOwnerRuntime(root);
  if ((await realpath(process.execPath)) !== runtime || nativeOwnerDigest(await readNativeOwnerFile(runtime, 100_000_000, true)) !== NATIVE_OWNER_BUN_SHA256)
    throw new Error("Native owner launcher runtime integrity mismatch");
  const replay = join(root, ".data/native-owner/replay");
  const directory = await lstat(replay);
  if (!directory.isDirectory() || directory.isSymbolicLink() || directory.uid !== uid || (directory.mode & 0o077) !== 0)
    throw new Error("Native owner replay directory is unsafe");
  const allowed = ["nonces.sqlite3", "nonces.sqlite3-wal", "nonces.sqlite3-shm"];
  if ((await readdir(replay)).some((name) => !allowed.includes(name))) throw new Error("Native owner replay directory is not dedicated");
  const environmentFile = join(root, ".env");
  const environmentInfo = await lstat(environmentFile);
  if (
    !environmentInfo.isFile() ||
    environmentInfo.isSymbolicLink() ||
    environmentInfo.nlink !== 1 ||
    environmentInfo.uid !== uid ||
    (environmentInfo.mode & 0o077) !== 0
  )
    throw new Error("Native owner root environment is unsafe");
  delete process.env.UOS_NATIVE_CARRIER;
  delete process.env.UOS_CANDIDATE_NATIVE_V1;
  delete process.env.UOS_CANDIDATE_INITIAL_SEAL_V1;
  loadEnvFile(environmentFile);
  const environmentAfter = await lstat(environmentFile);
  if (
    environmentInfo.dev !== environmentAfter.dev ||
    environmentInfo.ino !== environmentAfter.ino ||
    environmentInfo.mtimeMs !== environmentAfter.mtimeMs ||
    environmentInfo.size !== environmentAfter.size
  )
    throw new Error("Native owner root environment changed");
  if (loadedSetting("UOS_CANDIDATE_NATIVE_V1") || loadedSetting("UOS_CANDIDATE_INITIAL_SEAL_V1"))
    throw new Error("Candidate readonly binding shadows native owner");
  const raw = loadedSetting("UOS_NATIVE_CARRIER");
  if (!raw || raw.length > 16_384) throw new Error("Native owner carrier is missing or invalid");
  const input = nativeOwnerStdin(root, config, parseNativeOwnerCarrier(JSON.parse(raw)));
  return superviseNativeOwner(join(payload, "bin/uos-native"), root, input);
};
if (import.meta.main) {
  try {
    if (process.argv.length !== 2) throw new Error("Native owner launcher takes no arguments");
    const payload = await realpath(fileURLToPath(new URL("../", import.meta.url)));
    process.exitCode = await runNativeOwner(process.cwd(), payload);
  } catch {
    console.error("native_owner_launch_refused");
    process.exitCode = 1;
  }
}
