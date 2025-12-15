import { config } from "./config.ts";
import { codexInstructionsPromise } from "./codex.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";

export const handleHealth = async (): Promise<Response> => {
  const problems: string[] = [];
  if (!config.codexAuthJsonB64) problems.push("CODEX_AUTH_JSON_B64 missing");
  const kv = await kvPromise;
  if (config.isDeploy && config.authTokens.size === 0 && !kv) {
    problems.push("No UBIQUITY_AI_USER_TOKEN and Deno KV unavailable");
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
