export const CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage";
export const CODEX_USAGE_TIMEOUT_MS = 8_000;
export const CODEX_USAGE_MAX_RESPONSE_BYTES = 1_048_576;
export const CODEX_TOKEN_EXPIRY_SAFETY_MS = 5 * 60_000;
export const CODEX_USAGE_TRANSIENT_RETRY_ATTEMPTS = 3;
export const CODEX_USAGE_TRANSIENT_RETRY_DELAYS_MS = Object.freeze([250, 1_000]) as readonly number[];

export type CodexAuthSlot = 1 | 2;

export type CodexAuthSlotSecrets = Readonly<{
  slot1B64?: string;
  slot2B64?: string;
}>;

export type CodexAuthTokens = Readonly<{
  id_token: string;
  access_token: string;
  refresh_token: string;
  account_id: string;
}>;

/**
 * This value contains credentials. Do not serialize it, log it, or place it in
 * an artifact. The exact document remains in the parent orchestrator only for
 * validation, output scanning, and the loopback authentication relay. Codex
 * receives a non-secret synthetic document.
 */
export type CodexAuthDocument = Readonly<{
  slot: CodexAuthSlot;
  encoded: string;
  rawJson: string;
  tokens: CodexAuthTokens;
  lastRefresh: string;
  accessTokenExpiresAtMs: number;
}>;

export type CodexAuthFailureCode =
  | "not_configured"
  | "invalid_base64"
  | "invalid_utf8"
  | "invalid_json"
  | "invalid_document"
  | "invalid_access_token"
  | "access_token_expiring";

export class CodexAuthValidationError extends Error {
  readonly code: CodexAuthFailureCode;
  readonly slot: CodexAuthSlot;

  constructor(slot: CodexAuthSlot, code: CodexAuthFailureCode) {
    super(`Codex auth slot ${slot} is not usable (${code}).`);
    this.name = "CodexAuthValidationError";
    this.code = code;
    this.slot = slot;
  }
}

export type CodexUsageFailureCode =
  | CodexAuthFailureCode
  | "timeout"
  | "network_error"
  | "redirect_rejected"
  | "http_error"
  | "response_too_large"
  | "invalid_response_json"
  | "invalid_usage_document"
  | "quota_exhausted";

export type CodexUsageProbe =
  | Readonly<{
    kind: "available";
    slot: CodexAuthSlot;
    headroomPercent: number;
    observedAtMs: number;
  }>
  | Readonly<{
    kind: "unavailable";
    slot: CodexAuthSlot;
    failure: CodexUsageFailureCode;
    status: number | null;
    observedAtMs: number;
  }>;

export type CodexAccountSelection =
  | Readonly<{
    kind: "selected";
    slot: CodexAuthSlot;
    headroomPercent: number;
    probes: readonly [CodexUsageProbe, CodexUsageProbe];
    /** Non-enumerable at runtime. Never log or serialize this property. */
    auth: CodexAuthDocument;
  }>
  | Readonly<{
    kind: "unavailable";
    reason: "no_usable_account";
    probes: readonly [CodexUsageProbe, CodexUsageProbe];
  }>;

export type CodexUsageFetch = (
  input: RequestInfo | URL,
  init?: RequestInit,
) => Promise<Response>;

export type CodexUsageProbeDependencies = Readonly<{
  fetcher?: CodexUsageFetch;
  now?: () => number;
  usageUrl?: string;
  timeoutMs?: number;
  maxResponseBytes?: number;
  createTimeoutSignal?: (timeoutMs: number) => AbortSignal;
  probeRetry?: CodexUsageProbeRetry;
}>;

export type CodexUsageProbeRetry = Readonly<{
  attempts?: number;
  delaysMs?: readonly number[];
  sleep?: (delayMs: number) => Promise<void>;
}>;

export type SelectCodexAccountOptions =
  & CodexUsageProbeDependencies
  & Readonly<{
    slots: CodexAuthSlotSecrets;
    model: string | null;
    minimumValidityMs: number;
  }>;

type JsonRecord = Record<string, unknown>;

const isRecord = (value: unknown): value is JsonRecord =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const safeNow = (now: (() => number) | undefined): number => {
  const value = Math.trunc((now ?? Date.now)());
  if (!Number.isSafeInteger(value) || value < 0) throw new TypeError("now() must return a safe non-negative integer.");
  return value;
};

