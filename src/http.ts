import { config } from "./config.ts";

export const corsHeaders = (): HeadersInit => ({
  "Access-Control-Allow-Origin": config.allowOrigin,
  "Access-Control-Allow-Methods": "GET,POST,PUT,PATCH,DELETE,OPTIONS",
  "Access-Control-Allow-Headers":
    "Authorization,Content-Type,If-None-Match,OpenAI-Beta,OpenAI-Organization,OpenAI-Project,X-GitHub-Owner,X-GitHub-Repo,X-GitHub-Installation-Id,X-Ubiquity-Kernel-Token",
  // Allow browser clients to read gateway warnings, cache validators, and backoff hints.
  "Access-Control-Expose-Headers":
    "x-uos-warning,x-uos-request-id,x-ubq-upstream,x-uos-router-revision,x-uos-cache,ETag,Retry-After",
  "Access-Control-Max-Age": "86400",
});

export const withCors = (response: Response): Response => {
  const headers = new Headers(response.headers);
  if (!headers.has("x-uos-request-id")) headers.set("x-uos-request-id", crypto.randomUUID());
  for (const [key, value] of Object.entries(corsHeaders())) {
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
  console.trace(`[ai.ubq.fi] OpenAI API error (${status}):`, message);
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
