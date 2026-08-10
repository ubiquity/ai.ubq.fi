/**
 * Offline Stage 0 prompt-cache telemetry analyzer.
 *
 * This script deliberately accepts only stdin. It never reads a log file,
 * connects to a service, or persists the input. Its JSON output contains only
 * release identity and aggregate provider/opaque-model-cohort/route telemetry;
 * it never echoes an input line or request-level identifiers.
 */

const TERMINAL_MARKER = "[ai.ubq.fi] request_terminal";
const TERMINAL_LINE_PREFIX = `${TERMINAL_MARKER} `;
const INFO_TERMINAL_LINE_PREFIX = `INFO ${TERMINAL_LINE_PREFIX}`;

export const STAGE0_AGGREGATE_MIN_COMPLETED = 10_000;
export const STAGE0_COHORT_MIN_COMPLETED = 1_000;
export const STAGE0_MIN_REPORTED_COVERAGE = 0.995;

const MAX_REQUEST_ID_CHARS = 128;
const MAX_RELEASE_IDENTIFIER_CHARS = 128;
// The completed-evidence gate needs 10k events; cap retained IDs so hostile
// stdin cannot turn duplicate protection into unbounded memory retention.
const MAX_RETAINED_REQUEST_IDS = 100_000;
const SAFE_IDENTIFIER_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._:-]*$/;
const GIT_SHA_PATTERN = /^[0-9a-f]{7,64}$/i;
const USAGE_TELEMETRY_STATUSES = new Set(["missing", "partial", "reported", "invalid"] as const);
// A Responses or Chat inference terminal is emitted only by these in-process
// transports. Keeping this vocabulary closed means a forged or newly added
// provider cannot silently become a Stage 0 or outcome cohort.
const INFERENCE_PROVIDERS = new Set(["chatgpt_codex", "yunwu"] as const);
const PROMPT_CACHE_MODE_VALUES = ["implicit", "explicit", "legacy_retention", "unspecified"] as const;
const PROMPT_CACHE_MODES = new Set(PROMPT_CACHE_MODE_VALUES);
const AFFINITY_OUTCOME_VALUES = ["none", "preferred", "failover", "shadow_only"] as const;
const AFFINITY_OUTCOMES = new Set(AFFINITY_OUTCOME_VALUES);
const INFERENCE_TERMINAL_OUTCOMES = ["completed", "failed", "incomplete", "cancelled"] as const;
const MAX_MODEL_LABEL_CHARS = 128;
const TERMINAL_ROUTES = new Set(
  [
    "responses",
    "chat.completions",
    "embeddings",
    "embeddings.jobs.create",
    "embeddings.jobs.get",
  ] as const,
);
const STREAM_TERMINAL_TYPES = new Set(
  [
    "response.completed",
    "response.failed",
    "response.incomplete",
    "error",
    "eof",
    "cancelled",
    "deadline",
  ] as const,
);

type UsageTelemetryStatus = "missing" | "partial" | "reported" | "invalid";
type TerminalRoute = "responses" | "chat.completions" | "embeddings" | "embeddings.jobs.create" | "embeddings.jobs.get";
type InferenceRoute = "responses" | "chat.completions";
type InferenceTerminalOutcome = typeof INFERENCE_TERMINAL_OUTCOMES[number];
type PromptCacheMode = "implicit" | "explicit" | "legacy_retention" | "unspecified";
type AffinityOutcome = "none" | "preferred" | "failover" | "shadow_only";
type StreamTerminalType =
  | "response.completed"
  | "response.failed"
  | "response.incomplete"
  | "error"
  | "eof"
  | "cancelled"
  | "deadline";

type ReleaseIdentity = Readonly<{
  git_sha: string;
  deno_revision: string;
  router_revision: string | null;
}>;

/**
 * A report keeps the immutable SHA visible, but never echoes arbitrary
 * deployment or upstream router revision values from its input stream.
 */
type ReleaseReportIdentity = Readonly<{
  git_sha: string;
  deno_revision: "validated";
  router_revision: "validated" | null;
}>;

type TerminalEvent = Readonly<{
  request_id: string;
  route: TerminalRoute;
  status: number;
  stream_terminal_type: StreamTerminalType | null;
  input_tokens: number | null;
  cached_input_tokens: number | null;
  cache_write_input_tokens: number | null;
  usage_observed: boolean;
  usage_telemetry_status: UsageTelemetryStatus;
  prompt_cache_key_present: boolean;
  prompt_cache_mode: PromptCacheMode;
  account_slot: number | null;
  affinity_outcome: AffinityOutcome;
  stream: boolean | null;
  release: ReleaseIdentity;
  inference_outcome: InferenceTerminalOutcome | null;
  provider: string | null;
  model: string | null;
}>;

export type CacheTokenSummary = Readonly<{
  sum_tokens: number;
  observed_events: number;
  null_events: number;
  zero_events: number;
  positive_events: number;
}>;

/**
 * Counter-sum cache ratios calculated only from terminal events whose usage
 * parser classified the complete cache-read tuple as reported. Cache writes
 * are an independent provider measurement, so the payback numerator includes
 * cache reads from the same events and never subtracts writes from input.
 */
export type ValidReportedCacheMetrics = Readonly<{
  reported_events: number;
  cache_read: Readonly<{
    aggregate_input_tokens: number;
    aggregate_cached_input_tokens: number;
    ratio: number | null;
  }>;
  cache_write_payback: Readonly<{
    observed_events: number;
    aggregate_cached_input_tokens: number;
    aggregate_cache_write_input_tokens: number;
    ratio: number | null;
  }>;
}>;

export type Stage0CohortReport = Readonly<{
  provider: string;
  /** Opaque, per-report cohort label; never the logged model value. */
  model: string;
  route: string;
  completed_inference: number;
  status_totals: Readonly<Record<string, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
  valid_reported_cache_metrics: ValidReportedCacheMetrics;
  observed_completed_cache_read_input_tokens: CacheTokenSummary;
  observed_completed_cache_write_input_tokens: CacheTokenSummary;
  reported_over_completed: Readonly<{
    reported: number;
    completed: number;
    ratio: number | null;
  }>;
  completed_1k_gate: Readonly<{
    minimum_completed: number;
    observed_completed: number;
    passed: boolean;
  }>;
  reported_coverage_99_5_gate: Readonly<{
    minimum_ratio: number;
    reported: number;
    completed: number;
    ratio: number | null;
    passed: boolean;
  }>;
}>;

