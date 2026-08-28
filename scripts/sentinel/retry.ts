import { CodexInvocationError } from "./codex.ts";
import {
  assertSentinelRecoveryTransition,
  parseSentinelRecoveryRecord,
  type SentinelRecoveryIdentityV1,
  type SentinelRecoveryPhase,
  type SentinelRecoveryRecordV1,
} from "./recovery.ts";
import { CandidateValidationError } from "./validation.ts";

const SHA256 = /^[0-9a-f]{64}$/u;
const ISO_TIMESTAMP = /^\d{4}-\d{2}-\d{2}T/u;

export const SENTINEL_RETRY_HISTORY_SCHEMA_VERSION = 1 as const;
export const SENTINEL_RETRY_CIRCUIT_THRESHOLD = 3 as const;
export const SENTINEL_VALIDATION_REPAIR_LIMIT = 1 as const;
export const SENTINEL_RETRY_MAX_AUTOMATIC_ATTEMPTS = 8 as const;
export const SENTINEL_RETRY_MAX_HISTORY_ENTRIES = 8 as const;
export const SENTINEL_RETRY_BASE_DELAY_MS = 60_000;
export const SENTINEL_RETRY_MAX_DELAY_MS = 6 * 60 * 60 * 1_000;
export const SENTINEL_RETRY_JITTER_RATIO = 0.2;

export type SentinelFailureClass =
  | "capacity_quota"
  | "transient_transport"
  | "runner_interruption"
  | "invalid_implementation_report"
  | "validation_failure"
  | "review_exhaustion"
  | "git_publication_ambiguity"
  | "stale_source"
  | "unrecoverable_evidence";

const failureClasses = new Set<SentinelFailureClass>([
  "capacity_quota",
  "transient_transport",
  "runner_interruption",
  "invalid_implementation_report",
  "validation_failure",
  "review_exhaustion",
  "git_publication_ambiguity",
  "stale_source",
  "unrecoverable_evidence",
]);

const sourceKinds = new Set<SentinelRecoveryIdentityV1["source_kind"]>([
  "github_issue",
  "review_backlog",
  "triage",
  "incident",
]);

const nonEmpty = (value: unknown): value is string => typeof value === "string" && value.trim().length > 0;

const validTimestamp = (value: unknown): value is string =>
  nonEmpty(value) && ISO_TIMESTAMP.test(value) && Number.isFinite(Date.parse(value));

const validIdentity = (value: unknown): value is SentinelRecoveryIdentityV1 => {
  if (
    value === null || typeof value !== "object" || Array.isArray(value) ||
    !nonEmpty((value as Record<string, unknown>).repository) ||
    !sourceKinds.has((value as Record<string, unknown>).source_kind as SentinelRecoveryIdentityV1["source_kind"]) ||
    !nonEmpty((value as Record<string, unknown>).source_id) ||
    !nonEmpty((value as Record<string, unknown>).source_revision) ||
    !Number.isSafeInteger((value as Record<string, unknown>).candidate_generation) ||
    ((value as Record<string, unknown>).candidate_generation as number) <= 0
  ) return false;
  return true;
};

const parseIdentity = (value: unknown, label = "Sentinel retry identity"): SentinelRecoveryIdentityV1 => {
  if (!validIdentity(value)) throw new Error(`${label} is invalid`);
  return value;
};

const parseFailureClass = (value: unknown): SentinelFailureClass => {
  if (typeof value !== "string" || !failureClasses.has(value as SentinelFailureClass)) {
    throw new Error("Sentinel retry failure class is invalid");
  }
  return value as SentinelFailureClass;
};

/** Normalize only the stable portion of a provider or validation signature. */
export const normalizeSentinelFailureSignature = (value: string): string => {
  if (typeof value !== "string") throw new TypeError("Sentinel failure signature must be a string");
  return value.normalize("NFKC").replace(/\r\n?/gu, "\n").split("\n").map((line) => line.trim()).join("\n")
    .replace(/\b\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z\b/gu, "<timestamp>")
    .replace(/\b[0-9a-f]{40,64}\b/giu, "<sha>")
    .replace(/\b\d{8,}\b/gu, "<number>")
    .replace(/[ \t]+/gu, " ").trim().slice(0, 4_096);
};

const stableValue = (value: unknown): unknown => {
  if (Array.isArray(value)) return value.map(stableValue);
  if (value !== null && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>).sort(([left], [right]) => left.localeCompare(right)).map(
        ([key, nested]) => [key, stableValue(nested)],
      ),
    );
  }
  return value;
};

