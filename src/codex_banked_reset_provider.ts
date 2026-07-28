/**
 * Boundary for an externally visible Codex usage-reset redemption.
 *
 * The status-only adapter deliberately reads no provider response schemas:
 * an explicit rollout policy treats HTTP 2xx as completion. All other
 * outcomes remain ambiguous and are never retried automatically.
 */

/**
 * Stable, non-secret inputs that bind a redemption to one exhausted account
 * and one observed quota generation. `quotaGeneration` must not be a request
 * identifier: it is the durable identity of the exhaustion window.
 */
export type ResetAccountContext = Readonly<{
  accountId: string;
  accountIdHash: string;
  credentialVersion: string;
  quotaGeneration: string;
}>;

/** A provider's observed reset inventory for one account. */
export type ResetInventory = Readonly<{
  availableCount: number;
  observedAtMs: number;
  resetType: string;
}>;

/**
 * Results deliberately preserve ambiguity. Callers must reconcile `unknown`
 * through `lookup()` and `verifyApplied()` rather than issuing another
 * redemption.
 */
export type RedeemResetResult =
  | Readonly<{ kind: "completed"; providerReceiptId: string }>
  | Readonly<{ kind: "accepted"; providerReceiptId: string }>
  | Readonly<{ kind: "already_redeemed"; providerReceiptId: string }>
  | Readonly<{ kind: "rejected"; reason: string }>
  | Readonly<{ kind: "unknown"; providerReceiptId: string | null }>;

/**
 * Capabilities established from an exact provider contract, not inferred from
 * a successful HTTP response. An unavailable or unreviewed contract must use
 * false/null/empty values so live redemption remains disabled.
 */
export type CodexUsageResetProviderContract = Readonly<{
  idempotency: Readonly<{
    /** The provider accepts the caller's deterministic idempotency key. */
    callerSupplied: boolean;
    /** Documented retention period for that key, or null when unproven. */
    retentionMs: number | null;
  }>;
  lookup: Readonly<{
    /** Required to reconcile an ambiguous request that never returned a receipt. */
    byIdempotencyKey: boolean;
    /** Useful after a receipt has been durably recorded. */
    byProviderReceiptId: boolean;
  }>;
  verification: Readonly<{
    /** A distinct observation can prove the quota reset actually took effect. */
    independentlyVerifiable: boolean;
  }>;
  /**
   * Explicit rollout policy: every HTTP 2xx from `redeem` is final enough to
   * repair routing and issue the single post-reset inference retry. This is
   * not inferred from arbitrary HTTP success.
   */
  http2xxIsFinal?: boolean;
  /**
   * Whether a receipt identifier is non-secret and may be retained in the
   * durable audit record or emitted in logs. When false, reconciliation uses
   * the caller-supplied idempotency key instead.
   */
  receiptIdsSafeToPersistAndLog: boolean;
  /** Only reset types whose exact semantics have been documented and reviewed. */
  supportedResetTypes: readonly string[];
}>;

export type RedeemResetInput =
  & ResetAccountContext
  & Readonly<{
    /** Never log this raw value; persist or emit only a hash. */
    idempotencyKey: string;
  }>;

export type LookupRedeemResetInput =
  & ResetAccountContext
  & Readonly<{
    /** Never log this raw value; persist or emit only a hash. */
    idempotencyKey: string;
    providerReceiptId: string | null;
  }>;

/**
 * Injectable provider boundary. Tests can implement this interface without
 * network access. The unavailable provider remains the fail-closed fallback
 * when an account-bound status-only adapter cannot be constructed.
 */
