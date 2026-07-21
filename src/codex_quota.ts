import type { YunwuQuotaSnapshot } from "./yunwu_quota.ts";

export const YUNWU_CODEX_LIMIT_NAME = "YunWu balance";
export const OPENAI_SUBSCRIPTION_LIMIT_NAME = "OpenAI subscription";

const DEFAULT_CODEX_PREFIX = "x-codex";
const OPENAI_SUBSCRIPTION_PREFIX = "x-openai-subscription";

const RATE_LIMIT_WINDOW_SUFFIXES = [
  "primary-used-percent",
  "primary-window-minutes",
  "primary-reset-at",
  "secondary-used-percent",
  "secondary-window-minutes",
  "secondary-reset-at",
] as const;

const SHARED_CODEX_QUOTA_HEADERS = [
  "x-codex-credits-has-credits",
  "x-codex-credits-unlimited",
  "x-codex-credits-balance",
  "x-codex-primary-over-secondary-limit-percent",
  "x-codex-rate-limit-reached-type",
] as const;

const formatPercent = (value: number): string => {
  const rounded = Math.round(Math.min(100, Math.max(0, value)) * 1_000_000) / 1_000_000;
  return String(rounded);
};

export const buildCodexQuotaHeaders = (
  input: HeadersInit,
  snapshot: YunwuQuotaSnapshot | null,
): Headers => {
  const headers = new Headers(input);
  let copiedOpenAiLimit = false;

  for (const suffix of RATE_LIMIT_WINDOW_SUFFIXES) {
    const canonicalName = `${DEFAULT_CODEX_PREFIX}-${suffix}`;
    const namedName = `${OPENAI_SUBSCRIPTION_PREFIX}-${suffix}`;
    const upstreamValue = headers.get(canonicalName);
    headers.delete(canonicalName);
    headers.delete(namedName);
    if (upstreamValue !== null) {
      headers.set(namedName, upstreamValue);
      copiedOpenAiLimit = true;
    }
  }

  headers.delete(`${DEFAULT_CODEX_PREFIX}-limit-name`);
  headers.delete(`${OPENAI_SUBSCRIPTION_PREFIX}-limit-name`);
  if (copiedOpenAiLimit) {
    headers.set(`${OPENAI_SUBSCRIPTION_PREFIX}-limit-name`, OPENAI_SUBSCRIPTION_LIMIT_NAME);
  }
  for (const name of SHARED_CODEX_QUOTA_HEADERS) headers.delete(name);

  if (snapshot?.used_percent !== null && snapshot?.used_percent !== undefined) {
    headers.set(`${DEFAULT_CODEX_PREFIX}-limit-name`, YUNWU_CODEX_LIMIT_NAME);
    headers.set(`${DEFAULT_CODEX_PREFIX}-primary-used-percent`, formatPercent(snapshot.used_percent));
  }
  return headers;
};

export const withCodexQuotaHeaders = (
  response: Response,
  snapshot: YunwuQuotaSnapshot | null,
): Response =>
  new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers: buildCodexQuotaHeaders(response.headers, snapshot),
  });