const sha256Hex = async (value: string): Promise<string> => {
  const bytes = new TextEncoder().encode(value);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

export type SentinelFailureFingerprintInput = Readonly<{
  identity: SentinelRecoveryIdentityV1;
  failure_class: SentinelFailureClass;
  code?: string | null;
  phase?: string | null;
  signature?: string | null;
}>;

/**
 * Produce a stable digest for one failure. Run IDs, attempts, timestamps,
 * branches, and candidate SHAs are intentionally absent so retries of the
 * same immutable work item collapse to one fingerprint.
 */
export const stableSentinelFailureFingerprint = async (
  input: SentinelFailureFingerprintInput,
): Promise<string> => {
  const identity = parseIdentity(input.identity);
  const failureClass = parseFailureClass(input.failure_class);
  const canonical = stableValue({
    repository: identity.repository,
    source_kind: identity.source_kind,
    source_id: identity.source_id,
    source_revision: identity.source_revision,
    candidate_generation: identity.candidate_generation,
    failure_class: failureClass,
    code: input.code === null || input.code === undefined ? null : normalizeSentinelFailureSignature(input.code),
    phase: input.phase === null || input.phase === undefined ? null : normalizeSentinelFailureSignature(input.phase),
    signature: input.signature === null || input.signature === undefined
      ? null
      : normalizeSentinelFailureSignature(input.signature),
  });
  return await sha256Hex(JSON.stringify(canonical));
};

export const createSentinelFailureFingerprint = stableSentinelFailureFingerprint;
export const fingerprintSentinelFailure = stableSentinelFailureFingerprint;

export type SentinelFailureClassification = Readonly<{
  failure_class: SentinelFailureClass;
  code: string;
  phase: string | null;
  signature: string;
  retryable: boolean;
  validation_repairable: boolean;
}>;

const errorMessage = (error: unknown): string => {
  if (error instanceof Error) return error.message;
  if (typeof error === "string") return error;
  return "Unknown Sentinel failure";
};

const structuralFailureCode = (error: unknown): string | null => {
  if (error !== null && typeof error === "object" && !Array.isArray(error)) {
    const value = (error as Record<string, unknown>).failure;
    if (typeof value === "string" && value.trim().length > 0) return value;
  }
  return null;
};

export type SentinelFailureClassificationOptions = Readonly<{
  phase?: string | null;
  code?: string | null;
  signature?: string | null;
}>;

/** Classify known Sentinel failures without treating a generic error as quota exhaustion. */
export const classifySentinelFailure = (
  error: unknown,
  options: SentinelFailureClassificationOptions = {},
): SentinelFailureClassification => {
  const message = errorMessage(error);
  const normalizedMessage = normalizeSentinelFailureSignature(message);
  let failureClass: SentinelFailureClass = "unrecoverable_evidence";
  let retryable = false;
  let validationRepairable = false;
  let code = options.code ?? structuralFailureCode(error) ?? "unknown_failure";

  if (error instanceof CandidateValidationError) {
    failureClass = "validation_failure";
    retryable = true;
    validationRepairable = true;
    code = error.failure.phase;
  } else if (error instanceof CodexInvocationError) {
    code = error.failure;
    if (error.failure === "accounts_unavailable") {
      failureClass = "capacity_quota";
      retryable = true;
    } else if (
      error.failure === "invocation_timeout" || error.failure === "command_failed" ||
      error.failure === "runtime_failure"
    ) {
      failureClass = "transient_transport";
      retryable = true;
    } else if (
      error.failure === "last_message_missing" || error.failure === "native_review_missing" ||
      error.failure === "output_limit_exceeded"
    ) {
      failureClass = "invalid_implementation_report";
      retryable = true;
    }
  } else if (
    error !== null && typeof error === "object" && !Array.isArray(error) &&
    ((error as Record<string, unknown>).name === "AbortError" ||
      (error as Record<string, unknown>).name === "InterruptedError")
  ) {
    failureClass = "runner_interruption";
    retryable = true;
    code = code === "unknown_failure" ? "runner_interrupted" : code;
  } else if (/review\s+(?:round\s+)?exhaust|blocking findings after round/iu.test(normalizedMessage)) {
    failureClass = "review_exhaustion";
  } else if (/stale|source.+(?:changed|advanced)|development.+advanced|snapshot.+changed/iu.test(normalizedMessage)) {
    failureClass = "stale_source";
    retryable = true;
  } else if (/git|push|publication|remote ref|atomic.+(?:ambiguous|unknown)/iu.test(normalizedMessage)) {
    failureClass = "git_publication_ambiguity";
    retryable = true;
  } else if (/validation\s+failed|candidate validation/iu.test(normalizedMessage)) {
    failureClass = "validation_failure";
    retryable = true;
    validationRepairable = true;
  } else if (/runner|workflow.+(?:cancel|interrupt|lost)/iu.test(normalizedMessage)) {
    failureClass = "runner_interruption";
    retryable = true;
  } else if (/timeout|temporar(?:y|ily)|transport|network|connection/iu.test(normalizedMessage)) {
    failureClass = "transient_transport";
    retryable = true;
  } else if (/quota|capacity|rate.?limit|accounts?.+unavailable/iu.test(normalizedMessage)) {
    failureClass = "capacity_quota";
    retryable = true;
  }

  const signature = options.signature === null || options.signature === undefined
    ? error instanceof CandidateValidationError
      ? [
        error.failure.phase,
        error.failure.command.join(" "),
        error.failure.exit_code,
        error.failure.stdout_sha256,
        error.failure.stderr_sha256,
      ].join("|")
      : error instanceof CodexInvocationError
      ? error.failure
      : normalizedMessage
    : options.signature;
  return Object.freeze({
    failure_class: failureClass,
    code: normalizeSentinelFailureSignature(code),
    phase: options.phase ?? (error instanceof CandidateValidationError ? error.failure.phase : null),
    signature: normalizeSentinelFailureSignature(signature),
    retryable,
    validation_repairable: validationRepairable,
  });
};

export type SentinelRetryAttemptV1 = Readonly<{
  schema_version: typeof SENTINEL_RETRY_HISTORY_SCHEMA_VERSION;
  identity: SentinelRecoveryIdentityV1;
  attempt: number;
  failure_class: SentinelFailureClass;
  failure_fingerprint: string;
  observed_at: string;
}>;

export type SentinelRetryAttemptHistory = readonly SentinelRetryAttemptV1[];

export const parseSentinelRetryAttempt = (value: unknown): SentinelRetryAttemptV1 => {
  if (
    value === null || typeof value !== "object" || Array.isArray(value) ||
    (value as Record<string, unknown>).schema_version !== SENTINEL_RETRY_HISTORY_SCHEMA_VERSION ||
    !validIdentity((value as Record<string, unknown>).identity) ||
    !Number.isSafeInteger((value as Record<string, unknown>).attempt) ||
    ((value as Record<string, unknown>).attempt as number) <= 0 ||
    !failureClasses.has((value as Record<string, unknown>).failure_class as SentinelFailureClass) ||
    typeof (value as Record<string, unknown>).failure_fingerprint !== "string" ||
    !SHA256.test((value as Record<string, unknown>).failure_fingerprint as string) ||
    !validTimestamp((value as Record<string, unknown>).observed_at)
  ) throw new Error("Sentinel retry attempt is invalid");
  return value as SentinelRetryAttemptV1;
};

export const parseSentinelRetryHistory = (value: unknown): SentinelRetryAttemptHistory => {
  if (!Array.isArray(value) || value.length > SENTINEL_RETRY_MAX_HISTORY_ENTRIES) {
    throw new Error("Sentinel retry history exceeds its bound");
  }
  return Object.freeze(value.map(parseSentinelRetryAttempt));
};

const identityKey = (identity: SentinelRecoveryIdentityV1): string =>
  JSON.stringify([
    identity.repository,
    identity.source_kind,
    identity.source_id,
    identity.source_revision,
    identity.candidate_generation,
  ]);

const attemptKey = (attempt: SentinelRetryAttemptV1): string =>
  `${identityKey(attempt.identity)}|${attempt.attempt}|${attempt.failure_fingerprint}`;

export const createSentinelRetryAttempt = (
  input: Readonly<{
    identity: SentinelRecoveryIdentityV1;
    attempt: number;
    failure_class: SentinelFailureClass;
    failure_fingerprint: string;
    observed_at: string;
  }>,
): SentinelRetryAttemptV1 => {
  const attempt: SentinelRetryAttemptV1 = {
    schema_version: SENTINEL_RETRY_HISTORY_SCHEMA_VERSION,
    identity: parseIdentity(input.identity),
    attempt: input.attempt,
    failure_class: parseFailureClass(input.failure_class),
    failure_fingerprint: input.failure_fingerprint,
    observed_at: input.observed_at,
  };
  return parseSentinelRetryAttempt(attempt);
};

export const appendSentinelRetryAttempt = (
  history: SentinelRetryAttemptHistory,
  attempt: SentinelRetryAttemptV1,
): SentinelRetryAttemptHistory => {
  const parsedHistory = parseSentinelRetryHistory(history);
  const parsedAttempt = parseSentinelRetryAttempt(attempt);
  if (parsedHistory.some((item) => attemptKey(item) === attemptKey(parsedAttempt))) return parsedHistory;
  return Object.freeze([...parsedHistory, parsedAttempt].slice(-SENTINEL_RETRY_MAX_HISTORY_ENTRIES));
};

export const retryHistoryForIdentity = (
  history: SentinelRetryAttemptHistory,
  identity: SentinelRecoveryIdentityV1,
): SentinelRetryAttemptHistory => {
  const key = identityKey(parseIdentity(identity));
  return Object.freeze(parseSentinelRetryHistory(history).filter((attempt) => identityKey(attempt.identity) === key));
};

export const retryHistoryForSourceRevision = retryHistoryForIdentity;

export const sourceRevisionChanged = (
  identity: SentinelRecoveryIdentityV1,
  sourceRevision: string,
): boolean => {
  parseIdentity(identity);
  if (!nonEmpty(sourceRevision)) throw new Error("Sentinel source revision is required");
  return identity.source_revision !== sourceRevision;
};

export const freshSentinelCandidateIdentity = (
  identity: SentinelRecoveryIdentityV1,
  sourceRevision: string,
): SentinelRecoveryIdentityV1 => {
  const current = parseIdentity(identity);
  if (!nonEmpty(sourceRevision)) throw new Error("Sentinel source revision is required");
  if (sourceRevision === current.source_revision) return current;
  if (current.candidate_generation >= Number.MAX_SAFE_INTEGER) {
    throw new Error("Sentinel candidate generation exhausted its safe integer bound");
  }
  return {
    ...current,
    source_revision: sourceRevision,
    candidate_generation: current.candidate_generation + 1,
  };
};

export const computeSentinelRetryBackoffMs = (
  input: Readonly<{
    attempt: number;
    base_delay_ms?: number;
    max_delay_ms?: number;
    jitter_ratio?: number;
    random?: () => number;
  }>,
): number => {
  if (!Number.isSafeInteger(input.attempt) || input.attempt <= 0) {
    throw new TypeError("Sentinel retry attempt must be a positive integer");
  }
  const base = input.base_delay_ms ?? SENTINEL_RETRY_BASE_DELAY_MS;
  const maximum = input.max_delay_ms ?? SENTINEL_RETRY_MAX_DELAY_MS;
  const jitterRatio = input.jitter_ratio ?? SENTINEL_RETRY_JITTER_RATIO;
  if (!Number.isSafeInteger(base) || base <= 0 || !Number.isSafeInteger(maximum) || maximum < base) {
    throw new TypeError("Sentinel retry delay bounds are invalid");
  }
  if (!Number.isFinite(jitterRatio) || jitterRatio < 0 || jitterRatio > 1) {
    throw new TypeError("Sentinel retry jitter ratio is invalid");
  }
  const random = input.random ?? Math.random;
  const sample = random();
  if (!Number.isFinite(sample) || sample < 0 || sample >= 1) {
    throw new TypeError("Sentinel retry jitter source must return a value in [0, 1)");
  }
  const exponential = Math.min(maximum, base * Math.pow(2, input.attempt - 1));
  const jittered = exponential * (1 + ((sample * 2) - 1) * jitterRatio);
  return Math.min(maximum, Math.max(0, Math.round(jittered)));
};

export const boundedExponentialBackoffWithJitter = computeSentinelRetryBackoffMs;

export const canStartSentinelValidationRepair = (
  history: SentinelRetryAttemptHistory,
  identity: SentinelRecoveryIdentityV1,
): boolean =>
  retryHistoryForIdentity(history, identity).filter((attempt) => attempt.failure_class === "validation_failure")
    .length < SENTINEL_VALIDATION_REPAIR_LIMIT;

export const shouldAttemptSentinelValidationRepair = canStartSentinelValidationRepair;

export const isSentinelCircuitBreakerOpen = (
  history: SentinelRetryAttemptHistory,
  identity: SentinelRecoveryIdentityV1,
  failureFingerprint: string,
): boolean => {
  if (!SHA256.test(failureFingerprint)) throw new Error("Sentinel failure fingerprint is invalid");
  return retryHistoryForIdentity(history, identity).filter((attempt) =>
    attempt.failure_fingerprint === failureFingerprint
  )
    .length >= SENTINEL_RETRY_CIRCUIT_THRESHOLD;
};

export const sentinelCircuitBreakerOpen = isSentinelCircuitBreakerOpen;

export type SentinelRetryFailureInput = Readonly<{
  failure_class: SentinelFailureClass;
  failure_fingerprint?: string | null;
  code?: string | null;
  phase?: string | null;
  signature?: string | null;
  source_revision?: string;
}>;

export type SentinelRetryDecision = Readonly<{
  disposition: "retry_wait" | "manual_required" | "fresh_generation";
  should_retry: boolean;
  circuit_open: boolean;
  validation_repair_allowed: boolean;
  source_revision_changed: boolean;
  candidate_generation: number;
  attempt_count: number;
  identical_failure_count: number;
  backoff_ms: number | null;
  retry_at: string | null;
  failure_class: SentinelFailureClass;
  failure_fingerprint: string;
  reason: string;
  next_action: string | null;
}>;

export type SentinelRetryPolicyInput = Readonly<{
  identity: SentinelRecoveryIdentityV1;
  failure: SentinelRetryFailureInput;
  history: SentinelRetryAttemptHistory;
  now?: string;
  random?: () => number;
  base_delay_ms?: number;
  max_delay_ms?: number;
  jitter_ratio?: number;
}>;

const retryNow = (value: string | undefined): string => {
  const now = value ?? new Date().toISOString();
  if (!validTimestamp(now)) throw new Error("Sentinel retry timestamp is invalid");
  return now;
};

const manualReason = (
  input: Readonly<{
    circuitOpen: boolean;
    validationRepairAllowed: boolean;
    validationFailureCount: number;
    attemptCount: number;
    failureClass: SentinelFailureClass;
  }>,
): string => {
  if (input.circuitOpen) return "Three identical Sentinel failure fingerprints opened the circuit breaker.";
  if (input.failureClass === "validation_failure" && !input.validationRepairAllowed) {
    return "The one validation repair for this candidate generation was exhausted.";
  }
  if (input.attemptCount > SENTINEL_RETRY_MAX_AUTOMATIC_ATTEMPTS) {
    return "The bounded Sentinel retry attempt limit was exhausted.";
  }
  if (input.failureClass === "review_exhaustion") return "Native review exhaustion requires owner review.";
  if (input.failureClass === "unrecoverable_evidence") return "The failure evidence cannot be recovered automatically.";
  return "The Sentinel failure is not eligible for automatic retry.";
};

export const evaluateSentinelRetryPolicy = async (
  input: SentinelRetryPolicyInput,
): Promise<Readonly<{ decision: SentinelRetryDecision; history: SentinelRetryAttemptHistory }>> => {
  const identity = parseIdentity(input.identity);
  const observedSourceRevision = input.failure.source_revision ?? identity.source_revision;
  if (!nonEmpty(observedSourceRevision)) throw new Error("Sentinel source revision is required");
  const effectiveIdentity = freshSentinelCandidateIdentity(identity, observedSourceRevision);
  const now = retryNow(input.now);
  const parsedHistory = parseSentinelRetryHistory(input.history);
  const failureClass = parseFailureClass(input.failure.failure_class);
  const fingerprint = input.failure.failure_fingerprint === null || input.failure.failure_fingerprint === undefined
    ? await stableSentinelFailureFingerprint({
      identity: effectiveIdentity,
      failure_class: failureClass,
      code: input.failure.code,
      phase: input.failure.phase,
      signature: input.failure.signature,
    })
    : input.failure.failure_fingerprint;
  if (!SHA256.test(fingerprint)) throw new Error("Sentinel failure fingerprint is invalid");
  const sourceChanged = observedSourceRevision !== identity.source_revision;
  const prior = retryHistoryForIdentity(parsedHistory, effectiveIdentity);
  const attempt = Math.max(1, ...prior.map((entry) => entry.attempt + 1));
  const retryAttempt = createSentinelRetryAttempt({
    identity: effectiveIdentity,
    attempt,
    failure_class: failureClass,
    failure_fingerprint: fingerprint,
    observed_at: now,
  });
  const history = appendSentinelRetryAttempt(parsedHistory, retryAttempt);
  const current = retryHistoryForIdentity(history, effectiveIdentity);
  // History is intentionally capped, so use the highest durable attempt
  // number for the total-attempt bound instead of the retained row count.
  const attemptCount = Math.max(1, ...current.map((entry) => entry.attempt));
  const identicalFailureCount = current.filter((entry) => entry.failure_fingerprint === fingerprint).length;
  const validationFailureCount = prior.filter((entry) => entry.failure_class === "validation_failure").length;
  const validationRepairAllowed = failureClass === "validation_failure" &&
    validationFailureCount < SENTINEL_VALIDATION_REPAIR_LIMIT;
  if (sourceChanged) {
    return {
      history,
      decision: Object.freeze({
        disposition: "fresh_generation",
        should_retry: true,
        circuit_open: false,
        validation_repair_allowed: false,
        source_revision_changed: true,
        candidate_generation: effectiveIdentity.candidate_generation,
        attempt_count: 1,
        identical_failure_count: 1,
        backoff_ms: null,
        retry_at: null,
        failure_class: failureClass,
        failure_fingerprint: fingerprint,
        reason: "The source revision changed; start a fresh candidate generation.",
        next_action: "Claim the changed source revision with a new candidate generation.",
      }),
    };
  }
  const circuitOpen = identicalFailureCount >= SENTINEL_RETRY_CIRCUIT_THRESHOLD;
  const attemptLimit = attemptCount > SENTINEL_RETRY_MAX_AUTOMATIC_ATTEMPTS;
  const manual = circuitOpen || attemptLimit || (failureClass === "validation_failure" && !validationRepairAllowed) ||
    failureClass === "review_exhaustion" || failureClass === "unrecoverable_evidence";
  if (manual) {
    return {
      history,
      decision: Object.freeze({
        disposition: "manual_required",
        should_retry: false,
        circuit_open: circuitOpen,
        validation_repair_allowed: false,
        source_revision_changed: false,
        candidate_generation: identity.candidate_generation,
        attempt_count: attemptCount,
        identical_failure_count: identicalFailureCount,
        backoff_ms: null,
        retry_at: null,
        failure_class: failureClass,
        failure_fingerprint: fingerprint,
        reason: manualReason({
          circuitOpen,
          validationRepairAllowed,
          validationFailureCount,
          attemptCount,
          failureClass,
        }),
        next_action: "Owner review is required before another Sentinel attempt.",
      }),
    };
  }
  const backoffMs = computeSentinelRetryBackoffMs({
    attempt: attemptCount,
    base_delay_ms: input.base_delay_ms,
    max_delay_ms: input.max_delay_ms,
    jitter_ratio: input.jitter_ratio,
    random: input.random,
  });
  const retryAt = new Date(Date.parse(now) + backoffMs).toISOString();
  return {
    history,
    decision: Object.freeze({
      disposition: "retry_wait",
      should_retry: true,
      circuit_open: false,
      validation_repair_allowed: validationRepairAllowed,
      source_revision_changed: false,
      candidate_generation: identity.candidate_generation,
      attempt_count: attemptCount,
      identical_failure_count: identicalFailureCount,
      backoff_ms: backoffMs,
      retry_at: retryAt,
      failure_class: failureClass,
      failure_fingerprint: fingerprint,
      reason: validationRepairAllowed
        ? "Validation failed; one repair attempt remains on the same candidate generation."
        : "The bounded Sentinel retry policy scheduled another attempt.",
      next_action: `Retry after ${retryAt}.`,
    }),
  };
};

export const decideSentinelRetry = evaluateSentinelRetryPolicy;

const retryPhaseFor = (phase: SentinelRecoveryPhase): SentinelRecoveryPhase => {
  if (phase === "claimed" || phase === "workspace_dirty" || phase === "checkpoint_publishing") {
    return "recovery_pending";
  }
  return "retry_wait";
};

export type SentinelRetryRecoveryResult = Readonly<{
  before: SentinelRecoveryRecordV1;
  after: SentinelRecoveryRecordV1;
  decision: SentinelRetryDecision;
  history: SentinelRetryAttemptHistory;
  transitioned: boolean;
  next_identity: SentinelRecoveryIdentityV1 | null;
}>;

/** Apply retry policy to a durable checkpoint without changing its identity or clearing its candidate. */
export const applySentinelRetryPolicyToRecovery = async (
  input: Readonly<{
    record: unknown;
    failure: SentinelRetryFailureInput;
    history: SentinelRetryAttemptHistory;
    now?: string;
    random?: () => number;
    base_delay_ms?: number;
    max_delay_ms?: number;
    jitter_ratio?: number;
  }>,
): Promise<SentinelRetryRecoveryResult> => {
  const before = parseSentinelRecoveryRecord(input.record);
  const evaluated = await evaluateSentinelRetryPolicy({
    identity: before.identity,
    failure: input.failure,
    history: input.history,
    now: input.now,
    random: input.random,
    base_delay_ms: input.base_delay_ms,
    max_delay_ms: input.max_delay_ms,
    jitter_ratio: input.jitter_ratio,
  });
  if (evaluated.decision.source_revision_changed) {
    return {
      before,
      after: before,
      decision: evaluated.decision,
      history: evaluated.history,
      transitioned: false,
      next_identity: freshSentinelCandidateIdentity(before.identity, input.failure.source_revision!),
    };
  }
  if (before.phase === "manual_required" || before.phase === "rejected" || before.phase === "delivered") {
    return {
      before,
      after: before,
      decision: evaluated.decision,
      history: evaluated.history,
      transitioned: false,
      next_identity: null,
    };
  }
  const targetPhase = evaluated.decision.disposition === "manual_required"
    ? "manual_required"
    : retryPhaseFor(before.phase);
  if (targetPhase === before.phase) {
    return {
      before,
      after: before,
      decision: evaluated.decision,
      history: evaluated.history,
      transitioned: false,
      next_identity: null,
    };
  }
  if (before.state_version >= Number.MAX_SAFE_INTEGER) {
    throw new Error("Sentinel recovery state version exhausted its safe integer bound");
  }
  const now = retryNow(input.now);
  const next: SentinelRecoveryRecordV1 = {
    ...before,
    phase: targetPhase,
    disposition: targetPhase === "manual_required" ? "manual_required" : "active",
    state_version: before.state_version + 1,
    updated_at: now,
    failure_class: evaluated.decision.failure_class,
    failure_fingerprint: evaluated.decision.failure_fingerprint,
    reason: evaluated.decision.reason,
    next_action: evaluated.decision.next_action,
  };
  assertSentinelRecoveryTransition(before, next);
  return {
    before,
    after: parseSentinelRecoveryRecord(next),
    decision: evaluated.decision,
    history: evaluated.history,
    transitioned: true,
    next_identity: null,
  };
};

export const applySentinelRetryPolicy = applySentinelRetryPolicyToRecovery;

export const createFreshSentinelRecoveryRecord = (
  input: Readonly<{
    record: unknown;
    source_revision: string;
    run_id: string;
    attempt?: number;
    lease_token: string;
    now: string;
    base_sha?: string;
  }>,
): SentinelRecoveryRecordV1 => {
  const previous = parseSentinelRecoveryRecord(input.record);
  const identity = freshSentinelCandidateIdentity(previous.identity, input.source_revision);
  if (identity === previous.identity) {
    throw new Error("A fresh Sentinel recovery generation requires a changed source revision");
  }
  if (!nonEmpty(input.run_id) || !nonEmpty(input.lease_token) || !validTimestamp(input.now)) {
    throw new Error("Fresh Sentinel recovery generation metadata is invalid");
  }
  if (previous.state_version >= Number.MAX_SAFE_INTEGER) {
    throw new Error("Sentinel recovery state version exhausted its safe integer bound");
  }
  const record: SentinelRecoveryRecordV1 = {
    schema_version: 1,
    identity,
    run_id: input.run_id,
    attempt: input.attempt ?? 1,
    lease_token: input.lease_token,
    base_sha: input.base_sha ?? previous.base_sha,
    phase: "claimed",
    disposition: "active",
    state_version: previous.state_version + 1,
    created_at: input.now,
    updated_at: input.now,
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: [],
    artifact_digests: [],
    reason: "The source revision changed; a fresh candidate generation is required.",
    next_action: "Start implementation for the changed source revision.",
    predecessor: identityKey(previous.identity),
  };
  return parseSentinelRecoveryRecord(record);
};

export const freshSentinelRecoveryRecord = createFreshSentinelRecoveryRecord;