export interface CodexUsageResetProvider {
  readonly contract: CodexUsageResetProviderContract;
  readInventory(input: ResetAccountContext, signal: AbortSignal): Promise<ResetInventory>;
  redeem(input: RedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult>;
  lookup(input: LookupRedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult>;
  verifyApplied(input: ResetAccountContext, signal: AbortSignal): Promise<boolean>;
}

/** Injectable transport used to keep status-only adapter tests fully offline. */
export type CodexUsageResetFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

/** Credentials are bound in this closure, never placed in a reset context or durable record. */
export type StatusOnlyCodexUsageResetProviderOptions = Readonly<{
  codexBaseUrl: string;
  accountId: string;
  accessToken: string;
  userAgent: string;
  originator: string;
  fetch?: CodexUsageResetFetch;
  now?: () => number;
}>;

export type CodexUsageResetCreditEndpoints = Readonly<{
  inventoryUrl: string;
  consumeUrl: string;
}>;

/** A non-secret configuration error that prevents any reset transport. */
export class CodexUsageResetProviderConfigurationError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CodexUsageResetProviderConfigurationError";
  }
}

/** A non-secret inventory status failure. Its provider response body is never read. */
export class CodexUsageResetProviderHttpError extends Error {
  readonly operation: "inventory";
  readonly status: number;

  constructor(operation: "inventory", status: number) {
    super(`Codex usage-reset ${operation} request failed with status ${status}.`);
    this.name = "CodexUsageResetProviderHttpError";
    this.operation = operation;
    this.status = status;
  }
}

const nonEmptyOption = (value: string, name: string): string => {
  const normalized = value.trim();
  if (!normalized) throw new CodexUsageResetProviderConfigurationError(`${name} must be non-empty.`);
  return normalized;
};

const isHttpSuccess = (status: number): boolean => Number.isInteger(status) && status >= 200 && status < 300;

/**
 * Resolve the documented reset-credit routes from the existing Codex base.
 * Unknown layouts fail closed rather than appending a guessed suffix.
 */
export const resolveCodexUsageResetCreditEndpoints = (codexBaseUrl: string): CodexUsageResetCreditEndpoints => {
  let base: URL;
  try {
    base = new URL(nonEmptyOption(codexBaseUrl, "codexBaseUrl"));
  } catch (error) {
    if (error instanceof CodexUsageResetProviderConfigurationError) throw error;
    throw new CodexUsageResetProviderConfigurationError("codexBaseUrl must be an absolute URL.");
  }
  if (base.protocol !== "https:" && base.protocol !== "http:") {
    throw new CodexUsageResetProviderConfigurationError("codexBaseUrl must use HTTP or HTTPS.");
  }
  if (base.username || base.password || base.search || base.hash) {
    throw new CodexUsageResetProviderConfigurationError(
      "codexBaseUrl must not include credentials, a query string, or a fragment.",
    );
  }
  const pathname = base.pathname.replace(/\/+$/, "") || "/";
  const creditPath = pathname === "/backend-api" || pathname === "/backend-api/codex"
    ? "/backend-api/wham/rate-limit-reset-credits"
    : pathname === "/api/codex"
    ? "/api/codex/rate-limit-reset-credits"
    : null;
  if (!creditPath) {
    throw new CodexUsageResetProviderConfigurationError(
      "codexBaseUrl must use the /backend-api[/codex] or /api/codex layout.",
    );
  }
  const inventory = new URL(base);
  inventory.pathname = creditPath;
  inventory.search = "";
  inventory.hash = "";
  const consume = new URL(inventory);
  consume.pathname = `${creditPath}/consume`;
  return Object.freeze({ inventoryUrl: inventory.toString(), consumeUrl: consume.toString() });
};

/** Discard bytes without parsing a provider schema. */
const discardResponseBody = (response: Response): void => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void Promise.resolve(cancellation).catch(() => {});
  } catch {
    // Body cleanup never changes the status result.
  }
};