const assertMinimumValidity = (minimumValidityMs: number): number => {
  if (!Number.isSafeInteger(minimumValidityMs) || minimumValidityMs < 0) {
    throw new TypeError("minimumValidityMs must be a safe non-negative integer.");
  }
  return minimumValidityMs;
};

const decodeStandardBase64 = (encoded: string, slot: CodexAuthSlot): Uint8Array => {
  if (
    encoded.length === 0 || encoded.trim() !== encoded || encoded.length % 4 === 1 ||
    !/^[A-Za-z0-9+/]+={0,2}$/.test(encoded)
  ) {
    throw new CodexAuthValidationError(slot, "invalid_base64");
  }
  try {
    const binary = atob(encoded);
    return Uint8Array.from(binary, (character) => character.charCodeAt(0));
  } catch {
    throw new CodexAuthValidationError(slot, "invalid_base64");
  }
};

const decodeBase64UrlJson = (encoded: string, slot: CodexAuthSlot): unknown => {
  if (!encoded || !/^[A-Za-z0-9_-]+$/.test(encoded)) {
    throw new CodexAuthValidationError(slot, "invalid_access_token");
  }
  const standard = encoded.replaceAll("-", "+").replaceAll("_", "/");
  const padded = standard + "=".repeat((4 - standard.length % 4) % 4);
  try {
    const binary = atob(padded);
    const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
    return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch {
    throw new CodexAuthValidationError(slot, "invalid_access_token");
  }
};

const accessTokenExpiryMs = (accessToken: string, slot: CodexAuthSlot): number => {
  const parts = accessToken.split(".");
  if (parts.length < 2) throw new CodexAuthValidationError(slot, "invalid_access_token");
  const payload = decodeBase64UrlJson(parts[1]!, slot);
  if (!isRecord(payload) || !Number.isSafeInteger(payload.exp) || (payload.exp as number) <= 0) {
    throw new CodexAuthValidationError(slot, "invalid_access_token");
  }
  const expiresAtMs = (payload.exp as number) * 1_000;
  if (!Number.isSafeInteger(expiresAtMs)) throw new CodexAuthValidationError(slot, "invalid_access_token");
  return expiresAtMs;
};

const nonEmptyString = (value: unknown): string | null =>
  typeof value === "string" && value.length > 0 && value.trim() === value ? value : null;

const parseLastRefresh = (value: unknown): string | null => {
  const timestamp = nonEmptyString(value);
  if (!timestamp) return null;
  const match = timestamp.match(
    /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d{1,9})?(?:Z|[+-](\d{2}):(\d{2}))$/u,
  );
  if (!match) return null;
  const [year, month, day, hour, minute, second] = match.slice(1, 7).map(Number);
  const offsetHour = match[7] === undefined ? 0 : Number(match[7]);
  const offsetMinute = match[8] === undefined ? 0 : Number(match[8]);
  const leapYear = year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
  const daysInMonth = [31, leapYear ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1] ?? 0;
  return year > 0 && day >= 1 && day <= daysInMonth && hour <= 23 && minute <= 59 && second <= 59 &&
      offsetHour <= 23 && offsetMinute <= 59 && Number.isFinite(Date.parse(timestamp))
    ? timestamp
    : null;
};

/**
 * Parses a complete file-backed ChatGPT auth document without requiring its
 * current access token to be invocation-ready. This is only for the trusted
 * credential-maintenance lane, which must be able to restore an expired access
 * JWT so the pinned Codex CLI can perform its own refresh.
 */
export const parseCodexAuthJsonB64ForMaintenance = (
  encoded: string | undefined,
  slot: CodexAuthSlot,
): CodexAuthDocument => {
  if (encoded === undefined || encoded === "") throw new CodexAuthValidationError(slot, "not_configured");

  const bytes = decodeStandardBase64(encoded, slot);
  let rawJson: string;
  try {
    rawJson = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    throw new CodexAuthValidationError(slot, "invalid_utf8");
  }

  let value: unknown;
  try {
    value = JSON.parse(rawJson);
  } catch {
    throw new CodexAuthValidationError(slot, "invalid_json");
  }
  const lastRefresh = isRecord(value) ? parseLastRefresh(value.last_refresh) : null;
  if (!isRecord(value) || value.auth_mode !== "chatgpt" || !lastRefresh || !isRecord(value.tokens)) {
    throw new CodexAuthValidationError(slot, "invalid_document");
  }
  const idToken = nonEmptyString(value.tokens.id_token);
  const accessToken = nonEmptyString(value.tokens.access_token);
  const refreshToken = nonEmptyString(value.tokens.refresh_token);
  const accountId = nonEmptyString(value.tokens.account_id);
  if (!idToken || !accessToken || !refreshToken || !accountId) {
    throw new CodexAuthValidationError(slot, "invalid_document");
  }
  const accessTokenExpiresAtMs = accessTokenExpiryMs(accessToken, slot);

  return {
    slot,
    encoded,
    rawJson,
    tokens: {
      id_token: idToken,
      access_token: accessToken,
      refresh_token: refreshToken,
      account_id: accountId,
    },
    lastRefresh,
    accessTokenExpiresAtMs,
  };
};

