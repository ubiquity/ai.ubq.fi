/**
 * Offline Stage 0 prompt-cache telemetry analyzer.
 *
 * This script deliberately accepts only stdin. It never reads a log file,
 * connects to a service, or persists the input. Its JSON output contains only
 * release identity and aggregate provider/model/route telemetry; it never
 * echoes an input line or request-level identifiers.
 */

const TERMINAL_MARKER = "[ai.ubq.fi] request_terminal";

export const STAGE0_AGGREGATE_MIN_COMPLETED = 10_000;
export const STAGE0_COHORT_MIN_COMPLETED = 1_000;
export const STAGE0_MIN_REPORTED_COVERAGE = 0.995;

const USAGE_TELEMETRY_STATUSES = new Set(["missing", "partial", "reported", "invalid"] as const);
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

type TerminalEvent = Readonly<{
  request_id: string;
  route: TerminalRoute;
  status: number;
  stream_terminal_type: StreamTerminalType | null;
  cached_input_tokens: number | null;
  cache_write_input_tokens: number | null;
  usage_observed: boolean;
  usage_telemetry_status: UsageTelemetryStatus;
  release: ReleaseIdentity;
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

export type Stage0CohortReport = Readonly<{
  provider: string;
  model: string;
  route: string;
  completed_inference: number;
  status_totals: Readonly<Record<string, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
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

export type Stage0CacheTelemetryReport = Readonly<{
  version: 1;
  release: ReleaseIdentity;
  terminal_events: number;
  status_totals: Readonly<Record<string, number>>;
  completed_inference: number;
  completed_status_totals: Readonly<Record<string, number>>;
  usage_telemetry_status_totals: Readonly<Record<UsageTelemetryStatus, number>>;
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

type MutableCohort = {
  provider: string;
  model: string;
  route: string;
  completed_inference: number;
  status_totals: Map<string, number>;
  usage_telemetry_status_totals: Map<UsageTelemetryStatus, number>;
  cache_read_input_tokens: MutableCacheTokenSummary;
  cache_write_input_tokens: MutableCacheTokenSummary;
  reported: number;
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

const requireReleaseString = (record: Record<string, unknown>, key: string, lineNumber: number): string => {
  const value = requireNonEmptyString(record, key, lineNumber);
  if (value.trim().toLowerCase() === "unknown") {
    return fail(lineNumber, "terminal event has a missing release identity");
  }
  return value;
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

const terminalPayloadFromText = (text: string, lineNumber: number): string | null => {
  const markerIndex = text.indexOf(TERMINAL_MARKER);
  if (markerIndex === -1) return null;
  const json = text.slice(markerIndex + TERMINAL_MARKER.length).trim();
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

  const requestId = requireNonEmptyString(parsed, "request_id", lineNumber);
  const route = requireTerminalRoute(parsed, lineNumber);
  const streamTerminalType = requireStreamTerminalType(parsed, lineNumber);
  const status = requireStatus(parsed, lineNumber);
  const usageTelemetryStatus = requireUsageTelemetryStatus(parsed, lineNumber);
  const usageObserved = requireUsageObserved(parsed, lineNumber);
  const cachedInputTokens = requireCacheToken(parsed, "cached_input_tokens", lineNumber);
  const cacheWriteInputTokens = requireCacheToken(parsed, "cache_write_input_tokens", lineNumber);
  const release: ReleaseIdentity = {
    git_sha: requireReleaseString(parsed, "git_sha", lineNumber),
    deno_revision: requireReleaseString(parsed, "deno_revision", lineNumber),
    router_revision: requireNullableString(parsed, "router_revision", lineNumber),
  };

  const completed = (route === "responses" || route === "chat.completions") &&
    streamTerminalType === "response.completed";
  const provider = completed ? requireNonEmptyString(parsed, "provider", lineNumber) : null;
  const model = completed ? requireNonEmptyString(parsed, "model", lineNumber) : null;
  if (usageObserved !== (usageTelemetryStatus !== "missing")) {
    return fail(lineNumber, "terminal event has inconsistent usage_observed and usage_telemetry_status fields");
  }
  if (completed && usageTelemetryStatus === "reported" && cachedInputTokens === null) {
    return fail(lineNumber, "reported completed inference event is missing cached_input_tokens");
  }

  return {
    request_id: requestId,
    route,
    status,
    stream_terminal_type: streamTerminalType,
    cached_input_tokens: cachedInputTokens,
    cache_write_input_tokens: cacheWriteInputTokens,
    usage_observed: usageObserved,
    usage_telemetry_status: usageTelemetryStatus,
    release,
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

const toSortedCounts = <Key extends string>(counts: Map<Key, number>): Record<Key, number> =>
  Object.fromEntries(
    [...counts.entries()].sort(([left], [right]) => left.localeCompare(right)),
  ) as Record<Key, number>;

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
  #cacheReadInputTokens = createCacheTokenSummary();
  #cacheWriteInputTokens = createCacheTokenSummary();
  #cohorts = new Map<string, MutableCohort>();

  addLine(line: string, lineNumber: number): void {
    const event = parseTerminalEvent(line, lineNumber);
    if (event === null) return;

    if (this.#seenRequestIds.has(event.request_id)) {
      fail(lineNumber, "duplicate request_terminal event");
    }
    this.#seenRequestIds.add(event.request_id);
    this.#terminalEvents = addSafely(this.#terminalEvents, 1, lineNumber);
    increment(this.#statusTotals, String(event.status), lineNumber);

    if (this.#release === null) this.#release = event.release;
    else if (!sameRelease(this.#release, event.release)) {
      fail(lineNumber, "terminal event release identity differs from an earlier terminal event");
    }

    const completed = event.provider !== null && event.model !== null;
    if (!completed) return;

    this.#completedInference = addSafely(this.#completedInference, 1, lineNumber);
    increment(this.#completedStatusTotals, String(event.status), lineNumber);
    increment(this.#usageTelemetryStatusTotals, event.usage_telemetry_status, lineNumber);
    if (event.usage_telemetry_status === "reported") {
      this.#reportedCompleted = addSafely(this.#reportedCompleted, 1, lineNumber);
    }
    addCacheToken(this.#cacheReadInputTokens, event.cached_input_tokens, lineNumber);
    addCacheToken(this.#cacheWriteInputTokens, event.cache_write_input_tokens, lineNumber);

    const key = JSON.stringify([event.provider, event.model, event.route]);
    let cohort = this.#cohorts.get(key);
    if (!cohort) {
      cohort = {
        provider: event.provider,
        model: event.model,
        route: event.route,
        completed_inference: 0,
        status_totals: new Map(),
        usage_telemetry_status_totals: new Map(),
        cache_read_input_tokens: createCacheTokenSummary(),
        cache_write_input_tokens: createCacheTokenSummary(),
        reported: 0,
      };
      this.#cohorts.set(key, cohort);
    }
    cohort.completed_inference = addSafely(cohort.completed_inference, 1, lineNumber);
    increment(cohort.status_totals, String(event.status), lineNumber);
    increment(cohort.usage_telemetry_status_totals, event.usage_telemetry_status, lineNumber);
    if (event.usage_telemetry_status === "reported") {
      cohort.reported = addSafely(cohort.reported, 1, lineNumber);
    }
    addCacheToken(cohort.cache_read_input_tokens, event.cached_input_tokens, lineNumber);
    addCacheToken(cohort.cache_write_input_tokens, event.cache_write_input_tokens, lineNumber);
  }

  finish(): Stage0CacheTelemetryReport {
    if (this.#terminalEvents === 0 || this.#release === null) {
      throw new Stage0CacheTelemetryGateError("no request_terminal events were found on stdin");
    }

    const coverage = reportedOverCompleted(this.#reportedCompleted, this.#completedInference);
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
          model: cohort.model,
          route: cohort.route,
          completed_inference: cohort.completed_inference,
          status_totals: toSortedCounts(cohort.status_totals),
          usage_telemetry_status_totals: toSortedCounts(cohort.usage_telemetry_status_totals),
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
    const passingCohorts = cohorts.filter((cohort) => cohort.completed_1k_gate.passed).length;
    const coveragePassingCohorts = cohorts.filter((cohort) => cohort.reported_coverage_99_5_gate.passed).length;

    return {
      version: 1,
      release: this.#release,
      terminal_events: this.#terminalEvents,
      status_totals: toSortedCounts(this.#statusTotals),
      completed_inference: this.#completedInference,
      completed_status_totals: toSortedCounts(this.#completedStatusTotals),
      usage_telemetry_status_totals: toSortedCounts(this.#usageTelemetryStatusTotals),
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
