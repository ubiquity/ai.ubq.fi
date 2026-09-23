// Stage 0 cache telemetry report types and constants, split out of scripts/stage0-cache-telemetry-gate.ts.

export const TERMINAL_MARKER = "[ai.ubq.fi] request_terminal";
export const TERMINAL_LINE_PREFIX = `${TERMINAL_MARKER} `;
export const INFO_TERMINAL_LINE_PREFIX = `INFO ${TERMINAL_LINE_PREFIX}`;

export const STAGE0_AGGREGATE_MIN_COMPLETED = 10_000;
export const STAGE0_COHORT_MIN_COMPLETED = 1_000;
export const STAGE0_MIN_REPORTED_COVERAGE = 0.995;

export const MAX_REQUEST_ID_CHARS = 128;
export const MAX_RELEASE_IDENTIFIER_CHARS = 128;
// The completed-evidence gate needs 10k events; cap retained IDs so hostile
// stdin cannot turn duplicate protection into unbounded memory retention.
export const MAX_RETAINED_REQUEST_IDS = 100_000;
export const SAFE_IDENTIFIER_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._:-]*$/;
export const GIT_SHA_PATTERN = /^[0-9a-f]{7,64}$/i;
export const SHA256_HEX_PATTERN = /^[0-9a-f]{64}$/;
export const USAGE_TELEMETRY_STATUSES = new Set(["missing", "partial", "reported", "invalid"] as const);
// A Responses or Chat inference terminal is emitted only by these in-process
// transports. Keeping this vocabulary closed means a forged or newly added
// provider cannot silently become a Stage 0 or outcome cohort.
export const INFERENCE_PROVIDER_VALUES = ["chatgpt_codex", "metered", "surplus"] as const;
export const INFERENCE_PROVIDERS = new Set(INFERENCE_PROVIDER_VALUES);
export const PROMPT_CACHE_MODE_VALUES = ["implicit", "explicit", "legacy_retention", "unspecified"] as const;
export const PROMPT_CACHE_MODES = new Set(PROMPT_CACHE_MODE_VALUES);
export const ACTIVE_TRANSITION_REASON_VALUES = ["quota_exhausted", "credential_invalid", "account_removed_or_replaced"] as const;
/** `none` is only the report bucket for a null wire reason, never a wire value. */
export const ACTIVE_TRANSITION_REASON_KEYS = ["none", ...ACTIVE_TRANSITION_REASON_VALUES] as const;
export const ACTIVE_TRANSITION_REASONS = new Set(ACTIVE_TRANSITION_REASON_VALUES);
export const INFERENCE_TERMINAL_OUTCOMES = ["completed", "failed", "incomplete", "cancelled"] as const;
export const MAX_MODEL_LABEL_CHARS = 128;
export const TERMINAL_ROUTES = new Set(["responses", "chat.completions", "embeddings", "embeddings.jobs.create", "embeddings.jobs.get"] as const);
export const STREAM_TERMINAL_TYPES = new Set([
  "response.completed",
  "response.failed",
  "response.incomplete",
  "error",
  "eof",
  "cancelled",
  "deadline",
] as const);

export type UsageTelemetryStatus = "missing" | "partial" | "reported" | "invalid";
export type InferenceProvider = (typeof INFERENCE_PROVIDER_VALUES)[number];
export type TerminalRoute = "responses" | "chat.completions" | "embeddings" | "embeddings.jobs.create" | "embeddings.jobs.get";
export type InferenceRoute = "responses" | "chat.completions";
export type InferenceTerminalOutcome = (typeof INFERENCE_TERMINAL_OUTCOMES)[number];
export type PromptCacheMode = "implicit" | "explicit" | "legacy_retention" | "unspecified";
export type ActiveTransitionReason = "quota_exhausted" | "credential_invalid" | "account_removed_or_replaced" | null;
export type StreamTerminalType = "response.completed" | "response.failed" | "response.incomplete" | "error" | "eof" | "cancelled" | "deadline";

export type ReleaseIdentity = Readonly<{
  git_sha: string;
  deno_revision: string;
  router_revision: string | null;
}>;

