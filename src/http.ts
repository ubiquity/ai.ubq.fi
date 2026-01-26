import { config } from "./config.ts";

export const corsHeaders = (): HeadersInit => ({
  "Access-Control-Allow-Origin": config.allowOrigin,
  "Access-Control-Allow-Methods": "GET,POST,PUT,PATCH,DELETE,OPTIONS",
  "Access-Control-Allow-Headers":
    "Authorization,Content-Type,OpenAI-Beta,OpenAI-Organization,OpenAI-Project,X-GitHub-Owner,X-GitHub-Repo,X-GitHub-Installation-Id,X-Ubiquity-Kernel-Token",
  "Access-Control-Expose-Headers": "x-uos-warning",
  "Access-Control-Max-Age": "86400",
});

export const withCors = (response: Response): Response => {
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
): Response => {
  console.trace(`[ai.ubq.fi] OpenAI API error (${status}):`, message);
  return json(status, {
    error: {
      message,
      type: "invalid_request_error",
      code,
    },
  });
};

export const getBearerToken = (req: Request): string | null => {
  const value = req.headers.get("Authorization");
  if (!value) return null;
  const match = value.match(/^Bearer\s+(.+)$/i);
  return match?.[1]?.trim() || null;
};
