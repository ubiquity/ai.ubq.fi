export type Config = Readonly<{
  isDeploy: boolean;
  allowOrigin: string;
  authTokens: ReadonlySet<string>;
  adminTokens: ReadonlySet<string>;
  codexBaseUrl: string;
  codexAuthJsonB64: string;
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
  const isDeploy = Boolean(getEnv("DENO_DEPLOY") ?? getEnv("DENO_DEPLOYMENT_ID") ?? getEnv("DENO_REGION"));
  const authTokens = parseTokens(getEnv("UOS_AI_TOKEN"));
  const adminTokens = parseTokens(getEnv("DENO_DEPLOY_TOKEN"));
  const allowOrigin = (getEnv("CORS_ALLOW_ORIGIN") ?? "*").trim() || "*";

  const codexBaseUrl = (getEnv("CODEX_BASE_URL") ?? "https://chatgpt.com/backend-api/codex").trim().replace(/\/$/, "");
  const codexAuthJsonB64 = (getEnv("CODEX_AUTH_JSON_B64") ?? "").trim();

  return {
    isDeploy,
    codexBaseUrl,
    codexAuthJsonB64,
    allowOrigin,
    authTokens,
    adminTokens,
  };
};

export const config = loadConfig();

export const runtimeGitSha = (): string => getEnv("GIT_REVISION")?.trim() || getEnv("GITHUB_SHA")?.trim() || "unknown";
export const runtimeDeploymentId = (): string => getEnv("DENO_DEPLOYMENT_ID")?.trim() || "unknown";