/**
 * Aggregate-only observation for all terminal Responses and Chat outcomes.
 * It deliberately has no eligibility gate: Stage 0 remains completed-only.
 */
export type InferenceOutcomeCohortReport = Readonly<{
  provider: string;
  /** Opaque, per-report cohort label; never the logged model value. */
  model: string;
  route: InferenceRoute;
  stream: boolean | null;
  outcome: InferenceTerminalOutcome;
  stream_terminal_type: StreamTerminalType;
  terminal_events: number;
  terminal_without_usage: number;
  status_totals: Readonly<Record<string, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
  valid_reported_cache_metrics: ValidReportedCacheMetrics;
  observed_cache_read_input_tokens: CacheTokenSummary;
  observed_cache_write_input_tokens: CacheTokenSummary;
}>;

export type InferenceTerminalOutcomesReport = Readonly<{
  terminal_events: number;
  terminal_without_usage: number;
  outcome_totals: Readonly<Record<InferenceTerminalOutcome, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
  prompt_cache_key_presence: Readonly<{
    present: number;
    absent: number;
  }>;
  prompt_cache_mode_totals: Readonly<Record<PromptCacheMode, number>>;
  account_slot_summary: Readonly<{
    assigned_terminal_events: number;
    unassigned_terminal_events: number;
    distinct_assigned_slots: number;
  }>;
  affinity_outcome_totals: Readonly<Record<AffinityOutcome, number>>;
  cohorts: readonly InferenceOutcomeCohortReport[];
}>;

export type Stage0CacheTelemetryReport = Readonly<{
  version: 1;
  release: ReleaseReportIdentity;
  terminal_events: number;
  status_totals: Readonly<Record<string, number>>;
  completed_inference: number;
  completed_status_totals: Readonly<Record<string, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
  valid_reported_cache_metrics: ValidReportedCacheMetrics;
  observed_completed_cache_read_input_tokens: CacheTokenSummary;
  observed_completed_cache_write_input_tokens: CacheTokenSummary;
  reported_over_completed: Readonly<{
    reported: number;
    completed: number;
    ratio: number | null;
  }>;
  gates: Readonly<{
    aggregate_completed_10k: Readonly<{
      minimum_completed: number;
      observed_completed: number;
      passed: boolean;
    }>;
    observed_cohort_completed_1k: Readonly<{
      minimum_completed_per_cohort: number;
      selection: "all_observed_provider_model_route_cohorts";
      material_use_selection: "not_available_from_terminal_events";
      passing_cohorts: number;
      below_threshold_cohorts: number;
      all_observed_cohorts_passed: boolean;
    }>;
    reported_coverage_99_5: Readonly<{
      minimum_ratio: number;
      reported: number;
      completed: number;
      observed_all_completed_passed: boolean;
      passing_observed_cohorts: number;
      below_threshold_observed_cohorts: number;
      all_observed_cohorts_passed: boolean;
      supported_model_membership: "not_available_from_terminal_events";
    }>;
    stage0_eligibility: Readonly<{
      status: "not_evaluated";
      reason: "supported-model membership and materially-used cohort selection are not available in terminal events";
    }>;
  }>;
  cohorts: readonly Stage0CohortReport[];
  inference_terminal_outcomes: InferenceTerminalOutcomesReport;
}>;

export class Stage0CacheTelemetryGateError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "Stage0CacheTelemetryGateError";
  }
}

type MutableCacheTokenSummary = {
  sum_tokens: number;
  observed_events: number;
  null_events: number;
  zero_events: number;
  positive_events: number;
};

type MutableValidReportedCacheMetrics = {
  reported_events: number;
  input_tokens: number;
  cached_input_tokens: number;
  cache_write_observed_events: number;
  cache_write_cached_input_tokens: number;
  cache_write_input_tokens: number;
};

type MutableCohort = {
  provider: string;
  model: string;
  route: string;
  completed_inference: number;
  status_totals: Map<string, number>;
  usage_telemetry_status_totals: Map<UsageTelemetryStatus, number>;
  valid_reported_cache_metrics: MutableValidReportedCacheMetrics;
  cache_read_input_tokens: MutableCacheTokenSummary;
  cache_write_input_tokens: MutableCacheTokenSummary;
  reported: number;
};

type MutableInferenceOutcomeCohort = {
  provider: string | null;
  model: string | null;
  route: InferenceRoute;
  stream: boolean | null;
  outcome: InferenceTerminalOutcome;
  stream_terminal_type: StreamTerminalType;
  terminal_events: number;
  terminal_without_usage: number;
  status_totals: Map<string, number>;
  usage_telemetry_status_totals: Map<UsageTelemetryStatus, number>;
  valid_reported_cache_metrics: MutableValidReportedCacheMetrics;
  cache_read_input_tokens: MutableCacheTokenSummary;
  cache_write_input_tokens: MutableCacheTokenSummary;
};

const hasOwn = (value: Record<string, unknown>, key: string): boolean =>
  Object.prototype.hasOwnProperty.call(value, key);

const isRecord = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value);

const fail = (lineNumber: number, detail: string): never => {
  throw new Stage0CacheTelemetryGateError(`line ${lineNumber}: ${detail}`);
};

const requireNonEmptyString = (record: Record<string, unknown>, key: string, lineNumber: number): string => {
  const value = record[key];
  if (!hasOwn(record, key) || typeof value !== "string" || value.trim().length === 0) {
    return fail(lineNumber, `terminal event has an invalid ${key} field`);
  }
  return value;
};

const hasAsciiControlCharacter = (value: string): boolean => {
  for (const character of value) {
    const code = character.codePointAt(0) ?? 0;
    if (code < 0x20 || code === 0x7f) return true;
  }
  return false;
};

const requireBoundedIdentifier = (
  record: Record<string, unknown>,
  key: string,
  maxLength: number,
  lineNumber: number,
): string => {
  const value = requireNonEmptyString(record, key, lineNumber);
  if (value.length > maxLength || hasAsciiControlCharacter(value) || !SAFE_IDENTIFIER_PATTERN.test(value)) {
    return fail(lineNumber, `terminal event has an invalid ${key} field`);
  }
  return value;
};

