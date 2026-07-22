export const YUNWU_CODEX_LIMIT_NAME = "YunWu balance";

type ClientQuotaSnapshot = Readonly<{ used_percent: number | null }>;

const DEFAULT_CODEX_PREFIX = "x-codex";

const RATE_LIMIT_FAMILY_SUFFIXES = [
  "-limit-name",
  "-primary-used-percent",
  "-primary-window-minutes",
  "-primary-reset-at",
  "-primary-reset-after-seconds",
  "-secondary-used-percent",
  "-secondary-window-minutes",
  "-secondary-reset-at",
  "-secondary-reset-after-seconds",
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
  snapshot: ClientQuotaSnapshot | null,
): Headers => {
  const headers = new Headers(input);

  // Codex discovers named limit families from any x-*-primary-used-percent header. Version 0.144.6
  // parses every discovered family but persists only one response-derived snapshot, so forwarding
  // multiple families makes whichever name sorts last overwrite the others. Remove every parseable
  // family before publishing the one client-specific capacity source this gateway can measure.
  for (const name of [...headers.keys()]) {
    if (
      name.startsWith("x-") &&
      RATE_LIMIT_FAMILY_SUFFIXES.some((suffix) => name.endsWith(suffix))
    ) {
      headers.delete(name);
    }
  }
  for (const name of SHARED_CODEX_QUOTA_HEADERS) headers.delete(name);

  if (snapshot?.used_percent !== null && snapshot?.used_percent !== undefined) {
    const usedPercent = formatPercent(snapshot.used_percent);
    headers.set(`${DEFAULT_CODEX_PREFIX}-limit-name`, YUNWU_CODEX_LIMIT_NAME);
    headers.set(`${DEFAULT_CODEX_PREFIX}-primary-used-percent`, usedPercent);
  }
  return headers;
};

export const withCodexQuotaHeaders = (
  response: Response,
  snapshot: ClientQuotaSnapshot | null,
): Response =>
  new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers: buildCodexQuotaHeaders(response.headers, snapshot),
  });
