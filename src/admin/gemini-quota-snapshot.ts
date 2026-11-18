import { constants, type Stats } from "node:fs";
import { lstat, open } from "node:fs/promises";
import { basename, join, resolve } from "node:path";
import { isDeepStrictEqual } from "node:util";
import { isRecord } from "../utils.ts";

export const GEMINI_QUOTA_FILE = "antigravity-quota.json";
const MAX_SNAPSHOT_BYTES = 64 * 1024;
const MAX_BUCKETS = 64;
const MAX_DATE_MS = 8_640_000_000_000_000;

type QuotaBucket = Readonly<{
  remaining_fraction?: number;
  remaining_amount?: number;
  reset_time?: string;
  reset_in_seconds?: number;
  disabled?: boolean;
}>;

export type GeminiQuotaObservation =
  Readonly<{ status: "observed"; plan_tier: string | null; quota: Record<string, QuotaBucket> }> | Readonly<{ status: "signed_out" | "invalid_event" }>;

export type GeminiQuotaSnapshot = Readonly<{ version: 1; observed_at_ms: number }> & GeminiQuotaObservation;

const isObject = (value: unknown): value is Record<string, unknown> => isRecord(value) && !Array.isArray(value);

const shortLabel = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const label = value.trim();
  return label.length > 0 && label.length <= 96 && !/[@\p{C}]/u.test(label) ? label : null;
};

const hasSignedInEmail = (value: unknown): boolean => typeof value === "string" && value.length <= 254 && /^[^@\s\p{C}]+@[^@\s\p{C}]+$/u.test(value.trim());

const parseResetTime = (value: unknown): string | undefined => {
  if (typeof value !== "string" || !/^\d{4}-\d\d-\d\dT(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d(?:\.\d{1,9})?(?:Z|[+-]\d\d:\d\d)$/i.test(value)) return undefined;
  const day = value.slice(0, 10);
  const midnight = Date.parse(`${day}T00:00:00Z`);
  if (!Number.isFinite(midnight) || new Date(midnight).toISOString().slice(0, 10) !== day) return undefined;
  const timestamp = Date.parse(value);
  if (!Number.isFinite(timestamp)) return undefined;
  const normalized = new Date(timestamp).toISOString();
  return normalized.length === 24 ? normalized : undefined;
};

const nonnegativeNumber = (value: unknown, max: number): value is number => typeof value === "number" && Number.isFinite(value) && value >= 0 && value <= max;

const projectBucket = (raw: Record<string, unknown>): QuotaBucket => {
  const { remaining_fraction, remaining_amount, reset_in_seconds, disabled } = raw;
  const fraction = nonnegativeNumber(remaining_fraction, 1) ? remaining_fraction : undefined;
  const amount = nonnegativeNumber(remaining_amount, Number.MAX_SAFE_INTEGER) ? remaining_amount : undefined;
  const reset = parseResetTime(raw.reset_time);
  const seconds = nonnegativeNumber(reset_in_seconds, (MAX_DATE_MS - Date.now()) / 1000) ? reset_in_seconds : undefined;
  // Conflicting reported amounts leave remaining quota unknown. An absolute reset takes precedence over the countdown.
  return {
    ...(fraction !== undefined && amount === undefined ? { remaining_fraction: Math.abs(fraction) } : {}),
    ...(amount !== undefined && fraction === undefined ? { remaining_amount: Math.abs(amount) } : {}),
    ...(reset !== undefined ? { reset_time: reset } : {}),
    ...(reset === undefined && seconds !== undefined ? { reset_in_seconds: Math.abs(seconds) } : {}),
    ...(typeof disabled === "boolean" ? { disabled } : {}),
  };
};

const projectQuota = (plan: unknown, quota: unknown): GeminiQuotaObservation => {
  if (!isObject(quota)) return { status: "invalid_event" };
  const entries = Object.entries(quota);
  if (entries.length > MAX_BUCKETS) return { status: "invalid_event" };
  return {
    status: "observed",
    plan_tier: shortLabel(plan),
    quota: Object.fromEntries(entries.flatMap(([id, raw]) => (shortLabel(id) === id && isObject(raw) ? [[id, projectBucket(raw)]] : []))),
  };
};

export const projectAntigravityStatusLine = (input: unknown): GeminiQuotaObservation => {
  if (!isObject(input) || input.product !== "antigravity") return { status: "invalid_event" };
  if (input.agent_state === "authenticating" || !hasSignedInEmail(input.email)) return { status: "signed_out" };
  return projectQuota(input.plan_tier, input.quota ?? {});
};

export const requireGeminiQuotaDirectory = async (path: string): Promise<void> => {
  if (resolve(path) !== path || basename(path) !== ".data") throw new Error("Expected an absolute gateway state directory");
  const dir = await lstat(path);
  if (!dir.isDirectory() || dir.uid !== process.getuid?.() || (dir.mode & 0o022) !== 0) throw new Error("Unsafe gateway state directory");
};

export const isPrivateGeminiQuotaFile = (file: Stats): boolean =>
  file.isFile() && file.nlink <= 1 && file.uid === process.getuid?.() && (file.mode & 0o7777) === 0o600;

const decodeSnapshot = (value: unknown): GeminiQuotaSnapshot | null => {
  if (!isObject(value) || value.version !== 1 || !nonnegativeNumber(value.observed_at_ms, Date.now()) || !Number.isSafeInteger(value.observed_at_ms))
    return null;
  const { status, observed_at_ms } = value;
  if (status !== "observed" && status !== "signed_out" && status !== "invalid_event") return null;
  const observation: GeminiQuotaObservation = status === "observed" ? projectQuota(value.plan_tier, value.quota) : { status };
  const snapshot: GeminiQuotaSnapshot = { version: 1, observed_at_ms, ...observation };
  return isDeepStrictEqual(value, snapshot) ? snapshot : null;
};

export const readGeminiQuotaSnapshot = async (directory: string): Promise<GeminiQuotaSnapshot | Readonly<{ status: "missing" | "unavailable" }>> => {
  try {
    await requireGeminiQuotaDirectory(directory);
    // Open the inode before validating it. A concurrent rename can only give this reader a complete old or new file.
    const file = await open(join(directory, GEMINI_QUOTA_FILE), constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
    try {
      const stat = await file.stat();
      if (!isPrivateGeminiQuotaFile(stat) || stat.size > MAX_SNAPSHOT_BYTES) return { status: "unavailable" };
      const bytes = new Uint8Array(stat.size + 1);
      let length = 0;
      while (length < bytes.length) {
        const { bytesRead } = await file.read(bytes, length, bytes.length - length, length);
        if (bytesRead === 0) break;
        length += bytesRead;
      }
      if (length !== stat.size) return { status: "unavailable" };
      const body: unknown = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, length)));
      return decodeSnapshot(body) ?? { status: "unavailable" };
    } finally {
      await file.close();
    }
  } catch (error) {
    return isRecord(error) && error.code === "ENOENT" ? { status: "missing" } : { status: "unavailable" };
  }
};
