import { encodeHex } from "./utils.ts";
import { isRecord } from "./utils.ts";

export const API_KEY_ID_PREFIX = ["ubq_ai", "api_keys", "id"] as const;
export const API_KEY_HASH_PREFIX = ["ubq_ai", "api_keys", "hash"] as const;
export const API_KEY_NO_EXPIRATION_MS = -1;
export const API_KEY_NO_USAGE_LIMIT = -1;

const getEnvNumber = (key: string, defaultValue: number): number => {
  try {
    const value = Deno.env.get(key);
    if (!value) return defaultValue;
    const parsed = Number(value);
    if (!Number.isFinite(parsed)) return defaultValue;
    return Math.trunc(parsed);
  } catch {
    return defaultValue;
  }
};

export const DEFAULT_USAGE_LIMIT_REQUESTS = getEnvNumber("UOS_API_KEY_DEFAULT_USAGE_LIMIT", 50);
export const DEFAULT_EXPIRY_DAYS = getEnvNumber("UOS_API_KEY_DEFAULT_EXPIRY_DAYS", 90);
export const USAGE_RESET_PERIOD_MS = 7 * 24 * 60 * 60 * 1000;

export const apiKeyIdKey = (id: string) => [...API_KEY_ID_PREFIX, id] as const;
export const apiKeyHashKey = (hash: string) => [...API_KEY_HASH_PREFIX, hash] as const;

export const generateApiKeyToken = (): string => {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return `u_${encodeHex(bytes)}`;
};

export const coerceApiKeyExpiresAtMs = (record: unknown): number => {
  if (!isRecord(record)) return API_KEY_NO_EXPIRATION_MS;
  const value = record.expires_at_ms;
  if (typeof value !== "number" || !Number.isFinite(value)) return API_KEY_NO_EXPIRATION_MS;
  const expiresAtMs = Math.trunc(value);
  if (expiresAtMs === API_KEY_NO_EXPIRATION_MS) return API_KEY_NO_EXPIRATION_MS;
  if (expiresAtMs < 0) return API_KEY_NO_EXPIRATION_MS;
  return expiresAtMs;
};

export const calculateNextResetMs = (nowMs: number): number => {
  return nowMs + USAGE_RESET_PERIOD_MS;
};

export const shouldResetUsage = (resetAtMs: number, nowMs: number): boolean => {
  return resetAtMs <= nowMs;
};

export const getDefaultExpiryMs = (nowMs: number): number => {
  return nowMs + DEFAULT_EXPIRY_DAYS * 24 * 60 * 60 * 1000;
};
