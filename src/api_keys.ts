import { encodeHex } from "./utils.ts";
import { isRecord } from "./utils.ts";

export const API_KEY_ID_PREFIX = ["ubq_ai", "api_keys", "id"] as const;
export const API_KEY_HASH_PREFIX = ["ubq_ai", "api_keys", "hash"] as const;
export const API_KEY_NO_EXPIRATION_MS = -1;

export const apiKeyIdKey = (id: string) => [...API_KEY_ID_PREFIX, id] as const;
export const apiKeyHashKey = (hash: string) => [...API_KEY_HASH_PREFIX, hash] as const;

export const generateApiKeyToken = (): string => {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return encodeHex(bytes);
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
