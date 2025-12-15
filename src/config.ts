export type Config = Readonly<{
  isDeploy: boolean;
  allowOrigin: string;
  authTokens: ReadonlySet<string>;
  adminTokens: ReadonlySet<string>;
  codexBaseUrl: string;
  codexAuthJsonB64: string;
  codexInstructionsB64: string | null;
}>;

const parseTokens = (raw: string | undefined | null): Set<string> => {
  if (!raw) return new Set();
  return new Set(
    raw
      .split(/[\n,]/g)
      .map((token) => token.trim())
      .filter(Boolean),
  );
};

const loadConfig = (): Config => {
  const isDeploy = Boolean(Deno.env.get("DENO_DEPLOYMENT_ID") ?? Deno.env.get("DENO_REGION"));
  const authTokens = parseTokens(Deno.env.get("UBIQUITY_AI_USER_TOKEN"));
  const adminTokens = parseTokens(Deno.env.get("UBIQUITY_AI_ADMIN_TOKEN"));
  const allowOrigin = (Deno.env.get("CORS_ALLOW_ORIGIN") ?? "*").trim() || "*";

  const codexBaseUrl = (Deno.env.get("CODEX_BASE_URL") ?? "https://chatgpt.com/backend-api/codex")
    .trim()
    .replace(/\/$/, "");
  const codexAuthJsonB64 = (Deno.env.get("CODEX_AUTH_JSON_B64") ?? "").trim();
  const codexInstructionsB64 = (Deno.env.get("CODEX_INSTRUCTIONS_B64") ?? "").trim() || null;

  return {
    isDeploy,
    codexBaseUrl,
    codexAuthJsonB64,
    codexInstructionsB64,
    allowOrigin,
    authTokens,
    adminTokens,
  };
};

export const config = loadConfig();
