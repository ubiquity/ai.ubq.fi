/**
 * Boundary for an externally visible Codex usage-reset redemption.
 *
 * The shipped provider is intentionally non-networking. A production adapter
 * may be added only after its documented contract proves caller-supplied
 * idempotency retention, lookup by that key, and independent verification.
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
 * network access. The default provider remains non-networking until an exact
 * contract proves at-most-once reconciliation.
 */
export interface CodexUsageResetProvider {
  readonly contract: CodexUsageResetProviderContract;
  readInventory(input: ResetAccountContext, signal: AbortSignal): Promise<ResetInventory>;
  redeem(input: RedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult>;
  lookup(input: LookupRedeemResetInput, signal: AbortSignal): Promise<RedeemResetResult>;
  verifyApplied(input: ResetAccountContext, signal: AbortSignal): Promise<boolean>;
}

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

/**
 * Returns true only when the provider has an explicitly documented,
 * reconcilable contract. A transport status alone is never proof that a reset
 * applied, so it cannot satisfy this gate.
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
    return supportsResetType && contract.idempotency.callerSupplied === true &&
      isPositiveSafeInteger(contract.idempotency.retentionMs) &&
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
 * Default production provider. It has no network behavior and fails before
 * any provider interaction. Keeping it as the default prevents development
 * and automated tests from consuming a real reset.
 */
export const unavailableCodexUsageResetProvider: CodexUsageResetProvider = Object.freeze({
  contract: Object.freeze({
    idempotency: Object.freeze({ callerSupplied: false, retentionMs: null }),
    lookup: Object.freeze({ byIdempotencyKey: false, byProviderReceiptId: false }),
    verification: Object.freeze({ independentlyVerifiable: false }),
    receiptIdsSafeToPersistAndLog: false,
    supportedResetTypes: Object.freeze([]) as readonly string[],
  }),
  readInventory: (_input: ResetAccountContext, _signal: AbortSignal): Promise<ResetInventory> => unavailable(),
  redeem: (_input: RedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> => unavailable(),
  lookup: (_input: LookupRedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> => unavailable(),
  verifyApplied: (_input: ResetAccountContext, _signal: AbortSignal): Promise<boolean> => unavailable(),
});