/**
 * A report keeps the immutable SHA visible, but never echoes arbitrary
 * deployment or upstream router revision values from its input stream.
 */
export type ReleaseReportIdentity = Readonly<{
  git_sha: string;
  deno_revision: "validated";
  router_revision: "validated" | null;
}>;

export type TerminalEvent = Readonly<{
  request_id: string;
  route: TerminalRoute;
  status: number;
  latency_ms: number | null;
  stream_terminal_type: StreamTerminalType | null;
  input_tokens: number | null;
  cached_input_tokens: number | null;
  cache_write_input_tokens: number | null;
  usage_observed: boolean;
  usage_telemetry_status: UsageTelemetryStatus;
  prompt_cache_key_present: boolean;
  prompt_cache_mode: PromptCacheMode;
  account_slot: number | null;
  account_cohort_id: string | null;
  active_generation: number | null;
  active_transition_reason: ActiveTransitionReason;
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

export type LatencySummary = Readonly<{
  observed_events: number;
  min_ms: number | null;
  p50_ms: number | null;
  p95_ms: number | null;
  max_ms: number | null;
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
 * Content-free cache evidence across every routing dimension that can change
 * prefix reuse. Model values remain opaque and cache-key values never enter
 * the analyzer.
 */
export type CacheDimensionCohortReport = Readonly<{
  provider: string;
  /** Opaque, per-report cohort label; never the logged model value. */
  model: string;
  /** Stable pseudonymous join key; it is not secrecy for guessable model IDs. */
  model_cohort_id: string;
  route: InferenceRoute;
  /** Opaque, per-report label; never the isolate-local account-slot value. */
  account_slot_cohort: string;
  /** Stable high-entropy account join key; null on older or paid-provider logs. */
  account_cohort_id: string | null;
  prompt_cache_mode: PromptCacheMode;
  prompt_cache_key_present: boolean;
  completed_inference: number;
  status_totals: Readonly<Record<string, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
  valid_reported_cache_metrics: ValidReportedCacheMetrics;
  observed_completed_cache_read_input_tokens: CacheTokenSummary;
  observed_completed_cache_write_input_tokens: CacheTokenSummary;
  completed_latency_ms: LatencySummary;
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
  active_generation_summary: Readonly<{
    assigned_terminal_events: number;
    unassigned_terminal_events: number;
  }>;
  active_transition_reason_totals: Readonly<Record<(typeof ACTIVE_TRANSITION_REASON_KEYS)[number], number>>;
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
  completed_latency_ms: LatencySummary;
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
  cache_dimension_cohorts: readonly CacheDimensionCohortReport[];
  inference_terminal_outcomes: InferenceTerminalOutcomesReport;
}>;

export class Stage0CacheTelemetryGateError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "Stage0CacheTelemetryGateError";
  }
}

export type MutableCacheTokenSummary = {
  sum_tokens: number;
  observed_events: number;
  null_events: number;
  zero_events: number;
  positive_events: number;
};

export type MutableLatencySummary = number[];

export type MutableValidReportedCacheMetrics = {
  reported_events: number;
  input_tokens: number;
  cached_input_tokens: number;
  cache_write_observed_events: number;
  cache_write_cached_input_tokens: number;
  cache_write_input_tokens: number;
};

export type MutableCohort = {
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

export type MutableCacheDimensionCohort = {
  provider: InferenceProvider;
  model: string;
  route: InferenceRoute;
  account_slot: number | null;
  account_cohort_id: string | null;
  prompt_cache_mode: PromptCacheMode;
  prompt_cache_key_present: boolean;
  completed_inference: number;
  status_totals: Map<string, number>;
  usage_telemetry_status_totals: Map<UsageTelemetryStatus, number>;
  valid_reported_cache_metrics: MutableValidReportedCacheMetrics;
  cache_read_input_tokens: MutableCacheTokenSummary;
  cache_write_input_tokens: MutableCacheTokenSummary;
  latency_ms: MutableLatencySummary;
};

export type MutableInferenceOutcomeCohort = {
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
