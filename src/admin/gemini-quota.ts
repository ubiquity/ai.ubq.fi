import { dirname, resolve } from "node:path";
import { BUN_KV_DATABASE_PATH } from "#kv";
import { json } from "../http.ts";
import { readGeminiQuotaSnapshot } from "./gemini-quota-snapshot.ts";

export const handleAdminGeminiQuota = async (): Promise<Response> => {
  // The immutable launchers use the repository root as cwd, outside the selected release.
  const result = await readGeminiQuotaSnapshot(dirname(resolve(BUN_KV_DATABASE_PATH)));
  const body = "observed_at_ms" in result ? { ...result, age_ms: Math.max(0, Date.now() - result.observed_at_ms) } : result;
  return json(200, body, { "Cache-Control": "no-store" });
};