export const parseCodexAuthJsonB64 = (
  encoded: string | undefined,
  slot: CodexAuthSlot,
  options: Readonly<{ nowMs: number; minimumValidityMs: number }>,
): CodexAuthDocument => {
  const minimumValidityMs = assertMinimumValidity(options.minimumValidityMs);
  if (!Number.isSafeInteger(options.nowMs) || options.nowMs < 0) throw new TypeError("nowMs is invalid.");
  const auth = parseCodexAuthJsonB64ForMaintenance(encoded, slot);
  const requiredUntilMs = options.nowMs + minimumValidityMs;
  if (!Number.isSafeInteger(requiredUntilMs) || auth.accessTokenExpiresAtMs <= requiredUntilMs) {
    throw new CodexAuthValidationError(slot, "access_token_expiring");
  }
  return auth;
};

type UsageWindow = Readonly<{ usedPercent: number }>;

const parseWindow = (value: unknown): UsageWindow | null | "invalid" => {
  if (value === null) return null;
  if (!isRecord(value)) return "invalid";
  const usedPercent = value.used_percent;
  if (typeof usedPercent !== "number" || !Number.isFinite(usedPercent) || usedPercent < 0 || usedPercent > 100) {
    return "invalid";
  }
  return { usedPercent };
};

const normalizedQuotaLabel = (value: string): string => value.toLowerCase().replace(/[^a-z0-9]/g, "");

const rateLimitHeadroom = (value: unknown): number | null => {
  if (!isRecord(value) || !("primary_window" in value) || !("secondary_window" in value)) return null;
  const primary = parseWindow(value.primary_window);
  const secondary = parseWindow(value.secondary_window);
  if (primary === "invalid" || secondary === "invalid" || (!primary && !secondary)) return null;
  const usedPercents = [primary, secondary]
    .filter((window): window is UsageWindow => window !== null)
    .map((window) => window.usedPercent);
  return Math.min(...usedPercents.map((usedPercent) => 100 - usedPercent));
};

export const parseCodexUsageHeadroom = (value: unknown, model: string | null): number | null => {
  if (!isRecord(value)) return null;
  const baseHeadroom = rateLimitHeadroom(value.rate_limit);
  if (baseHeadroom === null) return null;

  const normalizedModel = typeof model === "string" ? normalizedQuotaLabel(model.trim()) : "";
  const matchingHeadrooms: number[] = [];
  if (value.additional_rate_limits !== undefined) {
    if (!Array.isArray(value.additional_rate_limits)) return null;
    for (const candidate of value.additional_rate_limits) {
      if (!isRecord(candidate) || typeof candidate.limit_name !== "string") return null;
      const normalizedLimitName = normalizedQuotaLabel(candidate.limit_name.trim());
      if (!normalizedLimitName) return null;
      const candidateHeadroom = rateLimitHeadroom(candidate.rate_limit);
      if (candidateHeadroom === null) return null;
      if (normalizedModel && normalizedLimitName === normalizedModel) matchingHeadrooms.push(candidateHeadroom);
    }
  }

  return matchingHeadrooms.length ? Math.max(...matchingHeadrooms) : baseHeadroom;
};

