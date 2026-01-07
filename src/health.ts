import { config } from "./config.ts";
import { CODEX_KV_KEY, codexInstructionsPromise } from "./codex.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";

export const handleHealth = async (): Promise<Response> => {
  const problems: string[] = [];
  const kv = await kvPromise;
  let hasCodexAuth = Boolean(config.codexAuthJsonB64);
  if (!hasCodexAuth && kv) {
    const entry = await kv.get(CODEX_KV_KEY);
    hasCodexAuth = Boolean(entry.value);
  }
  if (!hasCodexAuth) problems.push("CODEX_AUTH_JSON_B64 missing");
  if (config.isDeploy && config.authTokens.size === 0 && !kv) {
    problems.push("No UBIQUITY_AI_TOKEN and Deno KV unavailable");
  }
  try {
    await codexInstructionsPromise;
  } catch {
    problems.push("CODEX instructions missing (codex_instructions.md)");
  }
  return json(problems.length === 0 ? 200 : 500, {
    ok: problems.length === 0,
    problems,
  });
};
