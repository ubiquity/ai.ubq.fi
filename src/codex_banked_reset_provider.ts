/**
 * Boundary for an externally visible Codex usage-reset redemption.
 *
 * The upstream Codex reset-credit API has a documented inventory and consume
 * schema. HTTP 2xx is the transport-success gate; the response `code` is the
 * terminal redemption result.
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

/** A fully detailed opaque reset credit returned for one account. */
export type ResetInventoryCredit = Readonly<{
  /** Opaque upstream identifier. It is hashed before reaching telemetry or KV. */
  id: string;
  status: string;
  resetType: string;
  /** Null means the provider declares the credit non-expiring. */
  expiresAtMs: number | null;
}>;

/**
 * A provider's observed reset inventory for one account.  An omitted or
 * capped credit list is deliberately not representable: the caller must be
 * able to prove that it selected a specific available credit before it can
 * ever submit a consume request.
 */
export type ResetInventory = Readonly<{
  availableCount: number;
  observedAtMs: number;
  credits: readonly ResetInventoryCredit[];
}>;

/**
 * Results deliberately preserve ambiguity. Callers must never issue another
 * redemption for `unknown`: reconcilable providers use `lookup()` and
 * `verifyApplied()`, while terminal-only providers retain the ambiguity.
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
   * The provider's parsed terminal `redeem` result is authoritative for this
   * one submission. Ambiguous transport or response outcomes never qualify.
   */
  redeemOutcomeIsFinal?: boolean;
  /**
   * Whether a receipt identifier is non-secret and may be retained in the
   * durable audit record or emitted in logs. When false, a provider that
   * supports reconciliation uses the caller-supplied idempotency key instead.
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
    /** Exact opaque credit selected from the immediately-read complete inventory. */
    creditId: string;
  }>;

export type LookupRedeemResetInput =
  & ResetAccountContext
  & Readonly<{
    /** Never log this raw value; persist or emit only a hash. */
    idempotencyKey: string;
    providerReceiptId: string | null;
  }>;

/**
 * Injectable provider boundary. Tests can implement this interface with an
 * injected transport, so automated validation never reaches the real service.
 */