const statusOnlyProviderContract: CodexUsageResetProviderContract = Object.freeze({
  // The request accepts our deterministic key. We never retry an ambiguous
  // transport outcome, so no undocumented retention promise is relied on.
  idempotency: Object.freeze({ callerSupplied: true, retentionMs: null }),
  lookup: Object.freeze({ byIdempotencyKey: false, byProviderReceiptId: false }),
  verification: Object.freeze({ independentlyVerifiable: false }),
  http2xxIsFinal: true,
  // `status-NNN` is a local marker, never a provider receipt to retain.
  receiptIdsSafeToPersistAndLog: false,
  supportedResetTypes: Object.freeze(["banked_reset"]) as readonly string[],
});

/**
 * Build the user-approved status-only reset adapter. It never reads a
 * response body: any 2xx inventory means a one-credit preflight and any 2xx
 * consume is the final completion signal. A failed inventory request stops
 * before consume; a non-2xx or transport failure after consume is `unknown`,
 * with no speculative second consume request.
 */
export const createStatusOnlyCodexUsageResetProvider = (
  options: StatusOnlyCodexUsageResetProviderOptions,
): CodexUsageResetProvider => {
  const endpoints = resolveCodexUsageResetCreditEndpoints(options.codexBaseUrl);
  const accountId = nonEmptyOption(options.accountId, "accountId");
  const accessToken = nonEmptyOption(options.accessToken, "accessToken");
  const userAgent = nonEmptyOption(options.userAgent, "userAgent");
  const originator = nonEmptyOption(options.originator, "originator");
  const fetcher = options.fetch ?? ((input: RequestInfo | URL, init?: RequestInit) => globalThis.fetch(input, init));
  const now = options.now ?? Date.now;

  const assertBoundAccount = (input: ResetAccountContext): void => {
    if (input.accountId !== accountId) {
      throw new CodexUsageResetProviderConfigurationError("Reset account does not match the provider-bound account.");
    }
  };
  const headers = (contentType: boolean): Headers => {
    const result = new Headers({
      "Authorization": `Bearer ${accessToken}`,
      "ChatGPT-Account-ID": accountId,
      "originator": originator,
      "user-agent": userAgent,
      "Accept": "application/json",
    });
    if (contentType) result.set("Content-Type", "application/json");
    return result;
  };
  const requestStatus = async (url: string, init: RequestInit, signal: AbortSignal): Promise<number> => {
    if (signal.aborted) throw signal.reason ?? new DOMException("The request was aborted.", "AbortError");
    const response = await fetcher(url, { ...init, signal, redirect: "manual" });
    const status = response.status;
    discardResponseBody(response);
    return status;
  };

  return Object.freeze({
    contract: statusOnlyProviderContract,
    async readInventory(input: ResetAccountContext, signal: AbortSignal): Promise<ResetInventory> {
      assertBoundAccount(input);
      const status = await requestStatus(endpoints.inventoryUrl, { method: "GET", headers: headers(false) }, signal);
      if (!isHttpSuccess(status)) throw new CodexUsageResetProviderHttpError("inventory", status);
      const observedAtMs = now();
      if (!Number.isSafeInteger(observedAtMs) || observedAtMs < 0) {
        throw new CodexUsageResetProviderConfigurationError("now must return a non-negative safe integer.");
      }
      return { availableCount: 1, observedAtMs, resetType: "banked_reset" };
    },
    async redeem(input: RedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult> {
      assertBoundAccount(input);
      const status = await requestStatus(
        endpoints.consumeUrl,
        { method: "POST", headers: headers(true), body: JSON.stringify({ redeem_request_id: input.idempotencyKey }) },
        signal,
      );
      return isHttpSuccess(status)
        ? { kind: "completed", providerReceiptId: `status-${status}` }
        : { kind: "unknown", providerReceiptId: null };
    },
    lookup: (_input: LookupRedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> =>
      Promise.resolve({ kind: "unknown", providerReceiptId: null }),
    verifyApplied: (_input: ResetAccountContext, _signal: AbortSignal): Promise<boolean> => Promise.resolve(false),
  });
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isPositiveSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const hasNonEmptyResetType = (value: unknown): value is string => typeof value === "string" && value.trim().length > 0;

/**
 * Runtime validation for an injected provider's advertised capabilities.
 *
 * The production boundary is intentionally typed, but an adapter is still an
 * external integration and tests can inject arbitrary JavaScript. Treat a
 * malformed contract exactly like an unavailable one: fail closed before
 * inventory, lookup, verification, or redemption is called.
 */
export const providerSupportsResetType = (
  provider: Pick<CodexUsageResetProvider, "contract">,
  resetType: unknown,
): boolean => {
  try {
    const contract = provider?.contract;
    return isRecord(contract) && Array.isArray(contract.supportedResetTypes) &&
      hasNonEmptyResetType(resetType) &&
      contract.supportedResetTypes.some((supported) => supported === resetType && hasNonEmptyResetType(supported));
  } catch {
    return false;
  }
};

/** Returns whether receipt identifiers are explicitly approved for retention. */
export const providerReceiptIdsSafeToPersistAndLog = (
  provider: Pick<CodexUsageResetProvider, "contract">,
): boolean => {
  try {
    return provider?.contract?.receiptIdsSafeToPersistAndLog === true;
  } catch {
    return false;
  }
};

/** Returns true only for a provider that explicitly adopts the 2xx policy. */
export const providerTreatsHttp2xxAsFinal = (
  provider: Pick<CodexUsageResetProvider, "contract">,
): boolean => {
  try {
    return provider?.contract?.http2xxIsFinal === true;
  } catch {
    return false;
  }
};

/**
 * Returns true only when the provider supports conventional reconciliation or
 * explicitly opts into the configured status-only completion policy.
 */
export const providerSupportsLiveRedemption = (
  provider: Pick<CodexUsageResetProvider, "contract">,
): boolean => {
  try {
    const contract = provider?.contract;
    if (
      !isRecord(contract) || !isRecord(contract.idempotency) || !isRecord(contract.lookup) ||
      !isRecord(contract.verification)
    ) {
      return false;
    }
    const supportsResetType = Array.isArray(contract.supportedResetTypes) &&
      contract.supportedResetTypes.some(hasNonEmptyResetType);
    if (!supportsResetType || contract.idempotency.callerSupplied !== true) return false;
    if (contract.http2xxIsFinal === true) return true;
    return isPositiveSafeInteger(contract.idempotency.retentionMs) &&
      contract.lookup.byIdempotencyKey === true &&
      contract.verification.independentlyVerifiable === true;
  } catch {
    return false;
  }
};

/** A stable, non-secret failure used by the default fail-closed provider. */
export class CodexUsageResetProviderUnavailableError extends Error {
  readonly code = "codex_usage_reset_provider_unavailable";

  constructor() {
    super("Codex usage-reset provider is unavailable");
    this.name = "CodexUsageResetProviderUnavailableError";
  }
}

const unavailable = (): never => {
  throw new CodexUsageResetProviderUnavailableError();
};

/**
 * Fail-closed fallback provider. It has no network behavior and fails before
 * any provider interaction. The gateway uses it if it cannot construct the
 * account-bound status-only adapter for a reset candidate.
 */
export const unavailableCodexUsageResetProvider: CodexUsageResetProvider = Object.freeze({
  contract: Object.freeze({
    idempotency: Object.freeze({ callerSupplied: false, retentionMs: null }),
    lookup: Object.freeze({ byIdempotencyKey: false, byProviderReceiptId: false }),
    verification: Object.freeze({ independentlyVerifiable: false }),
    http2xxIsFinal: false,
    receiptIdsSafeToPersistAndLog: false,
    supportedResetTypes: Object.freeze([]) as readonly string[],
  }),
  readInventory: (_input: ResetAccountContext, _signal: AbortSignal): Promise<ResetInventory> => unavailable(),
  redeem: (_input: RedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> => unavailable(),
  lookup: (_input: LookupRedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> => unavailable(),
  verifyApplied: (_input: ResetAccountContext, _signal: AbortSignal): Promise<boolean> => unavailable(),
});