const requireRequestId = (record: Record<string, unknown>, lineNumber: number): string =>
  requireBoundedIdentifier(record, "request_id", MAX_REQUEST_ID_CHARS, lineNumber);

const requireReleaseString = (record: Record<string, unknown>, key: string, lineNumber: number): string => {
  const value = requireBoundedIdentifier(record, key, MAX_RELEASE_IDENTIFIER_CHARS, lineNumber);
  if (value.trim().toLowerCase() === "unknown") {
    return fail(lineNumber, "terminal event has a missing release identity");
  }
  return value;
};

const requireGitSha = (record: Record<string, unknown>, lineNumber: number): string => {
  const gitSha = requireReleaseString(record, "git_sha", lineNumber);
  if (!GIT_SHA_PATTERN.test(gitSha)) {
    return fail(lineNumber, "terminal event has an invalid git_sha field");
  }
  return gitSha;
};

const requireNullableString = (record: Record<string, unknown>, key: string, lineNumber: number): string | null => {
  if (!hasOwn(record, key)) return fail(lineNumber, `terminal event has an invalid ${key} field`);
  const value = record[key];
  if (value === null) return null;
  if (typeof value !== "string" || value.trim().length === 0) {
    return fail(lineNumber, `terminal event has an invalid ${key} field`);
  }
  return value;
};

const requireNullableReleaseString = (
  record: Record<string, unknown>,
  key: string,
  lineNumber: number,
): string | null => {
  if (!hasOwn(record, key)) return fail(lineNumber, `terminal event has an invalid ${key} field`);
  const value = record[key];
  if (value === null) return null;
  if (typeof value !== "string") return fail(lineNumber, `terminal event has an invalid ${key} field`);
  const bounded = requireBoundedIdentifier(record, key, MAX_RELEASE_IDENTIFIER_CHARS, lineNumber);
  if (bounded.toLowerCase() === "unknown") {
    return fail(lineNumber, "terminal event has a missing release identity");
  }
  return bounded;
};

const requireInferenceProvider = (record: Record<string, unknown>, lineNumber: number): string => {
  const provider = requireNonEmptyString(record, "provider", lineNumber);
  if (!INFERENCE_PROVIDERS.has(provider as "chatgpt_codex" | "yunwu")) {
    return fail(lineNumber, "inference terminal event has an unsupported provider field");
  }
  return provider;
};

const optionalInferenceProvider = (record: Record<string, unknown>): string | null => {
  const provider = record.provider;
  if (typeof provider !== "string" || provider.trim().length === 0) return null;
  return INFERENCE_PROVIDERS.has(provider as "chatgpt_codex" | "yunwu") ? provider : null;
};

const requireBoundedModelLabel = (record: Record<string, unknown>, lineNumber: number): string => {
  const model = requireNonEmptyString(record, "model", lineNumber);
  if (model.length > MAX_MODEL_LABEL_CHARS || hasAsciiControlCharacter(model)) {
    return fail(lineNumber, "inference terminal event has an invalid model field");
  }
  return model;
};

const optionalBoundedModelLabel = (record: Record<string, unknown>): string | null => {
  const model = record.model;
  if (typeof model !== "string" || model.trim().length === 0) return null;
  if (model.length > MAX_MODEL_LABEL_CHARS || hasAsciiControlCharacter(model)) return null;
  return model;
};

const requireTerminalRoute = (record: Record<string, unknown>, lineNumber: number): TerminalRoute => {
  const route = requireNonEmptyString(record, "route", lineNumber);
  if (!TERMINAL_ROUTES.has(route as TerminalRoute)) {
    return fail(lineNumber, "terminal event has an unsupported route field");
  }
  return route as TerminalRoute;
};

const requireStreamTerminalType = (record: Record<string, unknown>, lineNumber: number): StreamTerminalType | null => {
  const terminalType = requireNullableString(record, "stream_terminal_type", lineNumber);
  if (terminalType !== null && !STREAM_TERMINAL_TYPES.has(terminalType as StreamTerminalType)) {
    return fail(lineNumber, "terminal event has an unsupported stream_terminal_type field");
  }
  return terminalType as StreamTerminalType | null;
};

const requireStatus = (record: Record<string, unknown>, lineNumber: number): number => {
  const value = record.status;
  if (
    !hasOwn(record, "status") || typeof value !== "number" || !Number.isSafeInteger(value) || value < 100 || value > 599
  ) {
    return fail(lineNumber, "terminal event has an invalid status field");
  }
  return value;
};

const requireCacheToken = (record: Record<string, unknown>, key: string, lineNumber: number): number | null => {
  if (!hasOwn(record, key)) return fail(lineNumber, `terminal event has an invalid ${key} field`);
  const value = record[key];
  if (value === null) return null;
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
    return fail(lineNumber, `terminal event has an invalid ${key} field`);
  }
  return value;
};

const requireUsageTelemetryStatus = (record: Record<string, unknown>, lineNumber: number): UsageTelemetryStatus => {
  const value = record.usage_telemetry_status;
  if (
    !hasOwn(record, "usage_telemetry_status") || typeof value !== "string" ||
    !USAGE_TELEMETRY_STATUSES.has(value as UsageTelemetryStatus)
  ) {
    return fail(lineNumber, "terminal event has an invalid usage_telemetry_status field");
  }
  return value as UsageTelemetryStatus;
};

const requireUsageObserved = (record: Record<string, unknown>, lineNumber: number): boolean => {
  const value = record.usage_observed;
  if (!hasOwn(record, "usage_observed") || typeof value !== "boolean") {
    return fail(lineNumber, "terminal event has an invalid usage_observed field");
  }
  return value;
};

const requireBoolean = (record: Record<string, unknown>, key: string, lineNumber: number): boolean => {
  const value = record[key];
  if (!hasOwn(record, key) || typeof value !== "boolean") {
    return fail(lineNumber, `terminal event has an invalid ${key} field`);
  }
  return value;
};