export interface CodexUsageResetProvider {
  readonly contract: CodexUsageResetProviderContract;
  readInventory(input: ResetAccountContext, signal: AbortSignal): Promise<ResetInventory>;
  redeem(input: RedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult>;
  lookup(input: LookupRedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult>;
  verifyApplied(input: ResetAccountContext, signal: AbortSignal): Promise<boolean>;
}

/** Injectable transport used to keep upstream-adapter tests fully offline. */
export type CodexUsageResetFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

/** Credentials are bound in this closure, never placed in a durable reset context or record. */
export type UpstreamCodexUsageResetProviderOptions = Readonly<{
  codexBaseUrl: string;
  accountId: string;
  accessToken: string;
  userAgent: string;
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

/** A non-secret inventory-status failure. Its response body is never read. */
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

/** Validate whitespace-only text without changing an opaque protocol value. */
const requireNonBlank = (value: string, name: string): void => {
  if (!value.trim()) throw new CodexUsageResetProviderConfigurationError(`${name} must be non-empty.`);
};

const isHttpSuccess = (status: number): boolean => Number.isInteger(status) && status >= 200 && status < 300;
const isNonnegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const parsedExpiry = (value: unknown): number | null | undefined => {
  if (value === null) return null;
  if (typeof value !== "string" || !value.trim()) return undefined;
  const parsed = Date.parse(value);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : undefined;
};

const parseDetailedCredits = (value: unknown, availableCount: number): readonly ResetInventoryCredit[] | null => {
  if (!Array.isArray(value)) return null;
  const credits: ResetInventoryCredit[] = [];
  const ids = new Set<string>();
  let detailedAvailableCount = 0;
  for (const item of value) {
    if (!isRecord(item)) return null;
    const id = typeof item.id === "string" ? item.id : "";
    const status = typeof item.status === "string" ? item.status : "";
    const resetType = typeof item.reset_type === "string" ? item.reset_type : "";
    const expiresAtMs = parsedExpiry(item.expires_at);
    if (!id.trim() || !status.trim() || !resetType.trim() || expiresAtMs === undefined || ids.has(id)) return null;
    ids.add(id);
    if (status === "available") detailedAvailableCount += 1;
    credits.push({ id, status, resetType, expiresAtMs });
  }
  // The upstream may cap the list. A capped or summary-only result is unsafe
  // for a deterministic account-bound spend, so do not select from it.
  return detailedAvailableCount === availableCount ? credits : null;
};

/**
 * Resolve the approved reset-credit routes from the existing Codex base. An
 * unknown layout fails closed instead of appending a speculative suffix.
 */
export const resolveCodexUsageResetCreditEndpoints = (codexBaseUrl: string): CodexUsageResetCreditEndpoints => {
  let base: URL;
  try {
    base = new URL(nonEmptyOption(codexBaseUrl, "codexBaseUrl"));
  } catch (error) {
    if (error instanceof CodexUsageResetProviderConfigurationError) throw error;
    throw new CodexUsageResetProviderConfigurationError("codexBaseUrl must be an absolute URL.");
  }
  if (base.protocol !== "https:") {
    throw new CodexUsageResetProviderConfigurationError("codexBaseUrl must use HTTPS.");
  }
  if (base.username || base.password || base.search || base.hash) {
    throw new CodexUsageResetProviderConfigurationError(
      "codexBaseUrl must not include credentials, a query string, or a fragment.",
    );
  }
  const pathname = base.pathname.replace(/\/+$/, "") || "/";
  const creditPath = pathname === "/backend-api" || pathname === "/backend-api/codex"
    ? "/backend-api/wham/rate-limit-reset-credits"
    : pathname === "/" || pathname === "/api/codex"
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

/** Discard a non-success response body without treating it as a result. */
const discardResponseBody = (response: Response): void => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void Promise.resolve(cancellation).catch(() => {});
  } catch {
    // Best-effort cleanup never changes the result.
  }
};

const upstreamProviderContract: CodexUsageResetProviderContract = Object.freeze({
  // `redeem_request_id` is caller-supplied and upstream documents same-key
  // replay as idempotent. We intentionally do not auto-replay an ambiguous
  // request because no retention period or lookup endpoint is documented.
  idempotency: Object.freeze({ callerSupplied: true, retentionMs: null }),
  lookup: Object.freeze({ byIdempotencyKey: false, byProviderReceiptId: false }),
  verification: Object.freeze({ independentlyVerifiable: false }),
  // The pinned Codex schema defines `reset` and `already_redeemed` as terminal
  // success codes. Transport loss, non-2xx, malformed JSON, and unknown codes
  // remain ambiguous and are never replayed automatically.
  redeemOutcomeIsFinal: true,
  // The documented response has no provider receipt identifier.
  receiptIdsSafeToPersistAndLog: false,
  supportedResetTypes: Object.freeze(["codex_rate_limits"]) as readonly string[],
});

/**
 * Build the account-bound upstream adapter. It uses the pinned Codex source
 * contract: inventory is `{ credits, available_count }`; consume is
 * `{ redeem_request_id, credit_id? }`; only `reset` and `already_redeemed`
 * are terminal results. A non-2xx, malformed 2xx, or transport loss after
 * consume is ambiguous and never triggers a speculative second consume.
 */
export const createUpstreamCodexUsageResetProvider = (
  options: UpstreamCodexUsageResetProviderOptions,
): CodexUsageResetProvider => {
  const endpoints = resolveCodexUsageResetCreditEndpoints(options.codexBaseUrl);
  const accountId = nonEmptyOption(options.accountId, "accountId");
  const accessToken = nonEmptyOption(options.accessToken, "accessToken");
  const userAgent = nonEmptyOption(options.userAgent, "userAgent");
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
      "user-agent": userAgent,
    });
    if (contentType) result.set("Content-Type", "application/json");
    return result;
  };
  const request = async (url: string, init: RequestInit, signal: AbortSignal): Promise<Response> => {
    if (signal.aborted) throw signal.reason ?? new DOMException("The request was aborted.", "AbortError");
    return await fetcher(url, { ...init, signal, redirect: "manual" });
  };

  return Object.freeze({
    contract: upstreamProviderContract,
    async readInventory(input: ResetAccountContext, signal: AbortSignal): Promise<ResetInventory> {
      assertBoundAccount(input);
      const response = await request(endpoints.inventoryUrl, { method: "GET", headers: headers(false) }, signal);
      if (!isHttpSuccess(response.status)) {
        discardResponseBody(response);
        throw new CodexUsageResetProviderHttpError("inventory", response.status);
      }
      let payload: unknown;
      try {
        payload = await response.json();
      } catch {
        throw new CodexUsageResetProviderConfigurationError("Reset-credit inventory response was not valid JSON.");
      }
      if (!isRecord(payload) || !isNonnegativeSafeInteger(payload.available_count)) {
        throw new CodexUsageResetProviderConfigurationError("Reset-credit inventory response was invalid.");
      }
      const credits = parseDetailedCredits(payload.credits, payload.available_count);
      if (!credits) {
        throw new CodexUsageResetProviderConfigurationError(
          "Reset-credit inventory did not include a complete detailed available-credit list.",
        );
      }
      const observedAtMs = now();
      if (!Number.isSafeInteger(observedAtMs) || observedAtMs < 0) {
        throw new CodexUsageResetProviderConfigurationError("now must return a non-negative safe integer.");
      }
      return {
        availableCount: payload.available_count,
        observedAtMs,
        credits,
      };
    },
    async redeem(input: RedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult> {
      assertBoundAccount(input);
      requireNonBlank(input.idempotencyKey, "idempotencyKey");
      requireNonBlank(input.creditId, "creditId");
      const response = await request(
        endpoints.consumeUrl,
        {
          method: "POST",
          headers: headers(true),
          body: JSON.stringify({
            redeem_request_id: input.idempotencyKey,
            credit_id: input.creditId,
          }),
        },
        signal,
      );
      if (!isHttpSuccess(response.status)) {
        discardResponseBody(response);
        return { kind: "unknown", providerReceiptId: null };
      }
      let payload: unknown;
      try {
        payload = await response.json();
      } catch {
        return { kind: "unknown", providerReceiptId: null };
      }
      const code = isRecord(payload) && typeof payload.code === "string" ? payload.code : null;
      switch (code) {
        case "reset":
          return { kind: "completed", providerReceiptId: "upstream-reset" };
        case "already_redeemed":
          return { kind: "already_redeemed", providerReceiptId: "upstream-already-redeemed" };
        case "nothing_to_reset":
        case "no_credit":
          return { kind: "rejected", reason: code };
        default:
          return { kind: "unknown", providerReceiptId: null };
      }
    },
    lookup: (_input: LookupRedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> =>
      Promise.resolve({ kind: "unknown", providerReceiptId: null }),
    verifyApplied: (_input: ResetAccountContext, _signal: AbortSignal): Promise<boolean> => Promise.resolve(false),
  });
};

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

/** Returns true only for a provider with documented terminal redeem outcomes. */
export const providerTreatsRedeemOutcomeAsFinal = (
  provider: Pick<CodexUsageResetProvider, "contract">,
): boolean => {
  try {
    return provider?.contract?.redeemOutcomeIsFinal === true;
  } catch {
    return false;
  }
};

/**
 * Returns true for a provider with either a replay-safe, independently
 * reconcilable contract or documented terminal redeem outcomes. The latter is
 * deliberately at-most-once: an ambiguous outcome remains durable `unknown`
 * and is never submitted again.
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
    if (contract.redeemOutcomeIsFinal === true) return true;
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
 * Fail-closed fallback provider. It has no network behavior and is used when
 * the account-bound upstream adapter cannot be constructed.
 */
export const unavailableCodexUsageResetProvider: CodexUsageResetProvider = Object.freeze({
  contract: Object.freeze({
    idempotency: Object.freeze({ callerSupplied: false, retentionMs: null }),
    lookup: Object.freeze({ byIdempotencyKey: false, byProviderReceiptId: false }),
    verification: Object.freeze({ independentlyVerifiable: false }),
    redeemOutcomeIsFinal: false,
    receiptIdsSafeToPersistAndLog: false,
    supportedResetTypes: Object.freeze([]) as readonly string[],
  }),
  readInventory: (_input: ResetAccountContext, _signal: AbortSignal): Promise<ResetInventory> => unavailable(),
  redeem: (_input: RedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> => unavailable(),
  lookup: (_input: LookupRedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> => unavailable(),
  verifyApplied: (_input: ResetAccountContext, _signal: AbortSignal): Promise<boolean> => unavailable(),
});
