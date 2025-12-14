/// <reference lib="deno.ns" />

type Config = Readonly<{
  openaiBaseUrl: string;
  openaiApiKey: string;
  allowOrigin: string;
  authTokens: ReadonlySet<string>;
  authMisconfigured: boolean;
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
  const authTokens = parseTokens(Deno.env.get("UBQ_AI_AUTH_TOKENS") ?? Deno.env.get("UBQ_AI_API_KEYS"));
  const allowOrigin = (Deno.env.get("CORS_ALLOW_ORIGIN") ?? "*").trim() || "*";

  const openaiBaseUrl = (Deno.env.get("OPENAI_BASE_URL") ?? "https://api.openai.com").trim().replace(/\/$/, "");
  const openaiApiKey = (Deno.env.get("OPENAI_API_KEY") ?? "").trim();

  return {
    openaiBaseUrl,
    openaiApiKey,
    allowOrigin,
    authTokens,
    authMisconfigured: isDeploy && authTokens.size === 0,
  };
};

const config = loadConfig();

const corsHeaders = (): HeadersInit => ({
  "Access-Control-Allow-Origin": config.allowOrigin,
  "Access-Control-Allow-Methods": "GET,POST,PUT,PATCH,DELETE,OPTIONS",
  "Access-Control-Allow-Headers": "Authorization,Content-Type,OpenAI-Beta,OpenAI-Organization,OpenAI-Project",
  "Access-Control-Max-Age": "86400",
});

const withCors = (response: Response): Response => {
  const headers = new Headers(response.headers);
  for (const [key, value] of Object.entries(corsHeaders())) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const json = (status: number, body: unknown, extraHeaders: HeadersInit = {}): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...extraHeaders,
    },
  });

const openaiError = (status: number, message: string, code?: string): Response =>
  json(status, {
    error: {
      message,
      type: "invalid_request_error",
      code,
    },
  });

const getBearerToken = (req: Request): string | null => {
  const value = req.headers.get("Authorization");
  if (!value) return null;
  const match = value.match(/^Bearer\s+(.+)$/i);
  return match?.[1]?.trim() || null;
};

const proxyToOpenAI = async (req: Request): Promise<Response> => {
  if (config.authMisconfigured) {
    return openaiError(500, "Server misconfigured: UBQ_AI_AUTH_TOKENS is required in production", "server_error");
  }

  if (!config.openaiApiKey) {
    return openaiError(500, "Server misconfigured: OPENAI_API_KEY is missing", "server_error");
  }

  if (config.authTokens.size > 0) {
    const token = getBearerToken(req);
    if (!token || !config.authTokens.has(token)) {
      return openaiError(401, "Unauthorized", "invalid_api_key");
    }
  }

  const url = new URL(req.url);
  const upstreamUrl = `${config.openaiBaseUrl}${url.pathname}${url.search}`;

  const headers = new Headers(req.headers);
  headers.set("Authorization", `Bearer ${config.openaiApiKey}`);
  headers.delete("Host");

  let upstream: Response;
  try {
    upstream = await fetch(upstreamUrl, {
      method: req.method,
      headers,
      body: req.body,
      redirect: "manual",
    });
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    return openaiError(502, "Upstream request failed", "bad_gateway");
  }

  const respHeaders = new Headers(upstream.headers);
  respHeaders.set("x-ubq-upstream", "openai");
  return new Response(upstream.body, {
    status: upstream.status,
    statusText: upstream.statusText,
    headers: respHeaders,
  });
};

async function handler(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") {
    return withCors(new Response(null, { status: 204, headers: corsHeaders() }));
  }

  const url = new URL(req.url);
  const path = url.pathname;

  if (req.method === "GET" && path === "/") {
    return withCors(
      json(200, {
        ok: true,
        service: "ai.ubq.fi",
        openai_base_url: config.openaiBaseUrl,
        auth: config.authMisconfigured
          ? "misconfigured"
          : config.authTokens.size > 0
          ? "required"
          : "disabled (local only)",
        endpoints: {
          openai: "/v1/*",
          health: "/health",
        },
      }),
    );
  }

  if (req.method === "GET" && path === "/health") {
    const problems: string[] = [];
    if (!config.openaiApiKey) problems.push("OPENAI_API_KEY missing");
    if (config.authMisconfigured) problems.push("UBQ_AI_AUTH_TOKENS missing");
    return withCors(
      json(problems.length === 0 ? 200 : 500, {
        ok: problems.length === 0,
        problems,
      }),
    );
  }

  if (!path.startsWith("/v1/")) {
    return withCors(openaiError(404, "Not found", "not_found"));
  }

  const response = await proxyToOpenAI(req);
  return withCors(response);
}

export default { fetch: handler };
