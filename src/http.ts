import { parseTrustedAuthRelayOrigin } from "./auth_relay.ts";
import { config, runtimeDeploymentId, runtimeGitSha } from "./config.ts";
import { CEREBRAS_RATE_LIMIT_HEADERS } from "./cerebras_rate_limits.ts";

export const STANDARD_RATE_LIMIT_HEADERS = [
  "RateLimit",
  "RateLimit-Policy",
  "RateLimit-Limit",
  "RateLimit-Remaining",
  "RateLimit-Reset",
] as const;

const EXPOSED_RESPONSE_HEADERS = [
  "x-uos-warning",
  "x-uos-request-id",
  "x-uos-provider-request-id",
  "x-uos-upstream",
  "x-uos-router-revision",
  "x-uos-git-sha",
  "x-uos-deployment-id",
  "x-uos-cache",
  "ETag",
  "Retry-After",
  ...STANDARD_RATE_LIMIT_HEADERS,
  ...CEREBRAS_RATE_LIMIT_HEADERS,
  "x-codex-limit-name",
  "x-codex-primary-used-percent",
] as const;

const getRequestOrigin = (req?: Request): string | null => {
  const headerOrigin = req?.headers.get("origin");
  if (headerOrigin !== null && headerOrigin !== undefined) return parseTrustedAuthRelayOrigin(headerOrigin);
  try {
    return parseTrustedAuthRelayOrigin(new URL(req?.url ?? "").searchParams.get("cors_origin"));
  } catch {
    return null;
  }
};

export const corsHeaders = (req?: Request): HeadersInit => {
  const requestOrigin = getRequestOrigin(req);
  const configuredOrigin = config.allowOrigin;
  const canUseCredentials = Boolean(requestOrigin) &&
    (configuredOrigin === "*" || configuredOrigin === requestOrigin);
  return {
    "Access-Control-Allow-Origin": canUseCredentials ? requestOrigin! : configuredOrigin,
    "Access-Control-Allow-Methods": "GET,POST,PUT,PATCH,DELETE,OPTIONS",
    "Access-Control-Allow-Headers":
      "Authorization,Content-Type,If-None-Match,OpenAI-Beta,OpenAI-Organization,OpenAI-Project,X-GitHub-Owner,X-GitHub-Repo,X-GitHub-Installation-Id,X-Ubiquity-Kernel-Token",
    // Allow browser clients to read quota state, gateway warnings, cache validators, and backoff hints.
    "Access-Control-Expose-Headers": EXPOSED_RESPONSE_HEADERS.join(","),
    "Access-Control-Max-Age": "86400",
    ...(canUseCredentials ? { "Access-Control-Allow-Credentials": "true", Vary: "Origin" } : {}),
  };
};

export const withCors = (response: Response, req?: Request): Response => {
  const headers = new Headers(response.headers);
  headers.set("x-uos-git-sha", runtimeGitSha());
  headers.set("x-uos-deployment-id", runtimeDeploymentId());
  if (!headers.has("x-uos-request-id")) headers.set("x-uos-request-id", crypto.randomUUID());
  for (const [key, value] of Object.entries(corsHeaders(req))) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

export const json = (
  status: number,
  body: unknown,
  extraHeaders: HeadersInit = {},
): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...extraHeaders,
    },
  });

export const openaiError = (
  status: number,
  message: string,
  code?: string,
  options: { type?: string; param?: string | null; headers?: HeadersInit } = {},
): Response => {
  const type = (options.type ?? "invalid_request_error").trim() || "invalid_request_error";
  const error: Record<string, unknown> = {
    message,
    type,
    code,
  };
  if (Object.prototype.hasOwnProperty.call(options, "param")) {
    error.param = options.param ?? null;
  }
  return json(status, {
    error,
  }, options.headers);
};

export const notFound = (): Response =>
  json(404, {
    error: {
      message: "Not found",
      type: "not_found",
      code: "not_found",
    },
  });

export const getBearerToken = (req: Request): string | null => {
  const value = req.headers.get("Authorization");
  if (!value) return null;
  const match = value.match(/^Bearer\s+(.+)$/i);
  return match?.[1]?.trim() || null;
};
