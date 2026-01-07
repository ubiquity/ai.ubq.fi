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

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const loadConfig = (): Config => {
  const isDeploy = Boolean(getEnv("DENO_DEPLOYMENT_ID") ?? getEnv("DENO_REGION"));
  const authTokens = parseTokens(getEnv("UBIQUITY_AI_TOKEN"));
  const adminTokens = parseTokens(getEnv("DENO_DEPLOY_TOKEN"));
  const allowOrigin = (getEnv("CORS_ALLOW_ORIGIN") ?? "*").trim() || "*";

  const codexBaseUrl = (getEnv("CODEX_BASE_URL") ?? "https://chatgpt.com/backend-api/codex").trim().replace(/\/$/, "");
  const codexAuthJsonB64 = (getEnv("CODEX_AUTH_JSON_B64") ?? "").trim();
  const codexInstructionsB64 = (getEnv("CODEX_INSTRUCTIONS_B64") ?? "").trim() || null;

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
