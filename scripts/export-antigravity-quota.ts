import { dlopen, FFIType } from "bun:ffi";
import { constants, type Stats } from "node:fs";
import { open, rename, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { isDeepStrictEqual } from "node:util";
import {
  GEMINI_QUOTA_FILE,
  isPrivateGeminiQuotaFile,
  projectAntigravityStatusLine,
  readGeminiQuotaSnapshot,
  requireGeminiQuotaDirectory,
  type GeminiQuotaSnapshot,
} from "../src/admin/gemini-quota-snapshot.ts";

const MAX_INPUT_BYTES = 256 * 1024;
const OBSERVATION_INTERVAL_MS = 30_000;
const RESET_COUNTDOWN_TOLERANCE_MS = 1_000;

const readEvent = async (): Promise<unknown> => {
  const input = Bun.stdin.stream().getReader();
  const bytes = new Uint8Array(MAX_INPUT_BYTES);
  let length = 0;
  const deadline = AbortSignal.timeout(1_500);
  const cancel = () => {
    void input.cancel().catch(() => {});
  };
  deadline.addEventListener("abort", cancel, { once: true });
  try {
    for (let next = await input.read(); !next.done; next = await input.read()) {
      if (next.value.byteLength > bytes.byteLength - length) return null;
      bytes.set(next.value, length);
      length += next.value.byteLength;
    }
    if (deadline.aborted) return null;
    return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, length))) as unknown;
  } catch {
    return null;
  } finally {
    deadline.removeEventListener("abort", cancel);
    cancel();
  }
};

const sameQuota = (previous: GeminiQuotaSnapshot, current: GeminiQuotaSnapshot): boolean => {
  if (previous.status !== "observed" || current.status !== "observed") return previous.status === current.status;
  if (previous.plan_tier !== current.plan_tier || Object.keys(previous.quota).length !== Object.keys(current.quota).length) return false;
  return Object.entries(current.quota).every(([id, bucket]) => {
    if (!Object.hasOwn(previous.quota, id)) return false;
    const { reset_in_seconds: seconds, ...values } = bucket;
    const { reset_in_seconds: priorSeconds, ...priorValues } = previous.quota[id];
    if (!isDeepStrictEqual(values, priorValues)) return false;
    if (seconds === priorSeconds) return true;
    if (seconds === undefined || priorSeconds === undefined) return false;
    // A falling countdown to the same estimated instant is the same quota.
    return Math.abs(current.observed_at_ms + seconds * 1000 - (previous.observed_at_ms + priorSeconds * 1000)) <= RESET_COUNTDOWN_TOLERANCE_MS;
  });
};

const publishEvent = async (directory: string, path: string): Promise<void> => {
  const observation = projectAntigravityStatusLine(await readEvent());
  const snapshot: GeminiQuotaSnapshot = { version: 1, observed_at_ms: Date.now(), ...observation };
  const previous = await readGeminiQuotaSnapshot(directory);
  if ("observed_at_ms" in previous) {
    const age = snapshot.observed_at_ms - previous.observed_at_ms;
    if (age >= 0 && age < OBSERVATION_INTERVAL_MS && sameQuota(previous, snapshot)) return;
  }

  // Every writer holds the same lock, so a crash can leave at most one private temporary file.
  const temporary = `${path}.tmp`;
  await rm(temporary, { force: true });
  try {
    await writeFile(temporary, JSON.stringify(snapshot) + "\n", { mode: 0o600, flag: "wx" });
    await rename(temporary, path);
  } finally {
    await rm(temporary, { force: true });
  }
};

const acquire = async (fd: number, flock: (fd: number, operation: number) => number, stat: Stats): Promise<void> => {
  if (!isPrivateGeminiQuotaFile(stat) || stat.nlink !== 1) throw new Error("Unsafe Antigravity quota lock");
  const deadline = performance.now() + 2_000;
  const exclusiveNonblocking = 2 | 4;
  while (flock(fd, exclusiveNonblocking) !== 0) {
    if (performance.now() >= deadline) throw new Error("Antigravity quota lock timed out");
    await Bun.sleep(10);
  }
};

const exportStatusLine = async (directory: string): Promise<void> => {
  if (process.platform !== "linux" && process.platform !== "darwin") throw new Error("Antigravity quota export requires POSIX file locking");
  await requireGeminiQuotaDirectory(directory);
  const path = join(directory, GEMINI_QUOTA_FILE);
  // Use the platform's flock so termination releases the lock without any stale-lock cleanup.
  const libc = dlopen(process.platform === "darwin" ? "/usr/lib/libSystem.B.dylib" : "libc.so.6", {
    flock: { args: [FFIType.i32, FFIType.i32], returns: FFIType.i32 },
  });
  try {
    const lock = await open(`${path}.lock`, constants.O_WRONLY | constants.O_CREAT | constants.O_NOFOLLOW | constants.O_NONBLOCK, 0o600);
    try {
      await acquire(lock.fd, libc.symbols.flock, await lock.stat());
      // Google supplies no event clock. Serialize complete observations, including sign-out, on this host.
      await publishEvent(directory, path);
    } finally {
      await lock.close();
    }
  } finally {
    libc.close();
  }
};

if (import.meta.main) {
  // stdout is reserved for the CLI's status line. Keep both streams silent, including on bad input.
  const directory = process.argv.length === 3 ? process.argv[2] : undefined;
  if (directory === undefined) process.exitCode = 1;
  else
    await exportStatusLine(directory).catch(() => {
      process.exitCode = 1;
    });
}