const requirePromptCacheMode = (record: Record<string, unknown>, lineNumber: number): PromptCacheMode => {
  const mode = requireNonEmptyString(record, "prompt_cache_mode", lineNumber);
  if (!PROMPT_CACHE_MODES.has(mode as PromptCacheMode)) {
    return fail(lineNumber, "terminal event has an invalid prompt_cache_mode field");
  }
  return mode as PromptCacheMode;
};

const requireAccountSlot = (record: Record<string, unknown>, lineNumber: number): number | null => {
  if (!hasOwn(record, "account_slot")) return fail(lineNumber, "terminal event has an invalid account_slot field");
  const slot = record.account_slot;
  if (slot === null) return null;
  if (typeof slot !== "number" || !Number.isSafeInteger(slot) || slot < 0) {
    return fail(lineNumber, "terminal event has an invalid account_slot field");
  }
  return slot;
};

const requireAffinityOutcome = (record: Record<string, unknown>, lineNumber: number): AffinityOutcome => {
  const outcome = requireNonEmptyString(record, "affinity_outcome", lineNumber);
  if (!AFFINITY_OUTCOMES.has(outcome as AffinityOutcome)) {
    return fail(lineNumber, "terminal event has an invalid affinity_outcome field");
  }
  return outcome as AffinityOutcome;
};

const requireNullableBoolean = (record: Record<string, unknown>, key: string, lineNumber: number): boolean | null => {
  if (!hasOwn(record, key)) return fail(lineNumber, `terminal event has an invalid ${key} field`);
  const value = record[key];
  if (value === null || typeof value === "boolean") return value;
  return fail(lineNumber, `terminal event has an invalid ${key} field`);
};

const inferenceOutcomeFor = (
  route: TerminalRoute,
  streamTerminalType: StreamTerminalType | null,
): InferenceTerminalOutcome | null => {
  if (route !== "responses" && route !== "chat.completions") return null;
  if (streamTerminalType === "response.completed") return "completed";
  if (streamTerminalType === "response.incomplete") return "incomplete";
  if (streamTerminalType === "cancelled") return "cancelled";
  if (
    streamTerminalType === "response.failed" || streamTerminalType === "error" || streamTerminalType === "eof" ||
    streamTerminalType === "deadline"
  ) return "failed";
  return null;
};

const terminalPayloadFromText = (text: string, lineNumber: number): string | null => {
  const trimmed = text.trim();
  if (!trimmed.includes(TERMINAL_MARKER)) return null;
  // The bare console message and its exact `INFO ` export form are the only
  // raw line shapes this analyzer consumes. Anchoring the marker prevents
  // prompt or other user text embedded in an unrelated log line from
  // impersonating a terminal event. Structured exports must put one of these
  // exact console bodies in their `body` field below.
  const prefix = trimmed.startsWith(TERMINAL_LINE_PREFIX)
    ? TERMINAL_LINE_PREFIX
    : trimmed.startsWith(INFO_TERMINAL_LINE_PREFIX)
    ? INFO_TERMINAL_LINE_PREFIX
    : null;
  if (prefix === null) {
    return fail(lineNumber, "request_terminal log text must begin with the canonical terminal marker");
  }
  const json = trimmed.slice(prefix.length).trim();
  if (json.length === 0) return fail(lineNumber, "request_terminal event has no JSON payload");
  return json;
};

const terminalPayloadFromLine = (line: string, lineNumber: number): string | null => {
  const trimmed = line.trim();
  if (!trimmed.startsWith("{")) return terminalPayloadFromText(line, lineNumber);

  let envelope: unknown;
  try {
    envelope = JSON.parse(trimmed);
  } catch {
    if (line.includes(TERMINAL_MARKER)) {
      return fail(lineNumber, "request_terminal JSON envelope has malformed JSON");
    }
    return null;
  }
  if (!isRecord(envelope) || !hasOwn(envelope, "body")) {
    if (line.includes(TERMINAL_MARKER)) {
      return fail(lineNumber, "request_terminal event must use raw log text or a string body envelope");
    }
    return null;
  }
  if (typeof envelope.body !== "string") {
    if (line.includes(TERMINAL_MARKER)) {
      return fail(lineNumber, "request_terminal JSON envelope has an invalid body field");
    }
    return null;
  }
  return terminalPayloadFromText(envelope.body, lineNumber);
};

const parseTerminalEvent = (line: string, lineNumber: number): TerminalEvent | null => {
  const json = terminalPayloadFromLine(line, lineNumber);
  if (json === null) return null;

  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch {
    return fail(lineNumber, "request_terminal event has malformed JSON");
  }
  if (!isRecord(parsed)) return fail(lineNumber, "request_terminal event must be a JSON object");

  const requestId = requireRequestId(parsed, lineNumber);
  const route = requireTerminalRoute(parsed, lineNumber);
  const streamTerminalType = requireStreamTerminalType(parsed, lineNumber);
  const status = requireStatus(parsed, lineNumber);
  const usageTelemetryStatus = requireUsageTelemetryStatus(parsed, lineNumber);
  const usageObserved = requireUsageObserved(parsed, lineNumber);
  const inputTokens = requireCacheToken(parsed, "input_tokens", lineNumber);
  const cachedInputTokens = requireCacheToken(parsed, "cached_input_tokens", lineNumber);
  const cacheWriteInputTokens = requireCacheToken(parsed, "cache_write_input_tokens", lineNumber);
  const promptCacheKeyPresent = requireBoolean(parsed, "prompt_cache_key_present", lineNumber);
  const promptCacheMode = requirePromptCacheMode(parsed, lineNumber);
  const accountSlot = requireAccountSlot(parsed, lineNumber);
  const affinityOutcome = requireAffinityOutcome(parsed, lineNumber);
  const stream = requireNullableBoolean(parsed, "stream", lineNumber);
  const release: ReleaseIdentity = {
    git_sha: requireGitSha(parsed, lineNumber),
    deno_revision: requireReleaseString(parsed, "deno_revision", lineNumber),
    router_revision: requireNullableReleaseString(parsed, "router_revision", lineNumber),
  };

  const inferenceOutcome = inferenceOutcomeFor(route, streamTerminalType);
  if (inferenceOutcome === "completed" && (status < 200 || status >= 300)) {
    return fail(lineNumber, "completed inference event has a non-2xx status field");
  }
  const provider = inferenceOutcome === "completed"
    ? requireInferenceProvider(parsed, lineNumber)
    : inferenceOutcome === null
    ? null
    : optionalInferenceProvider(parsed);
  const model = inferenceOutcome === "completed"
    ? requireBoundedModelLabel(parsed, lineNumber)
    : inferenceOutcome === null
    ? null
    : optionalBoundedModelLabel(parsed);
  if (usageObserved !== (usageTelemetryStatus !== "missing")) {
    return fail(lineNumber, "terminal event has inconsistent usage_observed and usage_telemetry_status fields");
  }
  if (
    inferenceOutcome !== null && usageTelemetryStatus === "reported" &&
    (inputTokens === null || cachedInputTokens === null)
  ) {
    return fail(lineNumber, "reported inference terminal event is missing cache-read usage fields");
  }

  return {
    request_id: requestId,
    route,
    status,
    stream_terminal_type: streamTerminalType,
    input_tokens: inputTokens,
    cached_input_tokens: cachedInputTokens,
    cache_write_input_tokens: cacheWriteInputTokens,
    usage_observed: usageObserved,
    usage_telemetry_status: usageTelemetryStatus,
    prompt_cache_key_present: promptCacheKeyPresent,
    prompt_cache_mode: promptCacheMode,
    account_slot: accountSlot,
    affinity_outcome: affinityOutcome,
    stream,
    release,
    inference_outcome: inferenceOutcome,
    provider,
    model,
  };
};