const readBoundedResponseText = async (response: Response, limit: number): Promise<string | null> => {
  const declaredLength = response.headers.get("Content-Length");
  if (declaredLength !== null) {
    const parsed = Number(declaredLength);
    if (Number.isFinite(parsed) && parsed > limit) return null;
  }
  if (!response.body) return "";
  const reader = response.body.getReader();
  const chunks: Uint8Array[] = [];
  let byteLength = 0;
  try {
    while (true) {
      const next = await reader.read();
      if (next.done) break;
      byteLength += next.value.byteLength;
      if (byteLength > limit) {
        await reader.cancel().catch(() => undefined);
        return null;
      }
      chunks.push(next.value);
    }
  } finally {
    reader.releaseLock();
  }
  const combined = new Uint8Array(byteLength);
  let offset = 0;
  for (const chunk of chunks) {
    combined.set(chunk, offset);
    offset += chunk.byteLength;
  }
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(combined);
  } catch {
    return "\u0000";
  }
};

const unavailableProbe = (
  slot: CodexAuthSlot,
  failure: CodexUsageFailureCode,
  observedAtMs: number,
  status: number | null = null,
): CodexUsageProbe => ({ kind: "unavailable", slot, failure, status, observedAtMs });

/**
 * True only for probe failures that can reasonably be transient: a timeout, a
 * network or read error, an oversized response, or HTTP rate limiting and
 * upstream server errors. Authoritative signals (quota exhaustion, expired or
 * invalid credentials, malformed usage documents, redirects) are never
 * classified as transient, so they are never retried and never treated as
 * recoverable capacity.
 */
export const isTransientCodexUsageFailure = (probe: CodexUsageProbe): boolean => {
  if (probe.kind === "available") return false;
  if (probe.failure === "timeout" || probe.failure === "network_error" || probe.failure === "response_too_large") {
    return true;
  }
  return probe.failure === "http_error" && probe.status !== null &&
    (probe.status === 429 || probe.status >= 500);
};

export const probeCodexUsage = async (
  auth: CodexAuthDocument,
  model: string | null,
  dependencies: CodexUsageProbeDependencies = {},
): Promise<CodexUsageProbe> => {
  const observedAtMs = safeNow(dependencies.now);
  const timeoutMs = dependencies.timeoutMs ?? CODEX_USAGE_TIMEOUT_MS;
  const maxResponseBytes = dependencies.maxResponseBytes ?? CODEX_USAGE_MAX_RESPONSE_BYTES;
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) throw new TypeError("timeoutMs must be a positive integer.");
  if (!Number.isSafeInteger(maxResponseBytes) || maxResponseBytes <= 0) {
    throw new TypeError("maxResponseBytes must be a positive integer.");
  }
  const fetcher = dependencies.fetcher ?? fetch;
  const createTimeoutSignal = dependencies.createTimeoutSignal ?? AbortSignal.timeout;

  let response: Response;
  try {
    response = await fetcher(dependencies.usageUrl ?? CODEX_USAGE_URL, {
      method: "GET",
      headers: {
        Accept: "application/json",
        Authorization: `Bearer ${auth.tokens.access_token}`,
        "ChatGPT-Account-ID": auth.tokens.account_id,
      },
      redirect: "manual",
      cache: "no-store",
      signal: createTimeoutSignal(timeoutMs),
    });
  } catch (error) {
    const name = error instanceof Error ? error.name : "";
    const failure = name === "AbortError" || name === "TimeoutError" ? "timeout" : "network_error";
    return unavailableProbe(auth.slot, failure, observedAtMs);
  }

  if (response.status >= 300 && response.status < 400) {
    return unavailableProbe(auth.slot, "redirect_rejected", observedAtMs, response.status);
  }
  if (response.status !== 200) return unavailableProbe(auth.slot, "http_error", observedAtMs, response.status);

  let text: string | null;
  try {
    text = await readBoundedResponseText(response, maxResponseBytes);
  } catch (error) {
    const name = error instanceof Error ? error.name : "";
    const failure = name === "AbortError" || name === "TimeoutError" ? "timeout" : "network_error";
    return unavailableProbe(auth.slot, failure, observedAtMs, response.status);
  }
  if (text === null) return unavailableProbe(auth.slot, "response_too_large", observedAtMs, response.status);
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return unavailableProbe(auth.slot, "invalid_response_json", observedAtMs, response.status);
  }
  const headroomPercent = parseCodexUsageHeadroom(value, model);
  if (headroomPercent === null) {
    return unavailableProbe(auth.slot, "invalid_usage_document", observedAtMs, response.status);
  }
  if (headroomPercent <= 0) return unavailableProbe(auth.slot, "quota_exhausted", observedAtMs, response.status);
  return { kind: "available", slot: auth.slot, headroomPercent, observedAtMs };
};