const createCacheTokenSummary = (): MutableCacheTokenSummary => ({
  sum_tokens: 0,
  observed_events: 0,
  null_events: 0,
  zero_events: 0,
  positive_events: 0,
});

const createValidReportedCacheMetrics = (): MutableValidReportedCacheMetrics => ({
  reported_events: 0,
  input_tokens: 0,
  cached_input_tokens: 0,
  cache_write_observed_events: 0,
  cache_write_cached_input_tokens: 0,
  cache_write_input_tokens: 0,
});

const addSafely = (left: number, right: number, lineNumber: number): number => {
  const sum = left + right;
  if (!Number.isSafeInteger(sum)) fail(lineNumber, "terminal event aggregation exceeds the safe integer range");
  return sum;
};

const increment = <Key extends string>(counts: Map<Key, number>, key: Key, lineNumber: number): void => {
  counts.set(key, addSafely(counts.get(key) ?? 0, 1, lineNumber));
};

const addCacheToken = (summary: MutableCacheTokenSummary, value: number | null, lineNumber: number): void => {
  if (value === null) {
    summary.null_events = addSafely(summary.null_events, 1, lineNumber);
    return;
  }

  summary.sum_tokens = addSafely(summary.sum_tokens, value, lineNumber);
  summary.observed_events = addSafely(summary.observed_events, 1, lineNumber);
  if (value === 0) summary.zero_events = addSafely(summary.zero_events, 1, lineNumber);
  else summary.positive_events = addSafely(summary.positive_events, 1, lineNumber);
};

const toCacheTokenSummary = (summary: MutableCacheTokenSummary): CacheTokenSummary => ({ ...summary });

const addValidReportedCacheMetrics = (
  metrics: MutableValidReportedCacheMetrics,
  event: TerminalEvent,
  lineNumber: number,
): void => {
  if (
    event.usage_telemetry_status !== "reported" || event.input_tokens === null ||
    event.cached_input_tokens === null
  ) return;

  metrics.reported_events = addSafely(metrics.reported_events, 1, lineNumber);
  metrics.input_tokens = addSafely(metrics.input_tokens, event.input_tokens, lineNumber);
  metrics.cached_input_tokens = addSafely(metrics.cached_input_tokens, event.cached_input_tokens, lineNumber);
  if (event.cache_write_input_tokens === null) return;

  metrics.cache_write_observed_events = addSafely(metrics.cache_write_observed_events, 1, lineNumber);
  metrics.cache_write_cached_input_tokens = addSafely(
    metrics.cache_write_cached_input_tokens,
    event.cached_input_tokens,
    lineNumber,
  );
  metrics.cache_write_input_tokens = addSafely(
    metrics.cache_write_input_tokens,
    event.cache_write_input_tokens,
    lineNumber,
  );
};

const toValidReportedCacheMetrics = (
  metrics: MutableValidReportedCacheMetrics,
): ValidReportedCacheMetrics => ({
  reported_events: metrics.reported_events,
  cache_read: {
    aggregate_input_tokens: metrics.input_tokens,
    aggregate_cached_input_tokens: metrics.cached_input_tokens,
    ratio: metrics.input_tokens === 0 ? null : metrics.cached_input_tokens / metrics.input_tokens,
  },
  cache_write_payback: {
    observed_events: metrics.cache_write_observed_events,
    aggregate_cached_input_tokens: metrics.cache_write_cached_input_tokens,
    aggregate_cache_write_input_tokens: metrics.cache_write_input_tokens,
    ratio: metrics.cache_write_input_tokens === 0
      ? null
      : metrics.cache_write_cached_input_tokens / metrics.cache_write_input_tokens,
  },
});

const toSortedCounts = <Key extends string>(counts: Map<Key, number>): Record<Key, number> =>
  Object.fromEntries(
    [...counts.entries()].sort(([left], [right]) => left.localeCompare(right)),
  ) as Record<Key, number>;

const toFixedCounts = <Key extends string>(keys: readonly Key[], counts: Map<Key, number>): Record<Key, number> =>
  Object.fromEntries(keys.map((key) => [key, counts.get(key) ?? 0])) as Record<Key, number>;

const toOpaqueModelLabels = (models: Iterable<string>): Map<string, string> =>
  new Map(
    [...new Set(models)]
      .sort((left, right) => left.localeCompare(right))
      .map((model, index) => [model, `model_${index + 1}`] as const),
  );

const extendOpaqueModelLabels = (
  labels: ReadonlyMap<string, string>,
  models: Iterable<string>,
): Map<string, string> => {
  const extended = new Map(labels);
  for (
    const model of [...new Set(models)].filter((model) => !extended.has(model)).sort((left, right) =>
      left.localeCompare(right)
    )
  ) {
    extended.set(model, `model_${extended.size + 1}`);
  }
  return extended;
};

const reportedOverCompleted = (
  reported: number,
  completed: number,
): { reported: number; completed: number; ratio: number | null } => ({
  reported,
  completed,
  ratio: completed === 0 ? null : reported / completed,
});

const sameRelease = (left: ReleaseIdentity, right: ReleaseIdentity): boolean =>
  left.git_sha === right.git_sha &&
  left.deno_revision === right.deno_revision &&
  left.router_revision === right.router_revision;

const toReleaseReportIdentity = (release: ReleaseIdentity): ReleaseReportIdentity => ({
  git_sha: release.git_sha,
  deno_revision: "validated",
  router_revision: release.router_revision === null ? null : "validated",
});

class Stage0CacheTelemetryAccumulator {
  #release: ReleaseIdentity | null = null;
  // Retained only for the current stdin stream. Request identifiers are never
  // included in errors or output, but a duplicated terminal event must not
  // inflate a Stage 0 evidence gate.
  #seenRequestIds = new Set<string>();
  #terminalEvents = 0;
  #completedInference = 0;
  #reportedCompleted = 0;
  #statusTotals = new Map<string, number>();
  #completedStatusTotals = new Map<string, number>();
  #usageTelemetryStatusTotals = new Map<UsageTelemetryStatus, number>();
  #validReportedCacheMetrics = createValidReportedCacheMetrics();
  #cacheReadInputTokens = createCacheTokenSummary();
  #cacheWriteInputTokens = createCacheTokenSummary();
  #cohorts = new Map<string, MutableCohort>();
  #inferenceTerminalEvents = 0;
  #inferenceTerminalWithoutUsage = 0;
  #inferenceOutcomeTotals = new Map<InferenceTerminalOutcome, number>();
  #inferenceUsageTelemetryStatusTotals = new Map<UsageTelemetryStatus, number>();
  #promptCacheKeyPresentEvents = 0;
  #promptCacheKeyAbsentEvents = 0;
  #promptCacheModeTotals = new Map<PromptCacheMode, number>();
  #assignedAccountSlotEvents = 0;
  #unassignedAccountSlotEvents = 0;
  #assignedAccountSlots = new Set<number>();
  #affinityOutcomeTotals = new Map<AffinityOutcome, number>();
  #inferenceOutcomeCohorts = new Map<string, MutableInferenceOutcomeCohort>();