const invalidAuthProbe = (
  slot: CodexAuthSlot,
  error: unknown,
  observedAtMs: number,
): CodexUsageProbe =>
  unavailableProbe(
    slot,
    error instanceof CodexAuthValidationError ? error.code : "invalid_document",
    observedAtMs,
  );

const selectedAccount = (
  auth: CodexAuthDocument,
  probe: Extract<CodexUsageProbe, { kind: "available" }>,
  probes: readonly [CodexUsageProbe, CodexUsageProbe],
): Extract<CodexAccountSelection, { kind: "selected" }> => {
  const value = {
    kind: "selected" as const,
    slot: auth.slot,
    headroomPercent: probe.headroomPercent,
    probes,
  } as Extract<CodexAccountSelection, { kind: "selected" }>;
  Object.defineProperty(value, "auth", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: auth,
  });
  return Object.freeze(value);
};

/** Re-parses and probes both auth slots on every call. */
export const selectCodexAccountForInvocation = async (
  options: SelectCodexAccountOptions,
): Promise<CodexAccountSelection> => {
  const nowMs = safeNow(options.now);
  const minimumValidityMs = assertMinimumValidity(options.minimumValidityMs);
  const encodedBySlot: readonly [string | undefined, string | undefined] = [
    options.slots.slot1B64,
    options.slots.slot2B64,
  ];
  const authBySlot: Array<CodexAuthDocument | null> = [null, null];
  const authFailures: Array<unknown | null> = [null, null];
  for (const slot of [1, 2] as const) {
    try {
      authBySlot[slot - 1] = parseCodexAuthJsonB64(encodedBySlot[slot - 1], slot, { nowMs, minimumValidityMs });
    } catch (error) {
      authFailures[slot - 1] = error;
    }
  }

  const dependencies: CodexUsageProbeDependencies = {
    fetcher: options.fetcher,
    now: () => nowMs,
    usageUrl: options.usageUrl,
    timeoutMs: options.timeoutMs,
    maxResponseBytes: options.maxResponseBytes,
    createTimeoutSignal: options.createTimeoutSignal,
  };
  const retry = options.probeRetry ?? {};
  const retryAttempts = retry.attempts ?? CODEX_USAGE_TRANSIENT_RETRY_ATTEMPTS;
  if (!Number.isSafeInteger(retryAttempts) || retryAttempts < 1 || retryAttempts > 10) {
    throw new TypeError("probeRetry.attempts must be a positive integer no greater than 10");
  }
  const retryDelaysMs = retry.delaysMs ?? CODEX_USAGE_TRANSIENT_RETRY_DELAYS_MS;
  for (const delay of retryDelaysMs) {
    if (!Number.isSafeInteger(delay) || delay < 0) {
      throw new TypeError("probeRetry.delaysMs must be non-negative integers");
    }
  }
  const retrySleep = retry.sleep ??
    ((delayMs: number) => new Promise<void>((resolve) => globalThis.setTimeout(resolve, delayMs)));
  const probeWithRetry = async (auth: CodexAuthDocument): Promise<CodexUsageProbe> => {
    let probe = await probeCodexUsage(auth, options.model, dependencies);
    for (let attempt = 1; attempt < retryAttempts && isTransientCodexUsageFailure(probe); attempt++) {
      const delay = retryDelaysMs.length === 0 ? 0 : retryDelaysMs[Math.min(attempt - 1, retryDelaysMs.length - 1)]!;
      if (delay > 0) await retrySleep(delay);
      probe = await probeCodexUsage(auth, options.model, dependencies);
    }
    return probe;
  };
  const probes = await Promise.all(([1, 2] as const).map((slot) => {
    const auth = authBySlot[slot - 1];
    return auth ? probeWithRetry(auth) : Promise.resolve(invalidAuthProbe(slot, authFailures[slot - 1], nowMs));
  })) as [CodexUsageProbe, CodexUsageProbe];

  const candidates = probes
    .flatMap((probe, index) => probe.kind === "available" ? [{ probe, auth: authBySlot[index]! }] : [])
    .sort((left, right) =>
      right.probe.headroomPercent - left.probe.headroomPercent || left.auth.slot - right.auth.slot
    );
  const winner = candidates[0];
  if (!winner) return { kind: "unavailable", reason: "no_usable_account", probes };
  return selectedAccount(winner.auth, winner.probe, probes);
};