  addLine(line: string, lineNumber: number): void {
    const event = parseTerminalEvent(line, lineNumber);
    if (event === null) return;

    if (this.#seenRequestIds.has(event.request_id)) {
      fail(lineNumber, "duplicate request_terminal event");
    }
    if (this.#seenRequestIds.size >= MAX_RETAINED_REQUEST_IDS) {
      fail(lineNumber, "request_terminal event retention limit exceeded");
    }
    this.#seenRequestIds.add(event.request_id);
    this.#terminalEvents = addSafely(this.#terminalEvents, 1, lineNumber);
    increment(this.#statusTotals, String(event.status), lineNumber);

    if (this.#release === null) this.#release = event.release;
    else if (!sameRelease(this.#release, event.release)) {
      fail(lineNumber, "terminal event release identity differs from an earlier terminal event");
    }

    if (event.inference_outcome !== null) this.#addInferenceOutcome(event, lineNumber);
    if (event.inference_outcome !== "completed") return;
    const provider = event.provider ?? fail(lineNumber, "completed inference event is missing a provider or model");
    const model = event.model ?? fail(lineNumber, "completed inference event is missing a provider or model");

    this.#completedInference = addSafely(this.#completedInference, 1, lineNumber);
    increment(this.#completedStatusTotals, String(event.status), lineNumber);
    increment(this.#usageTelemetryStatusTotals, event.usage_telemetry_status, lineNumber);
    if (event.usage_telemetry_status === "reported") {
      this.#reportedCompleted = addSafely(this.#reportedCompleted, 1, lineNumber);
    }
    addValidReportedCacheMetrics(this.#validReportedCacheMetrics, event, lineNumber);
    addCacheToken(this.#cacheReadInputTokens, event.cached_input_tokens, lineNumber);
    addCacheToken(this.#cacheWriteInputTokens, event.cache_write_input_tokens, lineNumber);

    const key = JSON.stringify([provider, model, event.route]);
    const existingCohort = this.#cohorts.get(key);
    let cohort: MutableCohort;
    if (existingCohort === undefined) {
      cohort = {
        provider,
        model,
        route: event.route,
        completed_inference: 0,
        status_totals: new Map(),
        usage_telemetry_status_totals: new Map(),
        valid_reported_cache_metrics: createValidReportedCacheMetrics(),
        cache_read_input_tokens: createCacheTokenSummary(),
        cache_write_input_tokens: createCacheTokenSummary(),
        reported: 0,
      };
      this.#cohorts.set(key, cohort);
    } else cohort = existingCohort;
    cohort.completed_inference = addSafely(cohort.completed_inference, 1, lineNumber);
    increment(cohort.status_totals, String(event.status), lineNumber);
    increment(cohort.usage_telemetry_status_totals, event.usage_telemetry_status, lineNumber);
    if (event.usage_telemetry_status === "reported") {
      cohort.reported = addSafely(cohort.reported, 1, lineNumber);
    }
    addValidReportedCacheMetrics(cohort.valid_reported_cache_metrics, event, lineNumber);
    addCacheToken(cohort.cache_read_input_tokens, event.cached_input_tokens, lineNumber);
    addCacheToken(cohort.cache_write_input_tokens, event.cache_write_input_tokens, lineNumber);
  }

  #addInferenceOutcome(event: TerminalEvent, lineNumber: number): void {
    const outcome = event.inference_outcome ?? fail(lineNumber, "inference terminal event is missing an outcome");
    const provider = event.provider;
    const model = event.model;
    const streamTerminalType = event.stream_terminal_type ??
      fail(lineNumber, "inference terminal event is missing a stream_terminal_type");
    const route: InferenceRoute = event.route === "responses" || event.route === "chat.completions"
      ? event.route
      : fail(lineNumber, "inference terminal event has an invalid route field");

    this.#inferenceTerminalEvents = addSafely(this.#inferenceTerminalEvents, 1, lineNumber);
    increment(this.#inferenceOutcomeTotals, outcome, lineNumber);
    increment(this.#inferenceUsageTelemetryStatusTotals, event.usage_telemetry_status, lineNumber);
    if (event.prompt_cache_key_present) {
      this.#promptCacheKeyPresentEvents = addSafely(this.#promptCacheKeyPresentEvents, 1, lineNumber);
    } else {
      this.#promptCacheKeyAbsentEvents = addSafely(this.#promptCacheKeyAbsentEvents, 1, lineNumber);
    }
    increment(this.#promptCacheModeTotals, event.prompt_cache_mode, lineNumber);
    if (event.account_slot === null) {
      this.#unassignedAccountSlotEvents = addSafely(this.#unassignedAccountSlotEvents, 1, lineNumber);
    } else {
      this.#assignedAccountSlotEvents = addSafely(this.#assignedAccountSlotEvents, 1, lineNumber);
      this.#assignedAccountSlots.add(event.account_slot);
    }
    increment(this.#affinityOutcomeTotals, event.affinity_outcome, lineNumber);

    const key = JSON.stringify([
      provider,
      model,
      route,
      event.stream,
      outcome,
      streamTerminalType,
    ]);
    const existingCohort = this.#inferenceOutcomeCohorts.get(key);
    let cohort: MutableInferenceOutcomeCohort;
    if (existingCohort === undefined) {
      cohort = {
        provider,
        model,
        route,
        stream: event.stream,
        outcome,
        stream_terminal_type: streamTerminalType,
        terminal_events: 0,
        terminal_without_usage: 0,
        status_totals: new Map(),
        usage_telemetry_status_totals: new Map(),
        valid_reported_cache_metrics: createValidReportedCacheMetrics(),
        cache_read_input_tokens: createCacheTokenSummary(),
        cache_write_input_tokens: createCacheTokenSummary(),
      };
      this.#inferenceOutcomeCohorts.set(key, cohort);
    } else cohort = existingCohort;

    cohort.terminal_events = addSafely(cohort.terminal_events, 1, lineNumber);
    increment(cohort.status_totals, String(event.status), lineNumber);
    increment(cohort.usage_telemetry_status_totals, event.usage_telemetry_status, lineNumber);
    if (event.usage_telemetry_status === "missing") {
      this.#inferenceTerminalWithoutUsage = addSafely(this.#inferenceTerminalWithoutUsage, 1, lineNumber);
      cohort.terminal_without_usage = addSafely(cohort.terminal_without_usage, 1, lineNumber);
    }
    addValidReportedCacheMetrics(cohort.valid_reported_cache_metrics, event, lineNumber);
    addCacheToken(cohort.cache_read_input_tokens, event.cached_input_tokens, lineNumber);
    addCacheToken(cohort.cache_write_input_tokens, event.cache_write_input_tokens, lineNumber);
  }

  finish(): Stage0CacheTelemetryReport {
    if (this.#terminalEvents === 0 || this.#release === null) {
      throw new Stage0CacheTelemetryGateError("no request_terminal events were found on stdin");
    }

    const coverage = reportedOverCompleted(this.#reportedCompleted, this.#completedInference);
    const modelCohortLabels = toOpaqueModelLabels([...this.#cohorts.values()].map((cohort) => cohort.model));
    const outcomeModelCohortLabels = extendOpaqueModelLabels(
      modelCohortLabels,
      [...this.#inferenceOutcomeCohorts.values()]
        .map((cohort) => cohort.model)
        .filter((model): model is string => model !== null),
    );
    const cohorts = [...this.#cohorts.values()]
      .sort((left, right) =>
        left.provider.localeCompare(right.provider) ||
        left.model.localeCompare(right.model) ||
        left.route.localeCompare(right.route)
      )
      .map((cohort): Stage0CohortReport => {
        const cohortCoverage = reportedOverCompleted(cohort.reported, cohort.completed_inference);
        return {
          provider: cohort.provider,
          model: modelCohortLabels.get(cohort.model)!,
          route: cohort.route,
          completed_inference: cohort.completed_inference,
          status_totals: toSortedCounts(cohort.status_totals),
          usage_telemetry_status_totals: toSortedCounts(cohort.usage_telemetry_status_totals),
          valid_reported_cache_metrics: toValidReportedCacheMetrics(cohort.valid_reported_cache_metrics),
          observed_completed_cache_read_input_tokens: toCacheTokenSummary(cohort.cache_read_input_tokens),
          observed_completed_cache_write_input_tokens: toCacheTokenSummary(cohort.cache_write_input_tokens),
          reported_over_completed: cohortCoverage,
          completed_1k_gate: {
            minimum_completed: STAGE0_COHORT_MIN_COMPLETED,
            observed_completed: cohort.completed_inference,
            passed: cohort.completed_inference >= STAGE0_COHORT_MIN_COMPLETED,
          },
          reported_coverage_99_5_gate: {
            minimum_ratio: STAGE0_MIN_REPORTED_COVERAGE,
            reported: cohortCoverage.reported,
            completed: cohortCoverage.completed,
            ratio: cohortCoverage.ratio,
            passed: (cohortCoverage.ratio ?? 0) >= STAGE0_MIN_REPORTED_COVERAGE,
          },
        };
      });
    const inferenceOutcomeCohorts = [...this.#inferenceOutcomeCohorts.values()]
      .sort((left, right) =>
        (left.provider ?? "unknown").localeCompare(right.provider ?? "unknown") ||
        (left.model ?? "").localeCompare(right.model ?? "") ||
        left.route.localeCompare(right.route) ||
        String(left.stream).localeCompare(String(right.stream)) ||
        left.outcome.localeCompare(right.outcome) ||
        left.stream_terminal_type.localeCompare(right.stream_terminal_type)
      )
      .map((cohort): InferenceOutcomeCohortReport => ({
        provider: cohort.provider ?? "unknown",
        model: cohort.model === null ? "model_unknown" : outcomeModelCohortLabels.get(cohort.model)!,
        route: cohort.route,
        stream: cohort.stream,
        outcome: cohort.outcome,
        stream_terminal_type: cohort.stream_terminal_type,
        terminal_events: cohort.terminal_events,
        terminal_without_usage: cohort.terminal_without_usage,
        status_totals: toSortedCounts(cohort.status_totals),
        usage_telemetry_status_totals: toSortedCounts(cohort.usage_telemetry_status_totals),
        valid_reported_cache_metrics: toValidReportedCacheMetrics(cohort.valid_reported_cache_metrics),
        observed_cache_read_input_tokens: toCacheTokenSummary(cohort.cache_read_input_tokens),
        observed_cache_write_input_tokens: toCacheTokenSummary(cohort.cache_write_input_tokens),
      }));
    const passingCohorts = cohorts.filter((cohort) => cohort.completed_1k_gate.passed).length;
    const coveragePassingCohorts = cohorts.filter((cohort) => cohort.reported_coverage_99_5_gate.passed).length;

    return {
      version: 1,
      release: toReleaseReportIdentity(this.#release),
      terminal_events: this.#terminalEvents,
      status_totals: toSortedCounts(this.#statusTotals),
      completed_inference: this.#completedInference,
      completed_status_totals: toSortedCounts(this.#completedStatusTotals),
      usage_telemetry_status_totals: toSortedCounts(this.#usageTelemetryStatusTotals),
      valid_reported_cache_metrics: toValidReportedCacheMetrics(this.#validReportedCacheMetrics),
      observed_completed_cache_read_input_tokens: toCacheTokenSummary(this.#cacheReadInputTokens),
      observed_completed_cache_write_input_tokens: toCacheTokenSummary(this.#cacheWriteInputTokens),
      reported_over_completed: coverage,
      gates: {
        aggregate_completed_10k: {
          minimum_completed: STAGE0_AGGREGATE_MIN_COMPLETED,
          observed_completed: this.#completedInference,
          passed: this.#completedInference >= STAGE0_AGGREGATE_MIN_COMPLETED,
        },
        observed_cohort_completed_1k: {
          minimum_completed_per_cohort: STAGE0_COHORT_MIN_COMPLETED,
          selection: "all_observed_provider_model_route_cohorts",
          material_use_selection: "not_available_from_terminal_events",
          passing_cohorts: passingCohorts,
          below_threshold_cohorts: cohorts.length - passingCohorts,
          all_observed_cohorts_passed: cohorts.length > 0 && passingCohorts === cohorts.length,
        },
        reported_coverage_99_5: {
          minimum_ratio: STAGE0_MIN_REPORTED_COVERAGE,
          reported: coverage.reported,
          completed: coverage.completed,
          observed_all_completed_passed: coverage.ratio !== null && coverage.ratio >= STAGE0_MIN_REPORTED_COVERAGE,
          passing_observed_cohorts: coveragePassingCohorts,
          below_threshold_observed_cohorts: cohorts.length - coveragePassingCohorts,
          all_observed_cohorts_passed: cohorts.length > 0 && coveragePassingCohorts === cohorts.length,
          supported_model_membership: "not_available_from_terminal_events",
        },
        stage0_eligibility: {
          status: "not_evaluated",
          reason:
            "supported-model membership and materially-used cohort selection are not available in terminal events",
        },
      },
      cohorts,
      inference_terminal_outcomes: {
        terminal_events: this.#inferenceTerminalEvents,
        terminal_without_usage: this.#inferenceTerminalWithoutUsage,
        outcome_totals: toFixedCounts(INFERENCE_TERMINAL_OUTCOMES, this.#inferenceOutcomeTotals),
        usage_telemetry_status_totals: toSortedCounts(this.#inferenceUsageTelemetryStatusTotals),
        prompt_cache_key_presence: {
          present: this.#promptCacheKeyPresentEvents,
          absent: this.#promptCacheKeyAbsentEvents,
        },
        prompt_cache_mode_totals: toFixedCounts(PROMPT_CACHE_MODE_VALUES, this.#promptCacheModeTotals),
        account_slot_summary: {
          assigned_terminal_events: this.#assignedAccountSlotEvents,
          unassigned_terminal_events: this.#unassignedAccountSlotEvents,
          distinct_assigned_slots: this.#assignedAccountSlots.size,
        },
        affinity_outcome_totals: toFixedCounts(AFFINITY_OUTCOME_VALUES, this.#affinityOutcomeTotals),
        cohorts: inferenceOutcomeCohorts,
      },
    };
  }
}

export const analyzeStage0CacheTelemetryLines = (lines: Iterable<string>): Stage0CacheTelemetryReport => {
  const accumulator = new Stage0CacheTelemetryAccumulator();
  let lineNumber = 0;
  for (const line of lines) {
    lineNumber += 1;
    accumulator.addLine(line, lineNumber);
  }
  return accumulator.finish();
};

async function* readStdinLines(): AsyncGenerator<string> {
  let remaining = "";
  for await (const chunk of Deno.stdin.readable.pipeThrough(new TextDecoderStream())) {
    const lines = `${remaining}${chunk}`.split("\n");
    remaining = lines.pop() ?? "";
    for (const line of lines) yield line.endsWith("\r") ? line.slice(0, -1) : line;
  }
  if (remaining.length > 0) yield remaining.endsWith("\r") ? remaining.slice(0, -1) : remaining;
}

const analyzeStdin = async (): Promise<Stage0CacheTelemetryReport> => {
  const accumulator = new Stage0CacheTelemetryAccumulator();
  let lineNumber = 0;
  for await (const line of readStdinLines()) {
    lineNumber += 1;
    accumulator.addLine(line, lineNumber);
  }
  return accumulator.finish();
};

if (import.meta.main) {
  if (Deno.args.length > 0) {
    console.error("stage0-cache-telemetry-gate accepts stdin only and does not support arguments");
    Deno.exit(2);
  }

  try {
    console.log(JSON.stringify(await analyzeStdin(), null, 2));
  } catch (error) {
    if (error instanceof Stage0CacheTelemetryGateError) {
      console.error(`stage0-cache-telemetry-gate: ${error.message}`);
    } else {
      // Do not serialize unexpected errors: some runtimes include input values
      // in parser diagnostics, and request-level data must never be echoed.
      console.error("stage0-cache-telemetry-gate: analysis failed");
    }
    Deno.exit(2);
  }
}
