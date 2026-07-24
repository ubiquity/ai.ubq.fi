import {
  buildCodexRequest,
  CodexError,
  type CodexModelsSnapshot,
  fetchCodexResponses,
  getCodexModelsSnapshotDefaultModel,
  loadCodexModelsSnapshot,
  loadFullCodexModelsSnapshot,
} from "./codex.ts";
import { getCatalogClientVersion, handleCodexCatalogModels } from "./codex_catalog.ts";
import { DEFAULT_REASONING_EFFORT, normalizeReasoningEffort, type ReasoningEffort } from "./defaults.ts";
import { json, openaiError } from "./http.ts";
import { createInferenceSignal } from "./inference_deadline.ts";
import { getKv } from "./kv.ts";
import { loadRuntimeConfig } from "./runtime_config.ts";
import { CHAT_COMPLETIONS_REQUEST_KEYS, EMBEDDINGS_REQUEST_KEYS, RESPONSES_REQUEST_KEYS } from "./openai_schema.ts";
import { readJsonBody } from "./request.ts";
import {
  type PreflightedResponsesStream,
  preflightResponsesStream,
  proxyResponsesStreamIterator,
  readResponsesStream,
  ResponsesStreamError,
  type ResponsesStreamEvent,
} from "./responses_stream.ts";
import {
  type PaidFallbackReservation,
  recordYunwuAmbiguousFailure,
  recordYunwuTerminal,
  recordYunwuUndispatchedCancellation,
  recordYunwuUpstreamResponse,
  reservePaidFallback,
} from "./paid_fallback.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type {
  ChatCompletionRequest,
  MessageContentItem,
  ResponseInputItem,
  ResponseMessageItem,
  ResponsesRequest,
} from "./types.ts";
import { fetchYunwuResponses, YunwuError } from "./yunwu.ts";

const getDefaultModel = async (): Promise<string | null> => {
  const runtime = await loadRuntimeConfig();
  return runtime?.default_model ?? getCodexModelsSnapshotDefaultModel(runtime?.codex_models ?? null);
};

const inferenceSignal = (request: Request): AbortSignal => createInferenceSignal(request.signal);

const defaultModelUnavailableError = (): Response =>
  openaiError(
    503,
    "Default model is unavailable: no configured default model or Codex model snapshot.",
    "server_error",
  );

const getDefaultReasoningEffort = async (): Promise<ReasoningEffort> => {
  return (await loadRuntimeConfig())?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
};

type UsageContext = Readonly<{
  keyId: string | null;
  kernelRepo: { owner: string; repo: string } | null;
  kernelOrg: { owner: string } | null;
  paidFallbackEnabled?: boolean;
  idempotencyPrincipal?: string | null;
  requestId?: string;
  startedAtMs?: number;
  responseTelemetry?: ResponseTelemetryState;
}>;

type UpstreamProvider = "chatgpt_codex" | "yunwu";
export type InferenceFallbackReason = "primary_429";

export type ResponseTelemetry = Readonly<{
  provider: string;
  fallbackReason: InferenceFallbackReason | null;
  model: string | null;
  reasoning: string | null;
  inputTokens: number | null;
  outputTokens: number | null;
  quotaUsedPercent: number | null | undefined;
  completed: boolean;
  streamTerminalType: ResponseStreamTerminalType | null;
}>;

export type ResponseStreamTerminalType =
  | "response.completed"
  | "response.failed"
  | "response.incomplete"
  | "error"
  | "eof"
  | "cancelled"
  | "deadline";

type ResponseTelemetryState = {
  provider: string | null;
  fallbackReason: InferenceFallbackReason | null;
  model: string | null;
  reasoning: string | null;
  inputTokens: number | null;
  outputTokens: number | null;
  quotaUsedPercent: number | null | undefined;
  completed: boolean;
  streamTerminalType: ResponseStreamTerminalType | null;
};

const responseTelemetry = new WeakMap<Response, ResponseTelemetryState>();

const createResponseTelemetryState = (): ResponseTelemetryState => ({
  provider: null,
  fallbackReason: null,
  model: null,
  reasoning: null,
  inputTokens: null,
  outputTokens: null,
  quotaUsedPercent: undefined,
  completed: false,
  streamTerminalType: null,
});

const withResponseTelemetryContext = (
  context: UsageContext | undefined,
  state: ResponseTelemetryState,
): UsageContext => ({
  keyId: context?.keyId ?? null,
  kernelRepo: context?.kernelRepo ?? null,
  kernelOrg: context?.kernelOrg ?? null,
  paidFallbackEnabled: context?.paidFallbackEnabled,
  idempotencyPrincipal: context?.idempotencyPrincipal,
  requestId: context?.requestId,
  startedAtMs: context?.startedAtMs,
  responseTelemetry: state,
});

const attachResponseTelemetry = (response: Response, state: ResponseTelemetryState): Response => {
  state.provider ??= response.headers.get("x-uos-upstream") || "gateway";
  responseTelemetry.set(response, state);
  return response;
};

export const getResponseTelemetry = (response: Response): ResponseTelemetry | null => {
  const state = responseTelemetry.get(response);
  if (!state) return null;
  return {
    provider: state.provider ?? (response.headers.get("x-uos-upstream") || "gateway"),
    fallbackReason: state.fallbackReason,
    model: state.model,
    reasoning: state.reasoning,
    inputTokens: state.inputTokens,
    outputTokens: state.outputTokens,
    quotaUsedPercent: state.quotaUsedPercent,
    completed: state.completed,
    streamTerminalType: state.streamTerminalType,
  };
};

const runWithResponseTelemetry = async (
  context: UsageContext | undefined,
  run: (context: UsageContext) => Promise<Response>,
): Promise<Response> => {
  const state = createResponseTelemetryState();
  const response = await run(withResponseTelemetryContext(context, state));
  return attachResponseTelemetry(response, state);
};

type RoutedResponsesUpstream = Readonly<{
  response: Response;
  provider: UpstreamProvider;
  paidFallback: PaidFallbackReservation | null;
  gatewayResponse: boolean;
  fallbackReason: InferenceFallbackReason | null;
}>;

type UsageTokens = Readonly<{
  inputTokens: number;
  outputTokens: number;
  totalTokens: number;
}>;

const normalizeTokenCount = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const count = Math.trunc(value);
  if (count < 0) return null;
  return count;
};

const extractUsageTokens = (value: unknown): UsageTokens | null => {
  if (!isRecord(value)) return null;
  const inputTokens = normalizeTokenCount(value.input_tokens);
  const outputTokens = normalizeTokenCount(value.output_tokens);
  const totalTokens = normalizeTokenCount(value.total_tokens);
  if (inputTokens === null || outputTokens === null || totalTokens === null) return null;
  return { inputTokens, outputTokens, totalTokens };
};

const recordRequestUsage = (
  context: UsageContext | undefined,
  details: { model: string; route: string; stream: boolean; reasoning: string | null },
): Promise<void> => {
  if (context?.responseTelemetry) {
    context.responseTelemetry.model = details.model;
    context.responseTelemetry.reasoning = details.reasoning;
  }
  return Promise.resolve();
};

const recordCompletionUsage = (
  context: UsageContext | undefined,
  usage: UsageTokens | null,
): Promise<void> => {
  if (context?.responseTelemetry) {
    context.responseTelemetry.inputTokens = usage?.inputTokens ?? null;
    context.responseTelemetry.outputTokens = usage?.outputTokens ?? null;
    context.responseTelemetry.completed = true;
  }
  return Promise.resolve();
};

const recordErrorUsage = (_context: UsageContext | undefined): Promise<void> => Promise.resolve();

const recordStreamTerminalType = (
  context: UsageContext | undefined,
  terminalType: ResponseStreamTerminalType,
): void => {
  if (context?.responseTelemetry) context.responseTelemetry.streamTerminalType = terminalType;
};

const classifyStreamFailure = (
  error: unknown,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "premature_eof") return "eof";
  return "error";
};

const formatErrorSnippet = (error: unknown, maxLen = 280): string => {
  const raw = error instanceof Error ? error.message : String(error);
  const trimmed = raw.trim();
  if (!trimmed) return "";
  if (trimmed.length <= maxLen) return trimmed;
  return `${trimmed.slice(0, maxLen)}...`;
};

const withUpstreamProviderHeader = (response: Response, provider: string | null | undefined): Response => {
  if (!provider) return response;
  const headers = new Headers(response.headers);
  headers.set("x-uos-upstream", provider);
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const toCodexErrorResponse = (error: unknown, provider?: string | null): Response => {
  let response: Response;
  if (error instanceof CodexError) {
    response = openaiError(error.status, error.message, error.code);
  } else {
    const detail = formatErrorSnippet(error);
    const message = detail ? `Codex upstream request failed: ${detail}` : "Codex upstream request failed.";
    response = openaiError(502, message, "codex_upstream_unreachable");
  }
  return withUpstreamProviderHeader(response, provider);
};

type UpstreamErrorDetails = Readonly<{
  message: string;
  type?: string;
  code?: string;
  param?: string | null;
}>;

const getJsonString = (value: unknown, key: string): string | null => {
  if (!isRecord(value)) return null;
  const stringValue = getString(value[key]);
  return stringValue?.trim() || null;
};

const parseUpstreamErrorDetails = (text: string, statusText: string): UpstreamErrorDetails => {
  const trimmed = text.trim();
  if (trimmed) {
    try {
      const parsed = JSON.parse(trimmed) as unknown;
      if (isRecord(parsed)) {
        const error = isRecord(parsed.error) ? parsed.error : null;
        const message = getJsonString(error, "message") ?? getJsonString(parsed, "detail") ??
          getJsonString(parsed, "message");
        if (message) {
          return {
            message,
            type: getJsonString(error, "type") ?? getJsonString(parsed, "type") ?? undefined,
            code: getJsonString(error, "code") ?? getJsonString(parsed, "code") ?? undefined,
            param: Object.prototype.hasOwnProperty.call(error ?? {}, "param")
              ? getString(error?.param) ?? null
              : undefined,
          };
        }
      }
    } catch {
      // Treat non-JSON upstream bodies as plain text below.
    }
  }

  const snippet = trimmed ? formatErrorSnippet(trimmed) : "";
  return { message: snippet || statusText || "Upstream request failed." };
};

const upstreamStatusToErrorType = (status: number, upstreamType?: string): string => {
  if (upstreamType) return upstreamType;
  return status >= 500 ? "server_error" : "invalid_request_error";
};

const toOpenAiUpstreamErrorResponse = async (
  upstream: Response,
  provider: UpstreamProvider = "chatgpt_codex",
): Promise<Response> => {
  const text = await upstream.text().catch(() => "");
  const details = parseUpstreamErrorDetails(text, upstream.statusText);
  let status = upstream.status >= 400 && upstream.status <= 599 ? upstream.status : 502;
  let code = details.code ?? "upstream_error";
  if (provider === "yunwu") {
    if (upstream.status === 401 || upstream.status === 403) {
      status = 502;
      code = "yunwu_upstream_auth_error";
    } else if (upstream.status === 429 || upstream.status >= 500) {
      status = 503;
      code = "yunwu_upstream_unavailable";
    }
  }
  const headers: Record<string, string> = { "x-uos-upstream": provider };
  const retryAfter = upstream.headers.get("Retry-After");
  if (retryAfter) headers["Retry-After"] = retryAfter;
  const options: { type?: string; param?: string | null; headers: HeadersInit } = {
    type: upstreamStatusToErrorType(status, details.type),
    headers,
  };
  if (Object.prototype.hasOwnProperty.call(details, "param")) options.param = details.param ?? null;
  return openaiError(status, details.message, code, options);
};

const cancelResponseBody = async (response: Response): Promise<void> => {
  try {
    await response.body?.cancel();
  } catch {
    // The response may already be closed.
  }
};

const warnPaidFallbackBookkeepingFailure = (operation: string, error: unknown): void => {
  console.warn(
    `[ai.ubq.fi] Paid fallback ${operation} failed; leaving the reservation pending:`,
    error instanceof Error ? error.message : String(error),
  );
};

const bestEffortPaidFallbackBookkeeping = async (
  operation: string,
  run: () => Promise<unknown>,
): Promise<void> => {
  try {
    await run();
  } catch (error) {
    warnPaidFallbackBookkeepingFailure(operation, error);
  }
};

type YunwuTransportLifecycle = Readonly<{
  terminal: (eventType: string) => void;
  ambiguous: () => void;
  cancelled: () => void;
}>;

const createYunwuTransportLifecycle = (
  reservation: PaidFallbackReservation | null,
): YunwuTransportLifecycle => {
  let recorded = false;
  const schedule = (
    operation: string,
    run: (reservation: PaidFallbackReservation) => Promise<void>,
  ): void => {
    if (!reservation || recorded) return;
    recorded = true;
    void bestEffortPaidFallbackBookkeeping(operation, () => run(reservation));
  };
  return {
    terminal: (eventType) => {
      const terminalState = eventType === "response.completed"
        ? "completed"
        : eventType === "response.failed" || eventType === "error"
        ? "failed"
        : eventType === "response.incomplete"
        ? "incomplete"
        : null;
      if (!terminalState) return;
      schedule(
        "terminal reconciliation",
        (activeReservation) => recordYunwuTerminal(activeReservation, terminalState),
      );
    },
    ambiguous: () =>
      schedule(
        "ambiguous failure recording",
        (activeReservation) => recordYunwuAmbiguousFailure(activeReservation),
      ),
    cancelled: () =>
      schedule(
        "dispatched cancellation recording",
        (activeReservation) => recordYunwuTerminal(activeReservation, "cancelled"),
      ),
  };
};

const fetchResponsesWithPaidFallback = async (
  body: Record<string, unknown>,
  options: Readonly<{
    model: string;
    route: "chat.completions" | "responses";
    stream: boolean;
    reasoning: string | null;
    usageContext?: UsageContext;
    clientVersion?: string | null;
    signal?: AbortSignal;
  }>,
): Promise<RoutedResponsesUpstream> => {
  const telemetry = options.usageContext?.responseTelemetry;
  if (telemetry) telemetry.provider = "chatgpt_codex";
  const primary = await fetchCodexResponses(body, {
    clientVersion: options.clientVersion,
    signal: options.signal,
  });
  const keyId = options.usageContext?.keyId;
  const requestId = options.usageContext?.requestId;
  const createdAtMs = options.usageContext?.startedAtMs;
  const fallbackReason: InferenceFallbackReason | null = primary.status === 429 ? "primary_429" : null;
  if (telemetry) telemetry.fallbackReason = fallbackReason;
  if (
    primary.status !== 429 ||
    options.usageContext?.paidFallbackEnabled === false ||
    !keyId ||
    !requestId ||
    createdAtMs === undefined
  ) {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse: false,
      fallbackReason,
    };
  }
  if (options.signal?.aborted) {
    if (primary) await cancelResponseBody(primary);
    throw options.signal.reason instanceof Error
      ? options.signal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }

  const reservationInput = {
    keyId,
    requestId,
    createdAtMs,
    model: options.model,
    route: options.route,
    path: options.route === "responses" ? "/v1/responses" : "/v1/chat/completions",
    stream: options.stream,
    reasoning: options.reasoning,
    reason: "primary_429",
  } as const;
  let decision: Awaited<ReturnType<typeof reservePaidFallback>>;
  try {
    decision = await reservePaidFallback(reservationInput);
  } catch (error) {
    warnPaidFallbackBookkeepingFailure("admission", error);
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse: false,
      fallbackReason: reservationInput.reason,
    };
  }
  if (decision.kind === "skip") {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse: false,
      fallbackReason: reservationInput.reason,
    };
  }
  if (decision.kind === "blocked") {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse: false,
      fallbackReason: reservationInput.reason,
    };
  }

  await cancelResponseBody(primary);
  if (options.signal?.aborted) {
    await bestEffortPaidFallbackBookkeeping(
      "undispatched cancellation recording",
      () => recordYunwuUndispatchedCancellation(decision.reservation),
    );
    throw options.signal.reason instanceof Error
      ? options.signal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }
  if (telemetry) {
    telemetry.provider = "yunwu";
    telemetry.quotaUsedPercent = decision.reservation.quota_used_percent;
  }
  let result: Awaited<ReturnType<typeof fetchYunwuResponses>>;
  try {
    result = await fetchYunwuResponses(body, { signal: options.signal });
  } catch (error) {
    await bestEffortPaidFallbackBookkeeping(
      "ambiguous failure recording",
      () => recordYunwuAmbiguousFailure(decision.reservation),
    );
    if (error instanceof YunwuError) {
      return {
        response: openaiError(error.status, error.message, error.code, {
          type: "server_error",
          headers: { "x-uos-upstream": "yunwu" },
        }),
        provider: "yunwu",
        paidFallback: decision.reservation,
        gatewayResponse: true,
        fallbackReason: reservationInput.reason,
      };
    }
    throw error;
  }
  await bestEffortPaidFallbackBookkeeping(
    "upstream response recording",
    () => recordYunwuUpstreamResponse(decision.reservation, result.response, result.request_id),
  );
  return {
    response: result.response,
    provider: "yunwu",
    paidFallback: decision.reservation,
    gatewayResponse: false,
    fallbackReason: reservationInput.reason,
  };
};

type CodexModelReasoning = Readonly<{
  levels: ReasoningEffort[];
  defaultLevel: ReasoningEffort | null;
  wireEfforts: ReadonlyMap<ReasoningEffort, ReasoningEffort>;
}>;

type CodexModelMetadata = Readonly<{
  snapshot: CodexModelsSnapshot | null;
  record: Record<string, unknown> | null;
  reasoning: CodexModelReasoning;
}>;

const modelIdFromSnapshotRecord = (model: Record<string, unknown>): string | null => {
  const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
  return id?.trim() || null;
};

const findSnapshotModelRecord = (
  snapshot: CodexModelsSnapshot | null,
  model: string,
): Record<string, unknown> | null => {
  const target = model.trim();
  if (!target) return null;
  if (!snapshot || !Array.isArray(snapshot.models)) return null;
  return snapshot.models.find((entry) => {
    if (!isRecord(entry)) return false;
    return modelIdFromSnapshotRecord(entry) === target;
  }) ?? null;
};

const normalizeSnapshotReasoningEffort = (value: unknown): ReasoningEffort | null =>
  value === null ? "none" : normalizeReasoningEffort(value);

const extractSnapshotReasoningLevels = (model: Record<string, unknown> | null): ReasoningEffort[] => {
  const raw = Array.isArray(model?.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
  const levels = raw
    .map((entry) => {
      if (entry === null || typeof entry === "string") return normalizeSnapshotReasoningEffort(entry);
      if (isRecord(entry)) return normalizeSnapshotReasoningEffort(entry.effort);
      return null;
    })
    .filter((entry): entry is ReasoningEffort => Boolean(entry));
  return Array.from(new Set(levels));
};

const extractSnapshotReasoningEffortWireMap = (
  model: Record<string, unknown> | null,
): ReadonlyMap<ReasoningEffort, ReasoningEffort> => {
  const raw = model?.reasoning_effort_wire_map;
  if (!isRecord(raw)) return new Map();
  const entries = Object.entries(raw)
    .map(([effort, wireEffort]) => [normalizeReasoningEffort(effort), normalizeReasoningEffort(wireEffort)] as const)
    .filter((entry): entry is readonly [ReasoningEffort, ReasoningEffort] => entry[0] !== null && entry[1] !== null);
  return new Map(entries);
};

const getCodexModelReasoning = (record: Record<string, unknown> | null): CodexModelReasoning => {
  const defaultLevel = normalizeSnapshotReasoningEffort(record?.default_reasoning_level);
  const catalogLevels = extractSnapshotReasoningLevels(record);
  const levels = catalogLevels.includes("none") ? catalogLevels : ["none", ...catalogLevels];
  return {
    levels: defaultLevel && !levels.includes(defaultLevel) ? [...levels, defaultLevel] : levels,
    defaultLevel,
    wireEfforts: extractSnapshotReasoningEffortWireMap(record),
  };
};

const getCodexModelMetadata = async (model: string): Promise<CodexModelMetadata> => {
  const snapshot = await loadCodexModelsSnapshot();
  const record = findSnapshotModelRecord(snapshot, model);
  return { snapshot, record, reasoning: getCodexModelReasoning(record) };
};

const validateCodexModelAvailable = (model: string, metadata: CodexModelMetadata): Response | null => {
  if (!metadata.snapshot?.models?.length || metadata.record) return null;
  return openaiError(
    404,
    `The model '${model}' does not exist or is not available through this gateway. Use /v1/models for supported models.`,
    "model_not_found",
    { param: "model" },
  );
};

const resolveDefaultReasoningLabel = (
  modelReasoning: CodexModelReasoning,
  defaultEffort: ReasoningEffort,
): ReasoningEffort => {
  if (defaultEffort === "none") return "none";
  if (modelReasoning.levels.includes(defaultEffort)) return defaultEffort;
  if (modelReasoning.defaultLevel === "none") return "none";
  if (modelReasoning.defaultLevel && modelReasoning.levels.includes(modelReasoning.defaultLevel)) {
    return modelReasoning.defaultLevel;
  }
  return modelReasoning.levels[0] ?? "none";
};

const resolveReasoningLabelFromEffort = (
  effort: ReasoningEffort | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (effort === undefined) return defaultLabel;
  return effort;
};

const resolveReasoningLabelFromParam = (
  reasoning: Record<string, unknown> | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (reasoning === undefined) return defaultLabel;
  if (!isRecord(reasoning)) return defaultLabel;
  if ("effort" in reasoning) {
    const effort = normalizeReasoningEffort(reasoning.effort);
    if (effort) return effort;
  }
  return defaultLabel;
};

const extractReasoningParamEffort = (
  reasoning: Record<string, unknown> | undefined,
): ReasoningEffort | undefined => {
  if (reasoning === undefined) return undefined;
  if (!Object.prototype.hasOwnProperty.call(reasoning, "effort")) return undefined;
  return normalizeReasoningEffort(reasoning.effort) ?? undefined;
};

const reasoningEffortForCodexRequest = (
  effort: ReasoningEffort,
  modelReasoning: CodexModelReasoning,
): ReasoningEffort => modelReasoning.wireEfforts.get(effort) ?? effort;

const normalizeReasoningParamForCodex = (
  reasoning: Record<string, unknown> | undefined,
  modelReasoning: CodexModelReasoning,
): Record<string, unknown> | undefined => {
  if (reasoning === undefined) return undefined;
  const effort = extractReasoningParamEffort(reasoning);
  if (effort === undefined) return reasoning;
  return { ...reasoning, effort: reasoningEffortForCodexRequest(effort, modelReasoning) };
};

const UOS_WARNING_HEADER = "x-uos-warning";
const TEMPERATURE_IGNORED_WARNING = "temperature_ignored";
const MAX_OUTPUT_TOKENS_IGNORED_WARNING = "max_output_tokens_ignored";

const WARNING_KEY_MAP = new Map<string, string>([
  ["temperature", TEMPERATURE_IGNORED_WARNING],
  ["max_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
  ["max_completion_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
  ["max_output_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
]);

const buildIgnoredWarnings = (record: Record<string, unknown>, usedKeys: ReadonlySet<string>): string[] => {
  const warnings = new Set<string>();
  for (const key of Object.keys(record)) {
    if (usedKeys.has(key)) continue;
    const mapped = WARNING_KEY_MAP.get(key) ?? `${key}_ignored`;
    warnings.add(mapped);
  }
  return Array.from(warnings);
};

type PassthroughToolSchemaKey =
  | "tools"
  | "tool_choice"
  | "parallel_tool_calls"
  | "prompt_cache_key"
  | "text"
  | "include"
  | "context_management";

const normalizeCodexToolChoice = (value: unknown): unknown => {
  if (!isRecord(value)) return value;
  if (getString(value.type) !== "function") return value;

  const normalized: Record<string, unknown> = { ...value };
  const topLevelName = getString(normalized.name);
  const fn = isRecord(value.function) ? value.function : null;
  if (!fn && !topLevelName) return value;

  if (!topLevelName) {
    const functionName = getString(fn?.name);
    if (!functionName) return value;
    normalized.name = functionName;
  }

  delete normalized.function;
  return normalized;
};

const normalizeCodexTools = (value: unknown): unknown => {
  if (!Array.isArray(value)) return value;
  return value.map((tool) => {
    if (!isRecord(tool)) return tool;
    if (getString(tool.type) !== "function") return tool;
    const nestedFunction = isRecord(tool.function) ? tool.function : null;
    if (!nestedFunction) return tool;

    const normalized: Record<string, unknown> = { ...tool };
    const topLevelName = getString(normalized.name);
    const nestedName = getString(nestedFunction.name);
    if (!topLevelName && !nestedName) return tool;

    if (!topLevelName) {
      normalized.name = nestedName;
    }
    for (const [key, nestedValue] of Object.entries(nestedFunction)) {
      if (key in normalized) continue;
      normalized[key] = nestedValue;
    }
    delete normalized.function;
    return normalized;
  });
};

const normalizePassthroughForCodex = (key: PassthroughToolSchemaKey, value: unknown): unknown => {
  if (key === "tools") return normalizeCodexTools(value);
  if (key === "tool_choice") return normalizeCodexToolChoice(value);
  return value;
};

const applyPassthroughToCodexRequest = (
  codexBody: Record<string, unknown>,
  rawRecord: Record<string, unknown>,
  keys: readonly PassthroughToolSchemaKey[],
): void => {
  for (const key of keys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = normalizePassthroughForCodex(key, rawRecord[key]);
    }
  }
};

const withUosWarning = (response: Response, warnings: string[]): Response => {
  if (!warnings.length) return response;
  const headers = new Headers(response.headers);
  headers.set(UOS_WARNING_HEADER, warnings.join(", "));
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};
const parseReasoningEffortField = (
  value: unknown,
  fieldName: string,
): { ok: true; value: ReasoningEffort | undefined } | { ok: false; message: string } => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (typeof value !== "string") {
    return { ok: false, message: `${fieldName} must be a string` };
  }
  const normalized = normalizeReasoningEffort(value);
  if (!normalized) return { ok: false, message: `${fieldName} must be a non-empty string` };
  return { ok: true, value: normalized };
};

const parseReasoningParam = (
  value: unknown,
): { ok: true; value: Record<string, unknown> | undefined } | { ok: false; message: string } => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (!isRecord(value)) return { ok: false, message: "reasoning must be an object" };
  const normalized = { ...value };
  if ("effort" in normalized) {
    const effort = parseReasoningEffortField(normalized.effort, "reasoning.effort");
    if (!effort.ok) return effort;
    if (effort.value === undefined) delete normalized.effort;
    else normalized.effort = effort.value;
  }
  if ("summary" in normalized) {
    const summary = normalized.summary;
    if (summary === undefined || summary === null) delete normalized.summary;
    else if (typeof summary !== "string") {
      return { ok: false, message: "reasoning.summary must be a string" };
    }
  }
  if ("generate_summary" in normalized) {
    const generateSummary = normalized.generate_summary;
    if (generateSummary === undefined || generateSummary === null) delete normalized.generate_summary;
    else if (typeof generateSummary !== "string") {
      return { ok: false, message: "reasoning.generate_summary must be a string" };
    }
  }

  return { ok: true, value: Object.keys(normalized).length ? normalized : undefined };
};

const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set(CHAT_COMPLETIONS_REQUEST_KEYS);
const RESPONSES_ALLOWED_KEYS = new Set(RESPONSES_REQUEST_KEYS);
const CODEX_RESPONSES_EXTENSION_KEYS = new Set(["client_metadata"]);
const EMBEDDINGS_ALLOWED_KEYS = new Set(EMBEDDINGS_REQUEST_KEYS);

const findUnknownKey = (
  record: Record<string, unknown>,
  allowed: ReadonlySet<string>,
  extensions?: ReadonlySet<string>,
): string | null => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key) && !extensions?.has(key)) return key;
  }
  return null;
};

type EmbeddingsEncodingFormat = "float" | "base64";
type VoyageEmbeddingsInputType = "query" | "document";
type VoyageEmbeddingsDimension = 256 | 512 | 1024 | 2048;
type VoyageEmbeddingsOutputDtype = "float";

type ResolvedEmbeddingsProfile = Readonly<{
  upstream: "voyage";
  upstream_model: "voyage-4-large";
  input_type: VoyageEmbeddingsInputType;
  dimensions: VoyageEmbeddingsDimension;
  output_dtype: VoyageEmbeddingsOutputDtype;
  encoding_format: EmbeddingsEncodingFormat;
  truncation: boolean;
  cache_profile_key: string;
}>;

type ParsedEmbeddingsRequest = Readonly<{
  model: string;
  inputs: string[];
  total_chars: number;
  profile: ResolvedEmbeddingsProfile;
}>;

type EmbeddingsParseResult =
  | Readonly<{ ok: true; value: ParsedEmbeddingsRequest }>
  | Readonly<{ ok: false; response: Response }>;

type VoyageRateLimitState = Readonly<{
  window_start_ms: number;
  requests: number;
  tokens: number;
}>;

const EMBEDDINGS_MAX_INPUTS_PER_REQUEST = 128;
const EMBEDDINGS_MAX_CHARS_PER_INPUT = 20_000;
const EMBEDDINGS_MAX_TOTAL_CHARS = 100_000;
const EMBEDDINGS_TIMEOUT_MS = 20_000;
// KV cache is best-effort and quota-driven: we cache embeddings until KV rejects
// writes (storage/quota), then evict the oldest entries (FIFO index) and retry.
// We do not track "last read" to keep writes minimal.
const EMBEDDINGS_CACHE_EVICT_BATCH = 512;
const EMBEDDINGS_CACHE_EVICT_MAX_BATCH = 8192;
const EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES = 4;
const EMBEDDINGS_JOB_TTL_MS = 24 * 60 * 60_000;
const EMBEDDINGS_JOB_LOCK_MS = 30_000;
const EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES = new Set([429, 500, 502, 503, 504]);
const EMBEDDINGS_IDEMPOTENCY_LEASE_MS = 60_000;
const EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS = 48_000;
// 128 inputs x 2,048 finite JSON numbers fit comfortably below this cap.
const EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS = 256;
const EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS = 7 * 24 * 60 * 60_000;
// Response chunks are published before their ledger record. Keep them for one
// extra day so every published ledger expires before the chunks it references;
// unpublished/orphaned generations are reclaimed by the same TTL.
const EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS = EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS + 24 * 60 * 60_000;
const EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS = 255;

const VOYAGE_EMBEDDINGS_URL = "https://api.voyageai.com/v1/embeddings";
const VOYAGE_EMBEDDINGS_MODEL = "voyage-4-large";
const VOYAGE_DEFAULT_DIMENSIONS: VoyageEmbeddingsDimension = 1024;
const VOYAGE_OUTPUT_DTYPE: VoyageEmbeddingsOutputDtype = "float";
const VOYAGE_SUPPORTED_DIMENSIONS = new Set<number>([256, 512, 1024, 2048]);
const UOS_EMBEDDINGS_ALLOWED_KEYS = new Set([
  "dimensions",
  "encoding_format",
  "input",
  "input_type",
  "model",
  "truncation",
]);
// Voyage free-tier throttles are tiny; we enforce conservative defaults to avoid 429s.
const VOYAGE_RATE_LIMIT_RPM = 3;
const VOYAGE_RATE_LIMIT_TPM = 10_000;
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const VOYAGE_API_KEY_KV_KEY: Deno.KvKey = ["uos_ai", "voyage_api_key"];

type EmbeddingsIdempotencyState = "reserved" | "dispatched" | "succeeded" | "indeterminate";

type EmbeddingsIdempotencyRecord = Readonly<{
  v: 1;
  fingerprint: string;
  state: EmbeddingsIdempotencyState;
  owner_request_id: string | null;
  created_at_ms: number;
  updated_at_ms: number;
  lease_until_ms: number | null;
  response_status: number | null;
  response_content_type: string | null;
  response_generation: string | null;
  response_chunk_count: number | null;
  response_sha256: string | null;
}>;

type EmbeddingsIdempotencyLease = Readonly<{
  kv: Deno.Kv;
  key: Deno.KvKey;
  responseKeyPrefix: Deno.KvKey;
  fingerprint: string;
  ownerRequestId: string;
}>;

type EmbeddingsIdempotencyAcquireResult =
  | Readonly<{ kind: "acquired"; lease: EmbeddingsIdempotencyLease }>
  | Readonly<{ kind: "replay"; response: Response }>
  | Readonly<{ kind: "error"; response: Response }>;

type EmbeddingsJobStatus = "queued" | "running" | "succeeded" | "failed";

type EmbeddingsJobRecord = Readonly<{
  id: string;
  status: EmbeddingsJobStatus;
  created_at_ms: number;
  updated_at_ms: number;
  model: string;
  cache_profile_key: string;
  upstream: "voyage";
  upstream_model: "voyage-4-large";
  input_type: VoyageEmbeddingsInputType;
  dimensions: VoyageEmbeddingsDimension;
  output_dtype: VoyageEmbeddingsOutputDtype;
  encoding_format: EmbeddingsEncodingFormat;
  truncation: boolean;
  input_hashes: string[];
  input_count: number;
  total_chars: number;
  usage_total_tokens: number;
  retry_after_seconds: number | null;
  locked_until_ms: number | null;
  error: { message: string; type: string; code?: string } | null;
}>;

type EmbeddingsJobInputRecord = Readonly<{
  v: 1;
  iv_b64: string;
  data_b64: string;
  created_at_ms: number;
}>;

type EmbeddingsJobLookupRecord = Readonly<{
  cache_profile_key: string;
}>;

const embeddingsJobKey = (tokenHash: string, cacheProfileKey: string, id: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  tokenHash,
  cacheProfileKey,
  id,
];
const embeddingsJobLookupKey = (tokenHash: string, id: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  "lookup",
  tokenHash,
  id,
];
const embeddingsJobInputKey = (
  tokenHash: string,
  cacheProfileKey: string,
  jobId: string,
  hash: string,
): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  "input",
  tokenHash,
  cacheProfileKey,
  jobId,
  hash,
];

const embeddingsCacheIndexKey = (
  cacheProfileKey: string,
  createdAtMs: number,
  hash: string,
): Deno.KvKey => ["embeddings", "v2", "cache_index", cacheProfileKey, createdAtMs, hash];
const embeddingsCacheGlobalIndexPrefix: Deno.KvKey = ["embeddings", "v2", "cache_index_global"];
const embeddingsCacheGlobalIndexKey = (
  createdAtMs: number,
  cacheProfileKey: string,
  hash: string,
): Deno.KvKey => [...embeddingsCacheGlobalIndexPrefix, createdAtMs, cacheProfileKey, hash];
const embeddingsCacheIndexByHashKey = (cacheProfileKey: string, hash: string): Deno.KvKey => [
  "embeddings",
  "v2",
  "cache_index_by_hash",
  cacheProfileKey,
  hash,
];
const embeddingsCacheKey = (cacheProfileKey: string, hash: string): Deno.KvKey => [
  "embeddings",
  "v2",
  "cache",
  cacheProfileKey,
  hash,
];

const embeddingsIdempotencyKey = (principalHash: string, idempotencyKeyHash: string): Deno.KvKey => [
  "embeddings",
  "idempotency",
  "v1",
  principalHash,
  idempotencyKeyHash,
];

const embeddingsIdempotencyResponseKeyPrefix = (
  principalHash: string,
  idempotencyKeyHash: string,
): Deno.KvKey => [
  "embeddings",
  "idempotency",
  "v1",
  "response",
  principalHash,
  idempotencyKeyHash,
];

const isEmbeddingsIdempotencyState = (value: unknown): value is EmbeddingsIdempotencyState =>
  value === "reserved" || value === "dispatched" || value === "succeeded" || value === "indeterminate";

const normalizeEmbeddingsIdempotencyRecord = (value: unknown): EmbeddingsIdempotencyRecord | null => {
  if (!isRecord(value) || value.v !== 1 || typeof value.fingerprint !== "string") return null;
  if (!isEmbeddingsIdempotencyState(value.state)) return null;
  if (value.owner_request_id !== null && typeof value.owner_request_id !== "string") return null;
  if (
    typeof value.created_at_ms !== "number" || !Number.isFinite(value.created_at_ms) ||
    typeof value.updated_at_ms !== "number" || !Number.isFinite(value.updated_at_ms)
  ) {
    return null;
  }
  if (
    value.lease_until_ms !== null &&
    (typeof value.lease_until_ms !== "number" || !Number.isFinite(value.lease_until_ms))
  ) {
    return null;
  }
  if (
    value.response_status !== null &&
    (typeof value.response_status !== "number" || !Number.isInteger(value.response_status))
  ) {
    return null;
  }
  if (value.response_content_type !== null && typeof value.response_content_type !== "string") return null;
  if (value.response_generation !== null && typeof value.response_generation !== "string") return null;
  if (
    value.response_chunk_count !== null &&
    (
      typeof value.response_chunk_count !== "number" ||
      !Number.isInteger(value.response_chunk_count) ||
      value.response_chunk_count < 1 ||
      value.response_chunk_count > EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS
    )
  ) {
    return null;
  }
  if (value.response_sha256 !== null && typeof value.response_sha256 !== "string") return null;

  return {
    v: 1,
    fingerprint: value.fingerprint,
    state: value.state,
    owner_request_id: value.owner_request_id,
    created_at_ms: Math.trunc(value.created_at_ms),
    updated_at_ms: Math.trunc(value.updated_at_ms),
    lease_until_ms: value.lease_until_ms === null ? null : Math.trunc(value.lease_until_ms),
    response_status: value.response_status === null ? null : Math.trunc(value.response_status),
    response_content_type: value.response_content_type,
    response_generation: value.response_generation,
    response_chunk_count: value.response_chunk_count === null ? null : Math.trunc(value.response_chunk_count),
    response_sha256: value.response_sha256,
  };
};

const embeddingsIdempotencyError = (
  status: 409 | 503,
  message: string,
  code:
    | "embedding_idempotency_conflict"
    | "embedding_idempotency_in_progress"
    | "embedding_idempotency_indeterminate"
    | "embedding_idempotency_unavailable",
  retryAfterSeconds?: number,
): Response =>
  openaiError(status, message, code, {
    type: status === 503 ? "server_error" : "idempotency_error",
    param: null,
    ...(retryAfterSeconds === undefined ? {} : { headers: { "Retry-After": String(retryAfterSeconds) } }),
  });

const embeddingsIdempotencyConflictResponse = (): Response =>
  embeddingsIdempotencyError(
    409,
    "Idempotency-Key was already used with a different embeddings request.",
    "embedding_idempotency_conflict",
  );

const embeddingsIdempotencyInProgressResponse = (): Response =>
  embeddingsIdempotencyError(
    409,
    "The embeddings request for this Idempotency-Key is still in progress.",
    "embedding_idempotency_in_progress",
    1,
  );

const embeddingsIdempotencyIndeterminateResponse = (): Response =>
  embeddingsIdempotencyError(
    409,
    "The embeddings request outcome is indeterminate and will not be dispatched again.",
    "embedding_idempotency_indeterminate",
  );

const embeddingsIdempotencyUnavailableResponse = (): Response =>
  embeddingsIdempotencyError(
    503,
    "Idempotent embeddings requests require durable KV storage.",
    "embedding_idempotency_unavailable",
  );

const hasAsciiControlCharacter = (value: string): boolean => {
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code <= 0x1f || code === 0x7f) return true;
  }
  return false;
};

const buildEmbeddingsIdempotencyFingerprint = async (
  profile: ResolvedEmbeddingsProfile,
  orderedInputHashes: string[],
): Promise<string> =>
  await sha256Hex(
    JSON.stringify([
      "uos-embeddings-idempotency-v1",
      profile.upstream,
      profile.upstream_model,
      profile.input_type,
      profile.dimensions,
      profile.output_dtype,
      profile.encoding_format,
      profile.truncation,
      orderedInputHashes,
    ]),
  );

const loadEmbeddingsIdempotencyResponse = async (
  lease: Omit<EmbeddingsIdempotencyLease, "ownerRequestId">,
  record: EmbeddingsIdempotencyRecord,
): Promise<Response | null> => {
  if (
    record.state !== "succeeded" ||
    record.response_status !== 200 ||
    !record.response_content_type ||
    !record.response_generation ||
    record.response_chunk_count === null ||
    !record.response_sha256
  ) {
    return null;
  }

  const chunks = await Promise.all(
    Array.from(
      { length: record.response_chunk_count },
      (_, index) =>
        lease.kv.get<string>([
          ...lease.responseKeyPrefix,
          record.response_generation!,
          index,
        ]),
    ),
  );
  if (chunks.some((entry) => typeof entry.value !== "string")) return null;
  const body = chunks.map((entry) => entry.value as string).join("");
  if (await sha256Hex(body) !== record.response_sha256) return null;
  return new Response(body, {
    status: record.response_status,
    headers: {
      "Content-Type": record.response_content_type,
      "x-uos-idempotency-replayed": "true",
    },
  });
};

const markEmbeddingsIdempotencyIndeterminate = async (
  lease: EmbeddingsIdempotencyLease,
): Promise<void> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (!record || record.fingerprint !== lease.fingerprint) return;
      if (record.state === "indeterminate" || record.state === "succeeded") return;
      const now = Date.now();
      const next: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "indeterminate",
        owner_request_id: null,
        updated_at_ms: now,
        lease_until_ms: null,
        response_status: null,
        response_content_type: null,
        response_generation: null,
        response_chunk_count: null,
        response_sha256: null,
      };
      const commit = await lease.kv.atomic()
        .check(entry)
        .set(lease.key, next, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency indeterminate-state write failed:", error);
  }
};

const acquireEmbeddingsIdempotencyLease = async (params: {
  kv: Deno.Kv;
  principal: string;
  idempotencyKey: string;
  fingerprint: string;
  requestId: string;
}): Promise<EmbeddingsIdempotencyAcquireResult> => {
  const [principalHash, idempotencyKeyHash] = await Promise.all([
    sha256Hex(`uos-embeddings-principal-v1:${params.principal}`),
    sha256Hex(`uos-embeddings-key-v1:${params.idempotencyKey}`),
  ]);
  const key = embeddingsIdempotencyKey(principalHash, idempotencyKeyHash);
  const responseKeyPrefix = embeddingsIdempotencyResponseKeyPrefix(principalHash, idempotencyKeyHash);
  const lease: EmbeddingsIdempotencyLease = {
    kv: params.kv,
    key,
    responseKeyPrefix,
    fingerprint: params.fingerprint,
    ownerRequestId: params.requestId,
  };

  try {
    for (let attempt = 0; attempt < 8; attempt += 1) {
      const entry = await params.kv.get<EmbeddingsIdempotencyRecord>(key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      const now = Date.now();

      if (entry.versionstamp === null) {
        const reserved: EmbeddingsIdempotencyRecord = {
          v: 1,
          fingerprint: params.fingerprint,
          state: "reserved",
          owner_request_id: params.requestId,
          created_at_ms: now,
          updated_at_ms: now,
          lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
          response_status: null,
          response_content_type: null,
          response_generation: null,
          response_chunk_count: null,
          response_sha256: null,
        };
        const commit = await params.kv.atomic()
          .check(entry)
          .set(key, reserved, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
          .commit();
        if (commit.ok) return { kind: "acquired", lease };
        continue;
      }

      if (!record) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
      if (record.fingerprint !== params.fingerprint) {
        return { kind: "error", response: embeddingsIdempotencyConflictResponse() };
      }

      if (record.state === "succeeded") {
        const replay = await loadEmbeddingsIdempotencyResponse(lease, record);
        if (replay) return { kind: "replay", response: replay };
        const indeterminate: EmbeddingsIdempotencyRecord = {
          ...record,
          state: "indeterminate",
          owner_request_id: null,
          updated_at_ms: now,
          lease_until_ms: null,
          response_status: null,
          response_content_type: null,
          response_generation: null,
          response_chunk_count: null,
          response_sha256: null,
        };
        const commit = await params.kv.atomic()
          .check(entry)
          .set(key, indeterminate, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
          .commit();
        if (commit.ok) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
        continue;
      }

      if (record.state === "indeterminate") {
        return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
      }

      if (record.lease_until_ms !== null && record.lease_until_ms > now) {
        return { kind: "error", response: embeddingsIdempotencyInProgressResponse() };
      }

      if (record.state === "dispatched") {
        const indeterminate: EmbeddingsIdempotencyRecord = {
          ...record,
          state: "indeterminate",
          owner_request_id: null,
          updated_at_ms: now,
          lease_until_ms: null,
          response_status: null,
          response_content_type: null,
          response_generation: null,
          response_chunk_count: null,
          response_sha256: null,
        };
        const commit = await params.kv.atomic()
          .check(entry)
          .set(key, indeterminate, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
          .commit();
        if (commit.ok) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
        continue;
      }

      const reserved: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "reserved",
        owner_request_id: params.requestId,
        updated_at_ms: now,
        lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
        response_status: null,
        response_content_type: null,
        response_generation: null,
        response_chunk_count: null,
        response_sha256: null,
      };
      const commit = await params.kv.atomic()
        .check(entry)
        .set(key, reserved, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return { kind: "acquired", lease };
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency reservation failed:", error);
  }

  return { kind: "error", response: embeddingsIdempotencyUnavailableResponse() };
};

const markEmbeddingsIdempotencyDispatched = async (
  lease: EmbeddingsIdempotencyLease,
): Promise<boolean> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (
        !record ||
        record.fingerprint !== lease.fingerprint ||
        record.state !== "reserved" ||
        record.owner_request_id !== lease.ownerRequestId
      ) {
        return false;
      }
      const now = Date.now();
      const dispatched: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "dispatched",
        updated_at_ms: now,
        lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
      };
      const commit = await lease.kv.atomic()
        .check(entry)
        .set(lease.key, dispatched, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency dispatch-state write failed:", error);
  }
  return false;
};

const releaseEmbeddingsIdempotencyReservation = async (
  lease: EmbeddingsIdempotencyLease,
  allowDispatched: boolean,
): Promise<boolean> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (!record || record.fingerprint !== lease.fingerprint) return false;
      if (record.owner_request_id !== lease.ownerRequestId) return false;
      if (record.state !== "reserved" && !(allowDispatched && record.state === "dispatched")) return false;
      const commit = await lease.kv.atomic().check(entry).delete(lease.key).commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency reservation release failed:", error);
  }
  return false;
};

const storeEmbeddingsIdempotencySuccess = async (
  lease: EmbeddingsIdempotencyLease,
  response: Response,
): Promise<boolean> => {
  try {
    const body = await response.clone().text();
    const chunks: string[] = [];
    for (let offset = 0; offset < body.length; offset += EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS) {
      chunks.push(body.slice(offset, offset + EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS));
    }
    if (!chunks.length) chunks.push("");
    if (chunks.length > EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS) return false;
    const responseGeneration = lease.ownerRequestId;
    for (let index = 0; index < chunks.length; index += 1) {
      await lease.kv.set(
        [...lease.responseKeyPrefix, responseGeneration, index],
        chunks[index]!,
        { expireIn: EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS },
      );
    }
    const bodyHash = await sha256Hex(body);

    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (
        !record ||
        record.fingerprint !== lease.fingerprint ||
        (record.state !== "reserved" && record.state !== "dispatched") ||
        record.owner_request_id !== lease.ownerRequestId
      ) {
        return false;
      }
      const now = Date.now();
      const succeeded: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "succeeded",
        owner_request_id: null,
        updated_at_ms: now,
        lease_until_ms: null,
        response_status: response.status,
        response_content_type: response.headers.get("Content-Type") ?? "application/json",
        response_generation: responseGeneration,
        response_chunk_count: chunks.length,
        response_sha256: bodyHash,
      };
      const commit = await lease.kv.atomic()
        .check(entry)
        .set(lease.key, succeeded, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency response write failed:", error);
  }
  return false;
};

const normalizeEmbeddingsCacheTimestampMs = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const ts = Math.trunc(value);
  if (ts < 0) return null;
  return ts;
};

type EmbeddingsCacheEvictResult = Readonly<{
  evicted_embeddings: number;
  deleted_stale_index_keys: number;
}>;

const isEmbeddingsCacheQuotaError = (error: unknown): boolean => {
  const name = typeof (error as { name?: unknown })?.name === "string" ? String((error as { name: string }).name) : "";
  const message = error instanceof Error ? error.message : typeof error === "string" ? error : "";
  const combined = `${name} ${message}`.toLowerCase();
  if (!combined) return false;
  return (
    combined.includes("quota") ||
    (combined.includes("insufficient") && combined.includes("storage")) ||
    (combined.includes("insufficient") && combined.includes("space")) ||
    combined.includes("no space") ||
    combined.includes("storage limit") ||
    (combined.includes("storage") && combined.includes("exceeded"))
  );
};

const writeEmbeddingsCacheEntry = async (
  kv: Deno.Kv,
  cacheProfileKey: string,
  hash: string,
  embedding: number[],
  createdAtMs: number,
): Promise<{ isNew: boolean }> => {
  const byHashKey = embeddingsCacheIndexByHashKey(cacheProfileKey, hash);
  const cacheKey = embeddingsCacheKey(cacheProfileKey, hash);

  // Concurrency-safe: if multiple requests try to cache the same hash, only one
  // will win the "create index" CAS; the others will reuse the winner's index.
  for (let attempt = 0; attempt < 5; attempt += 1) {
    const entry = await kv.get<number>(byHashKey);
    const existingCreatedAtMs = normalizeEmbeddingsCacheTimestampMs(entry.value);
    if (existingCreatedAtMs !== null) {
      const indexKey = embeddingsCacheIndexKey(cacheProfileKey, existingCreatedAtMs, hash);
      const updated = await kv.atomic()
        .check(entry)
        .set(cacheKey, { embedding, created_at: new Date(existingCreatedAtMs).toISOString() })
        .set(indexKey, 1)
        .set(embeddingsCacheGlobalIndexKey(existingCreatedAtMs, cacheProfileKey, hash), 1)
        .commit();
      if (updated.ok) return { isNew: false };
      continue;
    }

    const createdAtIso = new Date(createdAtMs).toISOString();
    const indexKey = embeddingsCacheIndexKey(cacheProfileKey, createdAtMs, hash);
    const created = await kv.atomic()
      .check(entry)
      .set(cacheKey, { embedding, created_at: createdAtIso })
      .set(indexKey, 1)
      .set(embeddingsCacheGlobalIndexKey(createdAtMs, cacheProfileKey, hash), 1)
      .set(byHashKey, createdAtMs)
      .commit();
    if (created.ok) return { isNew: true };
    // CAS failed: `byHashKey` was updated/created in between, or was evicted and
    // recreated concurrently. Retry to reuse the now-canonical pointer.
  }
  return { isNew: false };
};

const writeEmbeddingsCacheEntryBestEffort = async (
  kv: Deno.Kv,
  cacheProfileKey: string,
  hash: string,
  embedding: number[],
  createdAtMs: number,
  deadlineMs: number,
): Promise<{ isNew: boolean }> => {
  let evictBatch = EMBEDDINGS_CACHE_EVICT_BATCH;
  // Attempts = 1 (initial write) + max retries.
  for (let attempt = 0; attempt <= EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES; attempt += 1) {
    if (Date.now() >= deadlineMs) return { isNew: false };
    try {
      return await writeEmbeddingsCacheEntry(kv, cacheProfileKey, hash, embedding, createdAtMs);
    } catch (error) {
      if (!isEmbeddingsCacheQuotaError(error)) {
        console.warn("[ai.ubq.fi] embeddings_cache write failed:", error);
        return { isNew: false };
      }

      // KV rejected the write (likely storage quota). Evict the oldest entries
      // across every embedding profile so a newly introduced profile cannot be
      // starved by cache entries owned by another profile.
      try {
        const evicted = await evictOldestEmbeddingsCacheEntries(kv, evictBatch);
        console.warn(
          `[ai.ubq.fi] embeddings_cache quota eviction requesting_profile=${cacheProfileKey} scope=global evicted=${evicted.evicted_embeddings} stale_index_deleted=${evicted.deleted_stale_index_keys} batch=${evictBatch}`,
        );
        if (evicted.evicted_embeddings <= 0 && evicted.deleted_stale_index_keys <= 0) return { isNew: false };
      } catch (evictError) {
        console.warn("[ai.ubq.fi] embeddings_cache quota eviction failed:", evictError);
        return { isNew: false };
      }

      evictBatch = Math.min(EMBEDDINGS_CACHE_EVICT_MAX_BATCH, evictBatch * 2);
    }
  }
  return { isNew: false };
};

const evictOldestEmbeddingsCacheEntries = async (
  kv: Deno.Kv,
  count: number,
): Promise<EmbeddingsCacheEvictResult> => {
  const keys: Array<{
    globalIndexKey: Deno.KvKey;
    cacheProfileKey: string;
    createdAtMs: number;
    hash: string;
  }> = [];
  for await (const entry of kv.list({ prefix: embeddingsCacheGlobalIndexPrefix }, { limit: count })) {
    const key = entry.key;
    const hash = key.at(-1);
    const cacheProfileKey = key.at(-2);
    const createdAtMs = key.at(-3);
    if (typeof hash !== "string" || !hash) continue;
    if (typeof cacheProfileKey !== "string" || !cacheProfileKey) continue;
    if (typeof createdAtMs !== "number" || !Number.isFinite(createdAtMs)) continue;
    keys.push({
      globalIndexKey: key,
      cacheProfileKey,
      createdAtMs: Math.trunc(createdAtMs),
      hash,
    });
  }
  if (!keys.length) return { evicted_embeddings: 0, deleted_stale_index_keys: 0 };

  const byHashKeys = keys.map((item) => embeddingsCacheIndexByHashKey(item.cacheProfileKey, item.hash));
  const byHashEntries = await Promise.all(byHashKeys.map((key) => kv.get<number>(key)));

  let evictedEmbeddings = 0;
  let deletedStaleIndexKeys = 0;
  for (let i = 0; i < keys.length; i += 1) {
    const { globalIndexKey, cacheProfileKey, createdAtMs, hash } = keys[i]!;
    const pointerEntry = byHashEntries[i]!;
    const pointer = normalizeEmbeddingsCacheTimestampMs(pointerEntry.value);
    const cacheKey = embeddingsCacheKey(cacheProfileKey, hash);
    const profileIndexKey = embeddingsCacheIndexKey(cacheProfileKey, createdAtMs, hash);

    if (pointer !== null && pointer !== createdAtMs) {
      // Stale duplicate index keys for this hash; delete only the indexes.
      const deleted = await kv.atomic()
        .check(pointerEntry)
        .delete(globalIndexKey)
        .delete(profileIndexKey)
        .commit();
      if (deleted.ok) deletedStaleIndexKeys += 1;
      continue;
    }

    if (pointer === null) {
      // Missing pointer (legacy / partial state): only delete the embedding value
      // if it still matches the index timestamp to avoid deleting a newer cache
      // entry that happens to share the same hash.
      const valueEntry = await kv.get<{ created_at?: unknown }>(cacheKey);
      const value = valueEntry.value;
      const createdAtIso = isRecord(value) && typeof value.created_at === "string" ? value.created_at : null;
      const expectedIso = new Date(createdAtMs).toISOString();
      if (createdAtIso !== expectedIso) {
        const deleted = await kv.atomic()
          .check(pointerEntry)
          .delete(globalIndexKey)
          .delete(profileIndexKey)
          .commit();
        if (deleted.ok) deletedStaleIndexKeys += 1;
        continue;
      }

      const commit = await kv.atomic()
        .check(pointerEntry)
        .delete(globalIndexKey)
        .delete(profileIndexKey)
        .delete(cacheKey)
        .commit();
      if (commit.ok) evictedEmbeddings += 1;
      continue;
    }

    // Canonical pointer match: evict embedding + index + pointer as an atomic unit.
    const commit = await kv.atomic()
      .check(pointerEntry)
      .delete(globalIndexKey)
      .delete(profileIndexKey)
      .delete(cacheKey)
      .delete(embeddingsCacheIndexByHashKey(cacheProfileKey, hash))
      .commit();
    if (commit.ok) evictedEmbeddings += 1;
  }
  return { evicted_embeddings: evictedEmbeddings, deleted_stale_index_keys: deletedStaleIndexKeys };
};

const resolveEmbeddingsJobTokenSeed = (
  jobId: string,
  authToken: string | null,
  usageContext?: UsageContext,
): string => {
  // Prefer stable identities so queued jobs remain resolvable even if bearer tokens refresh/rotate.
  if (usageContext?.keyId) return `uos_api_key_id:${usageContext.keyId}`;
  if (usageContext?.kernelRepo) {
    return `uos_kernel_repo:${usageContext.kernelRepo.owner}/${usageContext.kernelRepo.repo}`;
  }
  if (authToken) return authToken;
  return jobId;
};

const TOKEN_ESTIMATOR = new TextEncoder();

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

const estimateTokens = (text: string): number => {
  if (!text) return 0;
  const bytes = TOKEN_ESTIMATOR.encode(text).byteLength;
  return Math.ceil(bytes / 4);
};

const estimateTokenCount = (texts: string[]): number => texts.reduce((sum, text) => sum + estimateTokens(text), 0);

const chunkByTokenBudget = (
  items: ReadonlyArray<{ hash: string; text: string }>,
  maxItems: number,
  maxTokens: number,
): Array<Array<{ hash: string; text: string }>> => {
  const out: Array<Array<{ hash: string; text: string }>> = [];
  const itemLimit = Math.max(1, Math.trunc(maxItems));
  const tokenLimit = Math.max(1, Math.trunc(maxTokens));

  let current: Array<{ hash: string; text: string }> = [];
  let currentTokens = 0;

  for (const item of items) {
    const tokens = estimateTokens(item.text);
    const nextTokens = currentTokens + tokens;
    const hitsItemLimit = current.length >= itemLimit;
    const hitsTokenLimit = nextTokens > tokenLimit && current.length > 0;
    if (hitsItemLimit || hitsTokenLimit) {
      out.push(current);
      current = [];
      currentTokens = 0;
    }
    current.push(item);
    currentTokens += tokens;
  }
  if (current.length) out.push(current);
  return out;
};

const normalizeVoyageRateLimitState = (value: unknown): VoyageRateLimitState | null => {
  if (!isRecord(value)) return null;
  const windowStart = typeof value.window_start_ms === "number" && Number.isFinite(value.window_start_ms)
    ? Math.trunc(value.window_start_ms)
    : null;
  const requests = typeof value.requests === "number" && Number.isFinite(value.requests)
    ? Math.trunc(value.requests)
    : null;
  const tokens = typeof value.tokens === "number" && Number.isFinite(value.tokens) ? Math.trunc(value.tokens) : null;
  if (windowStart === null || requests === null || tokens === null) return null;
  if (windowStart < 0 || requests < 0 || tokens < 0) return null;
  return { window_start_ms: windowStart, requests, tokens };
};

const tryReserveVoyageBudget = async (
  kv: Deno.Kv,
  tokens: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  const windowMs = 60_000;
  const now = Date.now();
  const entry = await kv.get<VoyageRateLimitState>(VOYAGE_RATE_LIMIT_KEY);
  const current = normalizeVoyageRateLimitState(entry.value);
  const state = !current || now - current.window_start_ms >= windowMs
    ? { window_start_ms: now, requests: 0, tokens: 0 }
    : current;

  const wouldExceedRequests = VOYAGE_RATE_LIMIT_RPM > 0 && state.requests + 1 > VOYAGE_RATE_LIMIT_RPM;
  const wouldExceedTokens = VOYAGE_RATE_LIMIT_TPM > 0 && state.tokens + tokens > VOYAGE_RATE_LIMIT_TPM;
  if (wouldExceedRequests || wouldExceedTokens) {
    const waitMs = Math.max(0, windowMs - (now - state.window_start_ms));
    return { ok: false, wait_ms: waitMs };
  }

  const next: VoyageRateLimitState = {
    window_start_ms: state.window_start_ms,
    requests: state.requests + 1,
    tokens: state.tokens + tokens,
  };
  const commit = await kv.atomic().check(entry).set(VOYAGE_RATE_LIMIT_KEY, next).commit();
  if (commit.ok) return { ok: true };
  return { ok: false, wait_ms: 0 };
};

const applyVoyageRateLimit = async (
  kv: Deno.Kv,
  tokens: number,
  deadlineMs: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  // Best-effort concurrency-safe rate limiting using KV. If we can't reserve
  // within the request deadline, we fail with 429 and let clients retry.
  for (;;) {
    const now = Date.now();
    if (now >= deadlineMs) return { ok: false, wait_ms: 0 };
    let reserved: { ok: true } | { ok: false; wait_ms: number } = { ok: false, wait_ms: 0 };
    for (let attempt = 0; attempt < 6; attempt += 1) {
      reserved = await tryReserveVoyageBudget(kv, tokens);
      if (reserved.ok) return reserved;
      if (reserved.wait_ms > 0) break;
      await sleep(5 + attempt * 5);
    }
    if (reserved.ok) return reserved;
    const waitMs = reserved.wait_ms;
    if (waitMs <= 0) {
      // CAS contention without a concrete rate-limit wait; avoid tight spinning.
      const now2 = Date.now();
      const sleepMs = Math.min(25, Math.max(0, deadlineMs - now2));
      if (sleepMs > 0) await sleep(sleepMs);
      continue;
    }
    if (now + waitMs > deadlineMs) return { ok: false, wait_ms: waitMs };
    await sleep(waitMs);
  }
};

const parseEmbeddingsEncodingFormat = (
  value: unknown,
): { ok: true; value: EmbeddingsEncodingFormat } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: "float" };
  if (typeof value !== "string") return { ok: false, message: "encoding_format must be a string" };
  if (value === "float" || value === "base64") return { ok: true, value };
  return { ok: false, message: 'encoding_format must be one of: "float", "base64"' };
};

const parseEmbeddingsDimensions = (
  value: unknown,
): { ok: true; value: VoyageEmbeddingsDimension } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: VOYAGE_DEFAULT_DIMENSIONS };
  if (typeof value !== "number" || !Number.isFinite(value) || !Number.isInteger(value)) {
    return { ok: false, message: "dimensions must be an integer" };
  }
  if (!VOYAGE_SUPPORTED_DIMENSIONS.has(value)) {
    return { ok: false, message: "dimensions must be one of: 256, 512, 1024, 2048" };
  }
  return { ok: true, value: value as VoyageEmbeddingsDimension };
};

const buildEmbeddingsCacheProfileKey = (
  inputType: VoyageEmbeddingsInputType,
  dimensions: VoyageEmbeddingsDimension,
  encodingFormat: EmbeddingsEncodingFormat,
  truncation: boolean,
): string =>
  JSON.stringify([
    "voyage-profile-v2",
    VOYAGE_EMBEDDINGS_MODEL,
    inputType,
    dimensions,
    VOYAGE_OUTPUT_DTYPE,
    encodingFormat,
    truncation,
  ]);

const buildResolvedEmbeddingsProfile = (
  inputType: VoyageEmbeddingsInputType,
  dimensions: VoyageEmbeddingsDimension,
  encodingFormat: EmbeddingsEncodingFormat,
  truncation: boolean,
): ResolvedEmbeddingsProfile => ({
  upstream: "voyage",
  upstream_model: VOYAGE_EMBEDDINGS_MODEL,
  input_type: inputType,
  dimensions,
  output_dtype: VOYAGE_OUTPUT_DTYPE,
  encoding_format: encodingFormat,
  truncation,
  cache_profile_key: buildEmbeddingsCacheProfileKey(inputType, dimensions, encodingFormat, truncation),
});

const resolveOpenAiEmbeddingsModel = (raw: string): "voyage-4-large" | null => {
  const normalized = raw.trim().toLowerCase();
  if (
    normalized === VOYAGE_EMBEDDINGS_MODEL ||
    normalized === "text-embedding-3-small" ||
    normalized === "text-embedding-3-large"
  ) {
    return VOYAGE_EMBEDDINGS_MODEL;
  }
  return null;
};

const parseEmbeddingsRequest = (
  rawBody: Record<string, unknown>,
  contract: "openai" | "uos",
): EmbeddingsParseResult => {
  const allowedKeys = contract === "openai" ? EMBEDDINGS_ALLOWED_KEYS : UOS_EMBEDDINGS_ALLOWED_KEYS;
  const unknownKey = findUnknownKey(rawBody, allowedKeys);
  if (unknownKey) {
    return {
      ok: false,
      response: openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error"),
    };
  }

  const modelRaw = getString(rawBody.model);
  if (!modelRaw || !modelRaw.trim()) {
    return {
      ok: false,
      response: openaiError(400, "model is required and must be a non-empty string", "invalid_request_error", {
        param: "model",
      }),
    };
  }
  const model = contract === "openai" ? modelRaw.trim() : modelRaw;
  if (
    contract === "uos"
      ? model !== VOYAGE_EMBEDDINGS_MODEL
      : resolveOpenAiEmbeddingsModel(model) !== VOYAGE_EMBEDDINGS_MODEL
  ) {
    return {
      ok: false,
      response: openaiError(400, `Unsupported embedding model: ${model}`, "model_not_found", { param: "model" }),
    };
  }

  const dimensions = parseEmbeddingsDimensions(rawBody.dimensions);
  if (!dimensions.ok) {
    return {
      ok: false,
      response: openaiError(400, dimensions.message, "invalid_request_error", { param: "dimensions" }),
    };
  }

  const encodingFormat = parseEmbeddingsEncodingFormat(rawBody.encoding_format);
  if (!encodingFormat.ok) {
    return {
      ok: false,
      response: openaiError(400, encodingFormat.message, "invalid_request_error", { param: "encoding_format" }),
    };
  }
  if (contract === "uos" && encodingFormat.value !== "float") {
    return {
      ok: false,
      response: openaiError(
        400,
        'encoding_format must be "float" for UOS embeddings',
        "invalid_request_error",
        { param: "encoding_format" },
      ),
    };
  }

  let inputType: VoyageEmbeddingsInputType = "document";
  let truncation = true;
  if (contract === "uos") {
    if (rawBody.input_type !== "query" && rawBody.input_type !== "document") {
      return {
        ok: false,
        response: openaiError(
          400,
          'input_type is required and must be one of: "query", "document"',
          "invalid_request_error",
          { param: "input_type" },
        ),
      };
    }
    inputType = rawBody.input_type;
    if (rawBody.truncation !== undefined && typeof rawBody.truncation !== "boolean") {
      return {
        ok: false,
        response: openaiError(400, "truncation must be a boolean", "invalid_request_error", {
          param: "truncation",
        }),
      };
    }
    truncation = rawBody.truncation === undefined ? true : rawBody.truncation;
  } else if (Object.prototype.hasOwnProperty.call(rawBody, "user")) {
    const user = rawBody.user;
    if (user !== undefined && user !== null && typeof user !== "string") {
      return {
        ok: false,
        response: openaiError(400, "user must be a string", "invalid_request_error", { param: "user" }),
      };
    }
  }

  const inputRaw = rawBody.input;
  let inputs: string[] = [];
  if (typeof inputRaw === "string") {
    inputs = [inputRaw];
  } else if (Array.isArray(inputRaw)) {
    for (const item of inputRaw) {
      if (typeof item !== "string") {
        return {
          ok: false,
          response: openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
            param: "input",
          }),
        };
      }
      inputs.push(item);
    }
  } else {
    return {
      ok: false,
      response: openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
        param: "input",
      }),
    };
  }

  if (inputs.length === 0) {
    return {
      ok: false,
      response: openaiError(
        400,
        "input must be a non-empty string or a non-empty array",
        "invalid_request_error",
        { param: "input" },
      ),
    };
  }
  if (inputs.length > EMBEDDINGS_MAX_INPUTS_PER_REQUEST) {
    return {
      ok: false,
      response: openaiError(
        400,
        `Too many inputs: ${inputs.length} (max ${EMBEDDINGS_MAX_INPUTS_PER_REQUEST})`,
        "invalid_request_error",
        { param: "input" },
      ),
    };
  }

  let totalChars = 0;
  for (const text of inputs) {
    const len = text.length;
    if (len > EMBEDDINGS_MAX_CHARS_PER_INPUT) {
      return {
        ok: false,
        response: openaiError(
          400,
          `Input too large: ${len} chars (max ${EMBEDDINGS_MAX_CHARS_PER_INPUT})`,
          "invalid_request_error",
          { param: "input" },
        ),
      };
    }
    totalChars += len;
    if (totalChars > EMBEDDINGS_MAX_TOTAL_CHARS) {
      return {
        ok: false,
        response: openaiError(
          400,
          `Request too large: ${totalChars} chars total (max ${EMBEDDINGS_MAX_TOTAL_CHARS})`,
          "invalid_request_error",
          { param: "input" },
        ),
      };
    }
    const tokenEstimate = estimateTokens(text);
    if (tokenEstimate > VOYAGE_RATE_LIMIT_TPM) {
      return {
        ok: false,
        response: openaiError(
          400,
          `Input too large for embeddings provider: ~${tokenEstimate} tokens (max ${VOYAGE_RATE_LIMIT_TPM}).`,
          "invalid_request_error",
          { param: "input" },
        ),
      };
    }
  }

  return {
    ok: true,
    value: {
      model,
      inputs,
      total_chars: totalChars,
      profile: buildResolvedEmbeddingsProfile(inputType, dimensions.value, encodingFormat.value, truncation),
    },
  };
};

const isValidEmbeddingVector = (value: unknown, dimensions: VoyageEmbeddingsDimension): value is number[] =>
  Array.isArray(value) &&
  value.length === dimensions &&
  value.every((item) => typeof item === "number" && Number.isFinite(item));

const floatEmbeddingToBase64 = (embedding: number[]): string => {
  const buffer = new ArrayBuffer(embedding.length * 4);
  const view = new DataView(buffer);
  for (let i = 0; i < embedding.length; i += 1) {
    view.setFloat32(i * 4, embedding[i], true);
  }
  const bytes = new Uint8Array(buffer);
  const chunkSize = 0x8000; // Avoid large variadic calls and quadratic string concatenation.
  const chunks: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    chunks.push(String.fromCharCode(...chunk));
  }
  return btoa(chunks.join(""));
};

const bytesToBase64 = (bytes: Uint8Array): string => {
  const chunkSize = 0x8000;
  const chunks: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    chunks.push(String.fromCharCode(...chunk));
  }
  return btoa(chunks.join(""));
};

const base64ToBytes = (value: string): Uint8Array<ArrayBuffer> | null => {
  try {
    const raw = atob(value);
    const bytes = new Uint8Array(new ArrayBuffer(raw.length));
    for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
    return bytes;
  } catch {
    return null;
  }
};

const normalizeEmbeddingsJobInputRecord = (value: unknown): EmbeddingsJobInputRecord | null => {
  if (!isRecord(value)) return null;
  const v = value.v;
  if (v !== 1) return null;
  const iv = getString(value.iv_b64);
  const data = getString(value.data_b64);
  if (!iv || !data) return null;
  const createdAt = typeof value.created_at_ms === "number" && Number.isFinite(value.created_at_ms)
    ? Math.trunc(value.created_at_ms)
    : null;
  if (createdAt === null || createdAt < 0) return null;
  return { v: 1, iv_b64: iv, data_b64: data, created_at_ms: createdAt };
};

const importEmbeddingsJobKey = async (tokenSeed: string): Promise<CryptoKey> => {
  const material = new TextEncoder().encode(`uos_embeddings_job_v2:${tokenSeed}`);
  const digest = await crypto.subtle.digest("SHA-256", material);
  return await crypto.subtle.importKey("raw", digest, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
};

const encryptEmbeddingsJobInput = async (tokenSeed: string, text: string): Promise<EmbeddingsJobInputRecord> => {
  const key = await importEmbeddingsJobKey(tokenSeed);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const bytes = new TextEncoder().encode(text);
  const encrypted = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, bytes);
  return {
    v: 1,
    iv_b64: bytesToBase64(iv),
    data_b64: bytesToBase64(new Uint8Array(encrypted)),
    created_at_ms: Date.now(),
  };
};

const decryptEmbeddingsJobInput = async (
  tokenSeed: string,
  record: EmbeddingsJobInputRecord,
): Promise<string | null> => {
  const iv = base64ToBytes(record.iv_b64);
  const data = base64ToBytes(record.data_b64);
  if (!iv || !data) return null;
  try {
    const key = await importEmbeddingsJobKey(tokenSeed);
    const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, data);
    return new TextDecoder().decode(new Uint8Array(decrypted));
  } catch {
    return null;
  }
};

const extractRetryAfterMs = (value: string | null): number | null => {
  if (!value) return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  const seconds = Number(trimmed);
  if (Number.isFinite(seconds) && seconds > 0) {
    return Math.min(60_000, Math.trunc(seconds * 1000));
  }
  const asDate = Date.parse(trimmed);
  if (!Number.isNaN(asDate)) {
    const delta = asDate - Date.now();
    if (delta > 0) return Math.min(60_000, Math.trunc(delta));
  }
  return null;
};

const readVoyageApiKey = async (kv: Deno.Kv | null): Promise<string | null> => {
  const envKey = (getEnv("VOYAGEAI_API_KEY") ?? "").trim();
  if (envKey) return envKey;
  if (!kv) return null;
  const entry = await kv.get<string>(VOYAGE_API_KEY_KV_KEY);
  const kvKey = typeof entry.value === "string" ? entry.value.trim() : "";
  return kvKey || null;
};

const fetchVoyageEmbeddings = async (params: {
  apiKey: string;
  model: "voyage-4-large";
  inputs: string[];
  inputType: VoyageEmbeddingsInputType;
  dimensions: VoyageEmbeddingsDimension;
  outputDtype: VoyageEmbeddingsOutputDtype;
  truncation: boolean;
  deadlineMs: number;
}): Promise<{ vectors: number[][]; totalTokens: number | null }> => {
  const controller = new AbortController();
  const now = Date.now();
  const timeoutMs = Math.max(1, Math.min(EMBEDDINGS_TIMEOUT_MS, params.deadlineMs - now));
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const resp = await fetch(VOYAGE_EMBEDDINGS_URL, {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${params.apiKey}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        model: params.model,
        input: params.inputs.length === 1 ? params.inputs[0] : params.inputs,
        input_type: params.inputType,
        output_dimension: params.dimensions,
        output_dtype: params.outputDtype,
        truncation: params.truncation,
      }),
      signal: controller.signal,
    });

    if (!resp.ok) {
      // Avoid echoing upstream bodies; they can contain provider details and may be surfaced to clients/logs.
      const err = new Error(`Voyage embeddings failed (${resp.status}).`);
      (err as { status?: number; retry_after_ms?: number }).status = resp.status;
      (err as { retry_after_ms?: number }).retry_after_ms = extractRetryAfterMs(resp.headers.get("Retry-After")) ??
        undefined;
      throw err;
    }

    const payload = await resp.json().catch(() => null) as unknown;
    if (!isRecord(payload) || !Array.isArray(payload.data)) {
      throw new Error("Voyage embeddings returned invalid JSON.");
    }

    let totalTokens: number | null = null;
    if (isRecord(payload.usage)) {
      const rawTotalTokens = payload.usage.total_tokens;
      if (typeof rawTotalTokens === "number" && Number.isFinite(rawTotalTokens)) {
        totalTokens = Math.max(0, Math.trunc(rawTotalTokens));
      }
    }

    const data = payload.data as Array<Record<string, unknown>>;
    const vectors: number[][] = [];
    for (const item of data) {
      const embedding = isRecord(item) ? item.embedding : null;
      if (!Array.isArray(embedding)) {
        throw new Error("Voyage embeddings response missing embedding vector.");
      }
      const vec: number[] = [];
      for (const v of embedding) {
        if (typeof v !== "number" || !Number.isFinite(v)) {
          throw new Error("Voyage embeddings response contained non-numeric values.");
        }
        vec.push(v);
      }
      vectors.push(vec);
    }
    return { vectors, totalTokens };
  } finally {
    clearTimeout(timeout);
  }
};

const extractMessageContentItems = (role: ResponseMessageItem["role"], content: unknown): MessageContentItem[] => {
  const isAssistant = role === "assistant";
  const textItemType: MessageContentItem["type"] = isAssistant ? "output_text" : "input_text";

  if (typeof content === "string") {
    return [{ type: textItemType, text: content }];
  }

  if (!Array.isArray(content)) {
    return [{ type: textItemType, text: "" }];
  }

  const items: MessageContentItem[] = [];
  for (const part of content) {
    if (!isRecord(part)) continue;
    const partType = getString(part.type);

    if (partType === "text" || partType === "input_text" || partType === "output_text") {
      const text = getString(part.text);
      if (text) items.push({ type: textItemType, text });
      continue;
    }

    if (partType === "image_url" || partType === "input_image") {
      if (isAssistant) continue;
      let url: string | null = null;
      let detail: string | undefined;
      if (partType === "image_url") {
        const image = isRecord(part.image_url) ? part.image_url : null;
        url = image ? getString(image.url) : null;
        detail = normalizeImageDetail(image?.detail ?? part.detail);
      } else {
        url = getString(part.image_url);
        detail = normalizeImageDetail(part.detail);
      }
      const trimmed = (url ?? "").trim();
      if (trimmed) {
        items.push(
          detail ? { type: "input_image", image_url: trimmed, detail } : { type: "input_image", image_url: trimmed },
        );
      }
      continue;
    }
  }

  if (items.length > 0) return items;
  return [{ type: textItemType, text: "" }];
};

const normalizeImageDetail = (value: unknown): string | undefined => {
  const detail = getString(value)?.trim();
  return detail || undefined;
};

const messageContentToText = (items: MessageContentItem[]): string =>
  items
    .filter((item) => item.type === "input_text" || item.type === "output_text")
    .map((item) => item.text)
    .filter((text) => text && text.trim())
    .join("\n");

const chatRoleToCodexRole = (role: string): ResponseMessageItem["role"] | null => {
  if (role === "system") return "developer";
  if (role === "user") return "user";
  if (role === "assistant") return "assistant";
  if (role === "developer") return "developer";
  if (role === "tool") return "developer";
  return null;
};

const normalizeModelForCodex = (model: string): string => {
  return model.trim();
};

const normalizeUnixSeconds = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const seconds = Math.trunc(value);
  return seconds >= 0 ? seconds : null;
};

const normalizeModelEntry = (value: unknown, fallbackCreated: number): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = getString(value.id) ?? getString(value.slug) ?? getString(value.model) ?? getString(value.name);
  if (!id) return null;
  return {
    id,
    object: "model",
    created: normalizeUnixSeconds(value.created) ?? fallbackCreated,
    owned_by: getString(value.owned_by) ?? "openai",
  };
};

const normalizeModelList = (payload: unknown): { object: "list"; data: Record<string, unknown>[] } | null => {
  if (!isRecord(payload)) return null;
  const fallbackCreated = typeof payload.updated_at_ms === "number" && Number.isFinite(payload.updated_at_ms)
    ? Math.max(0, Math.trunc(payload.updated_at_ms / 1000))
    : 0;
  const data = Array.isArray(payload.data) ? payload.data : null;
  if (data) {
    const normalized = data.map((entry) => normalizeModelEntry(entry, fallbackCreated)).filter(Boolean) as Record<
      string,
      unknown
    >[];
    return { object: "list", data: normalized };
  }
  const models = Array.isArray(payload.models) ? payload.models : null;
  if (models) {
    const normalized = models.map((entry) => normalizeModelEntry(entry, fallbackCreated)).filter(Boolean) as Record<
      string,
      unknown
    >[];
    return { object: "list", data: normalized };
  }
  return null;
};

const normalizeModelCapabilitiesEntry = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = modelIdFromSnapshotRecord(value);
  if (!id) return null;
  const reasoning = getCodexModelReasoning(value);
  return {
    id,
    object: "uos.model_capabilities",
    owned_by: getString(value.owned_by) ?? "openai",
    display_name: getString(value.display_name),
    upstream_provider: "codex_chatgpt",
    supported_endpoints: ["/v1/chat/completions", "/v1/responses"],
    supported_reasoning_levels: reasoning.levels,
    default_reasoning_effort: reasoning.defaultLevel,
    reasoning_effort_wire_map: Object.fromEntries(reasoning.wireEfforts),
    context_window_tokens: normalizeTokenCount(value.context_window),
    max_context_window_tokens: normalizeTokenCount(value.max_context_window),
    auto_compact_token_limit_tokens: normalizeTokenCount(value.auto_compact_token_limit),
  };
};

const toResponseMessageItem = (message: unknown): ResponseMessageItem | null => {
  if (!isRecord(message)) return null;
  const roleRaw = getString(message.role);
  if (!roleRaw) return null;
  const role = chatRoleToCodexRole(roleRaw);
  if (!role) return null;
  const content = extractMessageContentItems(role, message.content);
  return { type: "message", role, content };
};

const normalizeResponseContentItem = (value: unknown): MessageContentItem | null => {
  if (!isRecord(value)) return null;
  const partType = getString(value.type);
  if (!partType) return null;

  if (partType === "input_text" || partType === "text") {
    const text = getString(value.text);
    if (text === null) return null;
    return { type: "input_text", text };
  }

  if (partType === "input_image" || partType === "image_url") {
    let url: string | null = null;
    let detail: string | undefined;
    if (partType === "image_url") {
      const image = isRecord(value.image_url) ? value.image_url : null;
      url = image ? getString(image.url) : null;
      detail = normalizeImageDetail(image?.detail ?? value.detail);
    } else {
      url = getString(value.image_url);
      detail = normalizeImageDetail(value.detail);
    }
    const trimmed = (url ?? "").trim();
    if (!trimmed) return null;
    return detail ? { type: "input_image", image_url: trimmed, detail } : { type: "input_image", image_url: trimmed };
  }

  return null;
};

const normalizeResponseInputItem = (value: unknown): ResponseMessageItem | null => {
  if (!isRecord(value)) return null;
  const itemType = getString(value.type);
  if (itemType && itemType !== "message") return null;
  return toResponseMessageItem(value);
};

const recordResponsesTerminal = (
  event: ResponsesStreamEvent,
  usageContext?: UsageContext,
): void => {
  if (event.terminal) recordStreamTerminalType(usageContext, event.type as ResponseStreamTerminalType);
  if (event.type !== "response.completed") {
    void recordErrorUsage(usageContext);
    return;
  }
  const usage = isRecord(event.value.response) ? extractUsageTokens(event.value.response.usage) : null;
  void recordCompletionUsage(usageContext, usage);
};

const responseHasOutputText = (output: unknown): boolean => {
  if (!Array.isArray(output)) return false;
  for (const item of output) {
    if (!isRecord(item) || !Array.isArray(item.content)) continue;
    for (const contentItem of item.content) {
      if (!isRecord(contentItem)) continue;
      if (getString(contentItem.type) === "output_text" && (getString(contentItem.text) ?? "").length > 0) {
        return true;
      }
    }
  }
  return false;
};

const withAccumulatedResponseText = (response: Record<string, unknown>, text: string): Record<string, unknown> => {
  if (!text || responseHasOutputText(response.output)) return response;
  const output = Array.isArray(response.output) ? [...response.output] : [];
  output.push({
    id: `msg_${crypto.randomUUID().replace(/-/g, "")}`,
    type: "message",
    status: "completed",
    role: "assistant",
    content: [{ type: "output_text", text, annotations: [] }],
  });
  return { ...response, output };
};

const withAccumulatedResponseItems = (
  response: Record<string, unknown>,
  accumulated: Record<string, unknown>[],
): Record<string, unknown> => {
  if (!accumulated.length) return response;
  const output = Array.isArray(response.output) ? response.output.filter(isRecord).map((item) => ({ ...item })) : [];
  const existingIds = new Set(output.map((item) => getString(item.id)).filter(Boolean));
  for (const item of accumulated) {
    const id = getString(item.id);
    if (id && existingIds.has(id)) continue;
    output.push(item);
    if (id) existingIds.add(id);
  }
  return { ...response, output };
};

const streamChatCompletions = (
  source: PreflightedResponsesStream,
  model: string,
  usageContext: UsageContext | undefined,
  provider: UpstreamProvider,
  lifecycle: YunwuTransportLifecycle,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): Response => {
  const encoder = new TextEncoder();
  const iterator = source.iterator;
  let pending: ResponsesStreamEvent | undefined = source.first;
  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let sentRole = false;
  let closed = false;
  const stream = new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (closed) return;
      try {
        while (!closed) {
          const next = pending ? { done: false as const, value: pending } : await iterator.next();
          pending = undefined;
          if (next.done) {
            throw new ResponsesStreamError("Upstream Responses stream ended before a terminal event.", {
              kind: "premature_eof",
            });
          }
          const event = next.value;
          const ev = event.value;
          const type = event.type;
          if (type === "response.created" && isRecord(ev.response)) {
            const upstreamId = getString(ev.response.id);
            const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
            if (upstreamId) id = upstreamId;
            if (createdAt) created = createdAt;
            continue;
          }

          if (type === "response.output_text.delta") {
            const delta = getString(ev.delta) ?? "";
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? { content: delta } : { role: "assistant", content: delta },
                  finish_reason: null,
                },
              ],
            };
            sentRole = true;
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            return;
          }

          if (type === "response.completed") {
            lifecycle.terminal(type);
            recordStreamTerminalType(usageContext, "response.completed");
            const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
            void recordCompletionUsage(usageContext, usageTokens);
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? {} : { role: "assistant" },
                  finish_reason: "stop",
                },
              ],
            };
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            controller.enqueue(encoder.encode("data: [DONE]\n\n"));
            closed = true;
            controller.close();
            void iterator.return("Responses terminal event translated").catch(() => {});
            return;
          }
          if (event.terminal) {
            lifecycle.terminal(type);
            recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
            void recordErrorUsage(usageContext);
            const errorValue = {
              error: {
                message: `Upstream terminated with ${type}.`,
                type: "server_error",
                code: "upstream_stream_error",
              },
            };
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(errorValue)}\n\n`));
            closed = true;
            controller.close();
            void iterator.return("Responses terminal error translated").catch(() => {});
            return;
          }
        }
      } catch (error) {
        if (closed) return;
        const terminalType = classifyStreamFailure(error, signal, downstreamSignal);
        recordStreamTerminalType(usageContext, terminalType);
        if (terminalType === "cancelled") lifecycle.cancelled();
        else lifecycle.ambiguous();
        void recordErrorUsage(usageContext);
        const errorValue = {
          error: {
            message: "The upstream stream ended unexpectedly.",
            type: "server_error",
            code: "upstream_stream_error",
          },
        };
        if (!downstreamSignal.aborted) {
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(errorValue)}\n\n`));
        }
        closed = true;
        controller.close();
      }
    },
    cancel(reason) {
      if (closed) return;
      closed = true;
      recordStreamTerminalType(usageContext, "cancelled");
      lifecycle.cancelled();
      void recordErrorUsage(usageContext);
      void iterator.return(reason).catch(() => {});
    },
  });

  return new Response(stream, {
    status: 200,
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      "x-uos-upstream": provider,
    },
  });
};

const completeChatCompletions = async (
  source: PreflightedResponsesStream,
  model: string,
  usageContext: UsageContext | undefined,
  provider: UpstreamProvider,
  lifecycle: YunwuTransportLifecycle,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): Promise<Response> => {
  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let content = "";
  let usage: Record<string, unknown> | null = null;

  let completed = false;
  try {
    let pending: ResponsesStreamEvent | undefined = source.first;
    for (;;) {
      const next = pending ? { done: false as const, value: pending } : await source.iterator.next();
      pending = undefined;
      if (next.done) break;
      const event = next.value;
      const ev = event.value;
      const type = event.type;
      if (event.terminal) {
        lifecycle.terminal(type);
        recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
      }
      if (type === "response.created" && isRecord(ev.response)) {
        const upstreamId = getString(ev.response.id);
        const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
        if (upstreamId) id = upstreamId;
        if (createdAt) created = createdAt;
        continue;
      }
      if (type === "response.output_text.delta") {
        content += getString(ev.delta) ?? "";
        continue;
      }
      if (type === "response.completed" && isRecord(ev.response)) {
        const usageTokens = extractUsageTokens(ev.response.usage);
        if (usageTokens) {
          usage = {
            prompt_tokens: usageTokens.inputTokens,
            completion_tokens: usageTokens.outputTokens,
            total_tokens: usageTokens.totalTokens,
          };
        }
        await recordCompletionUsage(usageContext, usageTokens);
        completed = true;
        break;
      }
      if (event.terminal) break;
    }
  } catch (error) {
    const terminalType = classifyStreamFailure(error, signal, downstreamSignal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType === "cancelled") lifecycle.cancelled();
    else lifecycle.ambiguous();
    completed = false;
  } finally {
    // This path consumes the generator manually (rather than through
    // `for await`), so explicitly close it after a terminal event or error.
    // Otherwise the parser can remain suspended at its final `yield` while
    // retaining the upstream reader lock.
    await source.iterator.return("Chat Completions response consumed").catch(() => {});
  }
  if (!completed) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Upstream stream ended without response.completed.", "upstream_stream_error", {
      headers: { "x-uos-upstream": provider },
    });
  }

  const body: Record<string, unknown> = {
    id,
    object: "chat.completion",
    created,
    model,
    choices: [
      {
        index: 0,
        message: { role: "assistant", content },
        finish_reason: "stop",
      },
    ],
  };
  if (usage) body.usage = usage;
  return json(200, body, { "x-uos-upstream": provider });
};

export const handleModels = async (req?: Request): Promise<Response> => {
  if (req) {
    const clientVersion = getCatalogClientVersion(req);
    if (clientVersion !== null) return await handleCodexCatalogModels(req, clientVersion);
  }
  const snapshot = await loadCodexModelsSnapshot();
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0
    ? normalizeModelList(snapshot)
    : null;

  return json(
    200,
    normalized ?? { object: "list", data: [] },
    { "x-uos-upstream": snapshot?.source || "stored_codex_models" },
  );
};

export const handleModelCapabilities = async (): Promise<Response> => {
  const snapshot = await loadFullCodexModelsSnapshot();
  const data = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0
    ? snapshot.models.map(normalizeModelCapabilitiesEntry).filter(Boolean) as Record<string, unknown>[]
    : [];

  return json(
    200,
    {
      object: "list",
      data,
      upstream_provider: "codex_chatgpt",
      source: snapshot?.source ?? "stored_codex_models",
      client_version: snapshot?.client_version ?? null,
      updated_at_ms: snapshot?.updated_at_ms ?? null,
    },
    { "x-uos-upstream": snapshot?.source || "stored_codex_models" },
  );
};

const withVoyageUpstreamHeader = (response: Response): Response => {
  const headers = new Headers(response.headers);
  headers.set("x-uos-upstream", "voyage");
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const handleEmbeddingsRequest = async (
  req: Request,
  contract: "openai" | "uos",
  usageContext?: UsageContext,
  options: Readonly<{ kv?: Deno.Kv | null }> = {},
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const rawBody = (await readJsonBody(req)) as Record<string, unknown> | null;
  if (!rawBody || !isRecord(rawBody)) {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }

  const parsed = parseEmbeddingsRequest(rawBody, contract);
  if (!parsed.ok) return parsed.response;
  const { model, inputs, profile } = parsed.value;

  const kv = Object.prototype.hasOwnProperty.call(options, "kv") ? options.kv ?? null : await getKv();
  const hashes = await Promise.all(inputs.map((text) => sha256Hex(text)));
  let idempotencyLease: EmbeddingsIdempotencyLease | null = null;
  let idempotencyDispatched = false;
  let idempotencyHasConfirmedSuccess = false;
  const idempotencyKey = contract === "uos" ? req.headers.get("Idempotency-Key") : null;
  if (idempotencyKey !== null) {
    if (
      !idempotencyKey ||
      idempotencyKey.length > EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS ||
      hasAsciiControlCharacter(idempotencyKey)
    ) {
      return openaiError(
        400,
        `Idempotency-Key must contain 1-${EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS} non-control characters.`,
        "invalid_request_error",
        { param: null },
      );
    }
    const principal = usageContext?.idempotencyPrincipal?.trim() ?? "";
    if (!kv || !principal) return embeddingsIdempotencyUnavailableResponse();
    const fingerprint = await buildEmbeddingsIdempotencyFingerprint(profile, hashes);
    const acquired = await acquireEmbeddingsIdempotencyLease({
      kv,
      principal,
      idempotencyKey,
      fingerprint,
      requestId,
    });
    if (acquired.kind === "replay") return acquired.response;
    if (acquired.kind === "error") return acquired.response;
    idempotencyLease = acquired.lease;
  }

  const releaseBeforeDispatch = async (response: Response): Promise<Response> => {
    if (!idempotencyLease) return response;
    const released = await releaseEmbeddingsIdempotencyReservation(idempotencyLease, false);
    return released ? response : embeddingsIdempotencyUnavailableResponse();
  };
  const releaseAfterExplicitUpstreamFailure = async (response: Response): Promise<Response> => {
    if (!idempotencyLease) return response;
    if (idempotencyHasConfirmedSuccess) return await failIndeterminate();
    const released = await releaseEmbeddingsIdempotencyReservation(idempotencyLease, true);
    if (released) return response;
    await markEmbeddingsIdempotencyIndeterminate(idempotencyLease);
    return embeddingsIdempotencyIndeterminateResponse();
  };
  const failIndeterminate = async (): Promise<Response> => {
    if (idempotencyLease) await markEmbeddingsIdempotencyIndeterminate(idempotencyLease);
    return embeddingsIdempotencyIndeterminateResponse();
  };

  await recordRequestUsage(usageContext, { model, route: "embeddings", stream: false, reasoning: null });

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return await releaseBeforeDispatch(
      openaiError(
        503,
        "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
        "server_error",
        { type: "server_error", param: null },
      ),
    );
  }
  const shouldCache = Boolean(kv);

  // Dedupe within a request (hash collisions are astronomically unlikely).
  const buckets = new Map<string, { text: string; indices: number[] }>();
  for (let i = 0; i < inputs.length; i += 1) {
    const hash = hashes[i]!;
    const existing = buckets.get(hash);
    if (existing) {
      existing.indices.push(i);
    } else {
      buckets.set(hash, { text: inputs[i]!, indices: [i] });
    }
  }

  const cacheProfileKey = profile.cache_profile_key;
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);
  const vectorsByIndex: Array<number[] | null> = Array.from({ length: inputs.length }, () => null);

  let voyageTotalTokens = 0;
  let sawVoyageTokenUsage = false;

  const missing: Array<{ hash: string; text: string; indices: number[] }> = [];
  if (shouldCache && kv) {
    const unique = Array.from(buckets.entries()).map(([hash, bucket]) => ({ hash, ...bucket }));
    const entries = await Promise.all(unique.map((item) => kv.get<{ embedding?: unknown }>(cacheKeyFor(item.hash))));
    for (let i = 0; i < unique.length; i += 1) {
      const item = unique[i]!;
      const entry = entries[i]!;
      const cached = entry.value?.embedding;
      if (isValidEmbeddingVector(cached, profile.dimensions)) {
        for (const idx of item.indices) vectorsByIndex[idx] = cached;
      } else {
        missing.push(item);
      }
    }
  } else {
    for (const [hash, bucket] of buckets.entries()) {
      missing.push({ hash, text: bucket.text, indices: bucket.indices });
    }
  }

  if (missing.length > 0) {
    const chunks = chunkByTokenBudget(
      missing.map((item) => ({ hash: item.hash, text: item.text })),
      EMBEDDINGS_MAX_INPUTS_PER_REQUEST,
      VOYAGE_RATE_LIMIT_TPM,
    );

    let offset = 0;
    for (const chunk of chunks) {
      const now = Date.now();
      if (now >= deadlineMs) {
        await recordErrorUsage(usageContext);
        if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
        return await releaseBeforeDispatch(
          openaiError(502, "Embeddings request timed out.", "timeout", { type: "server_error", param: null }),
        );
      }

      const chunkItems = missing.slice(offset, offset + chunk.length);
      offset += chunk.length;
      const texts = chunkItems.map((item) => item.text);
      const tokenEstimate = estimateTokenCount(texts);

      if (kv) {
        const reserved = await applyVoyageRateLimit(kv, tokenEstimate, deadlineMs);
        if (!reserved.ok) {
          await recordErrorUsage(usageContext);
          if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
          const retryAfterSeconds = Math.max(1, Math.ceil(reserved.wait_ms / 1000));
          const body = {
            error: {
              message: `Rate limit exceeded; retry after ~${retryAfterSeconds}s`,
              type: "rate_limit_error",
              code: "rate_limit_exceeded",
              param: null,
            },
          };
          return await releaseBeforeDispatch(json(429, body, { "Retry-After": String(retryAfterSeconds) }));
        }
      }

      let attempt = 0;
      let backoffMs = 250;
      let vectors: number[][] | null = null;

      for (;;) {
        if (idempotencyLease && !idempotencyDispatched) {
          const markedDispatched = await markEmbeddingsIdempotencyDispatched(idempotencyLease);
          if (!markedDispatched) {
            await recordErrorUsage(usageContext);
            return embeddingsIdempotencyUnavailableResponse();
          }
          idempotencyDispatched = true;
        }
        try {
          const upstream = await fetchVoyageEmbeddings({
            apiKey,
            model: profile.upstream_model,
            inputs: texts,
            inputType: profile.input_type,
            dimensions: profile.dimensions,
            outputDtype: profile.output_dtype,
            truncation: profile.truncation,
            deadlineMs,
          });
          vectors = upstream.vectors;
          if (idempotencyLease) idempotencyHasConfirmedSuccess = true;
          if (typeof upstream.totalTokens === "number") {
            sawVoyageTokenUsage = true;
            voyageTotalTokens += upstream.totalTokens;
          }
          break;
        } catch (error) {
          const status = (error as { status?: number }).status;
          const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
          const snippet = formatErrorSnippet(error);
          const message = snippet
            ? `Embeddings upstream request failed: ${snippet}`
            : "Embeddings upstream request failed.";

          if (!status || !EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(status)) {
            console.error(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
            await recordErrorUsage(usageContext);
            if (!status) return await failIndeterminate();
            return await releaseAfterExplicitUpstreamFailure(
              openaiError(502, message, "upstream_error", { type: "server_error", param: null }),
            );
          }

          const waitMs = Math.max(0, retryAfterMs ?? backoffMs);
          if (attempt >= 2) {
            console.error(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
            await recordErrorUsage(usageContext);
            if (status === 429) {
              const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
              const body = {
                error: {
                  message,
                  type: "rate_limit_error",
                  code: "rate_limit_exceeded",
                  param: null,
                },
              };
              return await releaseAfterExplicitUpstreamFailure(
                json(429, body, { "Retry-After": String(retryAfterSeconds) }),
              );
            }
            return await releaseAfterExplicitUpstreamFailure(
              openaiError(502, message, "upstream_error", { type: "server_error", param: null }),
            );
          }

          const now = Date.now();
          if (now + waitMs >= deadlineMs) {
            if (status === 429) {
              await recordErrorUsage(usageContext);
              const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
              const body = {
                error: {
                  message,
                  type: "rate_limit_error",
                  code: "rate_limit_exceeded",
                  param: null,
                },
              };
              return await releaseAfterExplicitUpstreamFailure(
                json(429, body, { "Retry-After": String(retryAfterSeconds) }),
              );
            }
            await recordErrorUsage(usageContext);
            return await releaseAfterExplicitUpstreamFailure(
              openaiError(502, message, "upstream_error", { type: "server_error", param: null }),
            );
          }

          await sleep(waitMs);
          backoffMs = Math.min(2000, backoffMs * 2);
          attempt += 1;
        }
      }

      if (!vectors || vectors.length !== chunkItems.length) {
        await recordErrorUsage(usageContext);
        if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
        return openaiError(502, "Embeddings upstream returned a size mismatch.", "upstream_error", {
          type: "server_error",
          param: null,
        });
      }

      const wrongLengthIndex = vectors.findIndex((vector) => vector.length !== profile.dimensions);
      if (wrongLengthIndex >= 0) {
        await recordErrorUsage(usageContext);
        const actualLength = vectors[wrongLengthIndex]?.length ?? 0;
        if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
        return openaiError(
          502,
          `Embeddings upstream returned vector length ${actualLength}; expected ${profile.dimensions}.`,
          "upstream_dimension_mismatch",
          { type: "server_error", param: null },
        );
      }

      for (let i = 0; i < chunkItems.length; i += 1) {
        const item = chunkItems[i]!;
        const vec = vectors[i]!;
        for (const idx of item.indices) vectorsByIndex[idx] = vec;
        if (shouldCache && kv) {
          await writeEmbeddingsCacheEntryBestEffort(
            kv,
            cacheProfileKey,
            item.hash,
            vec,
            Date.now(),
            deadlineMs,
          );
        }
      }
    }
  }

  const data: Array<{ object: "embedding"; index: number; embedding: number[] | string }> = [];
  for (let i = 0; i < vectorsByIndex.length; i += 1) {
    const vec = vectorsByIndex[i];
    if (!vec) {
      await recordErrorUsage(usageContext);
      if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
      return openaiError(502, "Embeddings gateway failed to construct a complete response.", "server_error", {
        type: "server_error",
        param: null,
      });
    }
    data.push({
      object: "embedding",
      index: i,
      embedding: profile.encoding_format === "base64" ? floatEmbeddingToBase64(vec) : vec,
    });
  }

  const usageTokens: UsageTokens | null = sawVoyageTokenUsage
    ? { inputTokens: voyageTotalTokens, outputTokens: 0, totalTokens: voyageTotalTokens }
    : null;
  const response = json(200, {
    object: "list",
    data,
    model,
    usage: {
      prompt_tokens: usageTokens?.inputTokens ?? 0,
      total_tokens: usageTokens?.totalTokens ?? 0,
    },
  });
  if (idempotencyLease) {
    const stored = await storeEmbeddingsIdempotencySuccess(idempotencyLease, response);
    if (!stored) {
      if (idempotencyDispatched) return await failIndeterminate();
      return await releaseBeforeDispatch(embeddingsIdempotencyUnavailableResponse());
    }
  }
  await recordCompletionUsage(usageContext, usageTokens);
  return response;
};

export const handleEmbeddings = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsRequest(req, "openai", context)),
  );

export const handleUosEmbeddings = async (
  req: Request,
  usageContext?: UsageContext,
  options: Readonly<{ kv?: Deno.Kv | null }> = {},
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsRequest(req, "uos", context, options)),
  );

const buildEmbeddingsJobBody = (
  job: EmbeddingsJobRecord,
  result: Record<string, unknown> | null,
): Record<string, unknown> => ({
  id: job.id,
  object: "embeddings.job",
  status: job.status,
  created_at_ms: job.created_at_ms,
  updated_at_ms: job.updated_at_ms,
  model: job.model,
  upstream: job.upstream,
  upstream_model: job.upstream_model,
  input_type: job.input_type,
  dimensions: job.dimensions,
  output_dtype: job.output_dtype,
  encoding_format: job.encoding_format,
  truncation: job.truncation,
  input_count: job.input_count,
  total_chars: job.total_chars,
  retry_after_seconds: job.retry_after_seconds,
  error: job.error,
  result,
});

const loadEmbeddingsVectorsFromCache = async (
  kv: Deno.Kv,
  cacheProfileKey: string,
  hashesByIndex: string[],
  dimensions: VoyageEmbeddingsDimension,
): Promise<Array<number[] | null>> => {
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);
  const entries = await Promise.all(uniqueHashes.map((hash) => kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))));
  const vectorsByHash = new Map<string, number[]>();
  for (let i = 0; i < uniqueHashes.length; i += 1) {
    const hash = uniqueHashes[i]!;
    const cached = entries[i]?.value?.embedding;
    if (isValidEmbeddingVector(cached, dimensions)) {
      vectorsByHash.set(hash, cached);
    }
  }
  return hashesByIndex.map((hash) => vectorsByHash.get(hash) ?? null);
};

const buildOpenAiEmbeddingsResult = (
  model: string,
  vectorsByIndex: Array<number[] | null>,
  usageTotalTokens: number,
  encodingFormat: EmbeddingsEncodingFormat,
): Record<string, unknown> => ({
  object: "list",
  data: vectorsByIndex.map((vec, index) => ({
    object: "embedding",
    index,
    embedding: vec && encodingFormat === "base64" ? floatEmbeddingToBase64(vec) : vec ?? [],
  })),
  model,
  usage: { prompt_tokens: usageTotalTokens, total_tokens: usageTotalTokens },
});

const reserveVoyageBudgetForJob = async (
  kv: Deno.Kv,
  tokens: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  for (let attempt = 0; attempt < 6; attempt += 1) {
    const reserved = await tryReserveVoyageBudget(kv, tokens);
    if (reserved.ok) return reserved;
    if (reserved.wait_ms > 0) return reserved;
    await sleep(5 + attempt * 5);
  }
  return { ok: false, wait_ms: 1000 };
};

const updateEmbeddingsJobRecord = async (
  kv: Deno.Kv,
  jobKey: Deno.KvKey,
  lookupKey: Deno.KvKey,
  job: EmbeddingsJobRecord,
): Promise<void> => {
  await kv.atomic()
    .set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(
      lookupKey,
      { cache_profile_key: job.cache_profile_key } satisfies EmbeddingsJobLookupRecord,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    )
    .commit();
};

const deleteEmbeddingsJobInputs = async (
  kv: Deno.Kv,
  tokenHash: string,
  cacheProfileKey: string,
  jobId: string,
  uniqueHashes: string[],
): Promise<void> => {
  await Promise.all(
    uniqueHashes.map((hash) => kv.delete(embeddingsJobInputKey(tokenHash, cacheProfileKey, jobId, hash))),
  );
};

const runEmbeddingsJobAttempt = async (params: {
  reqId: string;
  kv: Deno.Kv;
  apiKey: string;
  tokenSeed: string;
  tokenHash: string;
  jobKey: Deno.KvKey;
  jobLookupKey: Deno.KvKey;
  jobEntry: Deno.KvEntryMaybe<EmbeddingsJobRecord>;
  job: EmbeddingsJobRecord;
  deadlineMs: number;
  usageContext?: UsageContext;
}): Promise<Response> => {
  const now = Date.now();
  if (params.job.locked_until_ms && params.job.locked_until_ms > now) {
    const retryAfterSeconds = Math.max(1, Math.ceil((params.job.locked_until_ms - now) / 1000));
    const body = buildEmbeddingsJobBody(params.job, null);
    return json(202, body, {
      "Retry-After": String(retryAfterSeconds),
      "x-uos-upstream": params.job.upstream,
    });
  }

  const lockedUntilMs = now + EMBEDDINGS_JOB_LOCK_MS;
  const locked: EmbeddingsJobRecord = {
    ...params.job,
    status: "running",
    locked_until_ms: lockedUntilMs,
    updated_at_ms: now,
    retry_after_seconds: null,
  };

  const lockCommit = await params.kv.atomic()
    .check(params.jobEntry)
    .set(params.jobKey, locked, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(
      params.jobLookupKey,
      { cache_profile_key: locked.cache_profile_key } satisfies EmbeddingsJobLookupRecord,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    )
    .commit();
  if (!lockCommit.ok) {
    const body = buildEmbeddingsJobBody(params.job, null);
    return json(202, body, { "Retry-After": "1", "x-uos-upstream": params.job.upstream });
  }

  const cacheProfileKey = locked.cache_profile_key;
  const hashesByIndex = locked.input_hashes;
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);

  let currentJob: EmbeddingsJobRecord = locked;
  let queueRetryAfterMs: number | null = null;

  const computeMissing = async (): Promise<string[]> => {
    const entries = await Promise.all(
      uniqueHashes.map((hash) => params.kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))),
    );
    const missing: string[] = [];
    for (let i = 0; i < uniqueHashes.length; i += 1) {
      const hash = uniqueHashes[i]!;
      const cached = entries[i]?.value?.embedding;
      if (isValidEmbeddingVector(cached, locked.dimensions)) continue;
      missing.push(hash);
    }
    return missing;
  };

  const failJob = async (message: string, code: string): Promise<Response> => {
    const failed: EmbeddingsJobRecord = {
      ...currentJob,
      status: "failed",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: null,
      error: { message, type: "server_error", code },
    };
    currentJob = failed;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, failed);
    await deleteEmbeddingsJobInputs(
      params.kv,
      params.tokenHash,
      failed.cache_profile_key,
      failed.id,
      uniqueHashes,
    );
    await recordErrorUsage(params.usageContext);
    return json(200, buildEmbeddingsJobBody(failed, null), { "x-uos-upstream": failed.upstream });
  };

  const queueJob = async (waitMs: number): Promise<Response> => {
    const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
    const queued: EmbeddingsJobRecord = {
      ...currentJob,
      status: "queued",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: retryAfterSeconds,
      error: null,
    };
    currentJob = queued;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, queued);
    const body = buildEmbeddingsJobBody(queued, null);
    return json(202, body, { "Retry-After": String(retryAfterSeconds), "x-uos-upstream": queued.upstream });
  };

  const succeedJob = async (): Promise<Response> => {
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(
      params.kv,
      cacheProfileKey,
      hashesByIndex,
      currentJob.dimensions,
    );
    if (vectorsByIndex.some((vec) => !vec)) {
      return await failJob("Embeddings job completed but cache entries were missing.", "embeddings_job_cache_miss");
    }
    const succeeded: EmbeddingsJobRecord = {
      ...currentJob,
      status: "succeeded",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: null,
      error: null,
    };
    currentJob = succeeded;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, succeeded);
    await deleteEmbeddingsJobInputs(
      params.kv,
      params.tokenHash,
      succeeded.cache_profile_key,
      succeeded.id,
      uniqueHashes,
    );
    const result = buildOpenAiEmbeddingsResult(
      succeeded.model,
      vectorsByIndex,
      succeeded.usage_total_tokens,
      succeeded.encoding_format,
    );
    const usageTokens: UsageTokens | null = succeeded.usage_total_tokens > 0
      ? { inputTokens: succeeded.usage_total_tokens, outputTokens: 0, totalTokens: succeeded.usage_total_tokens }
      : null;
    await recordCompletionUsage(params.usageContext, usageTokens);
    return json(200, buildEmbeddingsJobBody(succeeded, result), { "x-uos-upstream": succeeded.upstream });
  };

  const missingBefore = await computeMissing();
  if (missingBefore.length === 0) return await succeedJob();

  const inputEntries = await Promise.all(
    missingBefore.map((hash) =>
      params.kv.get<EmbeddingsJobInputRecord>(
        embeddingsJobInputKey(params.tokenHash, locked.cache_profile_key, locked.id, hash),
      )
    ),
  );
  const items: Array<{ hash: string; text: string }> = [];
  for (let i = 0; i < missingBefore.length; i += 1) {
    const hash = missingBefore[i]!;
    const entry = inputEntries[i]!;
    const normalized = normalizeEmbeddingsJobInputRecord(entry.value);
    if (!normalized) {
      return await failJob("Embeddings job input expired or was unavailable.", "embeddings_job_input_missing");
    }
    const text = await decryptEmbeddingsJobInput(params.tokenSeed, normalized);
    if (text === null) {
      return await failJob("Embeddings job input could not be decrypted.", "embeddings_job_input_decrypt_failed");
    }
    items.push({ hash, text });
  }

  const chunks = chunkByTokenBudget(items, EMBEDDINGS_MAX_INPUTS_PER_REQUEST, VOYAGE_RATE_LIMIT_TPM);
  for (const chunk of chunks) {
    if (Date.now() >= params.deadlineMs) {
      queueRetryAfterMs = 1000;
      break;
    }

    const texts = chunk.map((item) => item.text);
    const tokenEstimate = estimateTokenCount(texts);

    const reserved = await reserveVoyageBudgetForJob(params.kv, tokenEstimate);
    if (!reserved.ok) {
      queueRetryAfterMs = reserved.wait_ms > 0 ? reserved.wait_ms : 1000;
      break;
    }

    let vectors: number[][] | null = null;
    let totalTokens: number | null = null;
    try {
      const upstream = await fetchVoyageEmbeddings({
        apiKey: params.apiKey,
        model: currentJob.upstream_model,
        inputs: texts,
        inputType: currentJob.input_type,
        dimensions: currentJob.dimensions,
        outputDtype: currentJob.output_dtype,
        truncation: currentJob.truncation,
        deadlineMs: params.deadlineMs,
      });
      vectors = upstream.vectors;
      totalTokens = upstream.totalTokens;
    } catch (error) {
      const status = (error as { status?: number }).status;
      const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
      if (status && EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(status)) {
        queueRetryAfterMs = retryAfterMs ?? (status === 429 ? 60_000 : 1_000);
        break;
      }
      const snippet = formatErrorSnippet(error);
      const message = snippet
        ? `Embeddings upstream request failed: ${snippet}`
        : "Embeddings upstream request failed.";
      return await failJob(message, "embeddings_job_upstream_error");
    }

    if (!vectors || vectors.length !== chunk.length) {
      return await failJob("Embeddings upstream returned a size mismatch.", "embeddings_job_upstream_mismatch");
    }

    const wrongLengthIndex = vectors.findIndex((vector) => vector.length !== currentJob.dimensions);
    if (wrongLengthIndex >= 0) {
      const actualLength = vectors[wrongLengthIndex]?.length ?? 0;
      return await failJob(
        `Embeddings upstream returned vector length ${actualLength}; expected ${currentJob.dimensions}.`,
        "embeddings_job_upstream_dimension_mismatch",
      );
    }

    if (typeof totalTokens === "number") {
      currentJob = { ...currentJob, usage_total_tokens: currentJob.usage_total_tokens + totalTokens };
    }

    for (let i = 0; i < chunk.length; i += 1) {
      const item = chunk[i]!;
      const vec = vectors[i]!;
      await writeEmbeddingsCacheEntryBestEffort(
        params.kv,
        currentJob.cache_profile_key,
        item.hash,
        vec,
        Date.now(),
        params.deadlineMs,
      );
    }
  }

  const missingAfter = await computeMissing();
  if (missingAfter.length === 0) return await succeedJob();

  const waitMs = queueRetryAfterMs ?? 60_000;
  return await queueJob(waitMs);
};

const handleEmbeddingsJobCreateInternal = async (
  req: Request,
  authToken: string | null,
  usageContext?: UsageContext,
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const rawBody = (await readJsonBody(req)) as Record<string, unknown> | null;
  if (!rawBody || !isRecord(rawBody)) {
    await recordErrorUsage(usageContext);
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }

  const parsed = parseEmbeddingsRequest(rawBody, "uos");
  if (!parsed.ok) {
    await recordErrorUsage(usageContext);
    return parsed.response;
  }
  const { model, inputs, total_chars: totalChars, profile } = parsed.value;

  await recordRequestUsage(usageContext, { model, route: "embeddings.jobs.create", stream: false, reasoning: null });

  const kv = await getKv();
  if (!kv) {
    await recordErrorUsage(usageContext);
    return openaiError(503, "Embeddings jobs require Deno KV", "server_error", { type: "server_error", param: null });
  }

  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }

  const hashesByIndex = await Promise.all(inputs.map((text) => sha256Hex(text)));
  const uniqueTextsByHash = new Map<string, string>();
  for (let i = 0; i < inputs.length; i += 1) uniqueTextsByHash.set(hashesByIndex[i]!, inputs[i]!);
  const uniqueHashes = Array.from(uniqueTextsByHash.keys());

  const jobId = `embjob_${crypto.randomUUID().replace(/-/g, "")}`;
  const tokenSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const tokenHash = await sha256Hex(tokenSeed);
  const now = Date.now();

  // Store encrypted inputs (no raw text) so queued jobs can be processed later without the client resending inputs.
  const inputWrites = uniqueHashes.map(async (hash) => {
    const record = await encryptEmbeddingsJobInput(tokenSeed, uniqueTextsByHash.get(hash)!);
    await kv.set(
      embeddingsJobInputKey(tokenHash, profile.cache_profile_key, jobId, hash),
      record,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    );
  });
  await Promise.all(inputWrites);

  const job: EmbeddingsJobRecord = {
    id: jobId,
    status: "queued",
    created_at_ms: now,
    updated_at_ms: now,
    model,
    cache_profile_key: profile.cache_profile_key,
    upstream: profile.upstream,
    upstream_model: profile.upstream_model,
    input_type: profile.input_type,
    dimensions: profile.dimensions,
    output_dtype: profile.output_dtype,
    encoding_format: profile.encoding_format,
    truncation: profile.truncation,
    input_hashes: hashesByIndex,
    input_count: inputs.length,
    total_chars: totalChars,
    usage_total_tokens: 0,
    retry_after_seconds: null,
    locked_until_ms: null,
    error: null,
  };
  const jobKey = embeddingsJobKey(tokenHash, profile.cache_profile_key, jobId);
  const jobLookupKey = embeddingsJobLookupKey(tokenHash, jobId);
  const persisted = await kv.atomic()
    .set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(
      jobLookupKey,
      { cache_profile_key: profile.cache_profile_key } satisfies EmbeddingsJobLookupRecord,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    )
    .commit();
  if (!persisted.ok) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Embeddings job could not be persisted.", "server_error", {
      type: "server_error",
      param: null,
    });
  }

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const entry = await kv.get<EmbeddingsJobRecord>(jobKey);
  const value = entry.value;
  if (!value) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Embeddings job could not be persisted.", "server_error", {
      type: "server_error",
      param: null,
    });
  }

  return await runEmbeddingsJobAttempt({
    reqId: requestId,
    kv,
    apiKey,
    tokenSeed,
    tokenHash,
    jobKey,
    jobLookupKey,
    jobEntry: entry,
    job: value,
    deadlineMs,
    usageContext,
  });
};

export const handleEmbeddingsJobCreate = async (
  req: Request,
  authToken: string | null,
  usageContext?: UsageContext,
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsJobCreateInternal(req, authToken, context)),
  );

const handleEmbeddingsJobGetInternal = async (
  _req: Request,
  authToken: string | null,
  jobId: string,
  usageContext?: UsageContext,
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const kv = await getKv();
  if (!kv) {
    await recordErrorUsage(usageContext);
    return openaiError(503, "Embeddings jobs require Deno KV", "server_error", { type: "server_error", param: null });
  }

  const preferredSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const preferredHash = await sha256Hex(preferredSeed);
  const tokenSeed = preferredSeed;
  const tokenHash = preferredHash;
  const jobLookupKey = embeddingsJobLookupKey(preferredHash, jobId);
  const lookupEntry = await kv.get<EmbeddingsJobLookupRecord>(jobLookupKey);
  const cacheProfileKey = isRecord(lookupEntry.value) ? getString(lookupEntry.value.cache_profile_key) : null;
  if (!cacheProfileKey) {
    await recordErrorUsage(usageContext);
    return openaiError(404, "Embeddings job not found", "not_found", {
      type: "invalid_request_error",
      param: null,
    });
  }
  const jobKey = embeddingsJobKey(preferredHash, cacheProfileKey, jobId);
  const entry = await kv.get<EmbeddingsJobRecord>(jobKey);

  const job = entry.value;
  if (!job || job.cache_profile_key !== cacheProfileKey) {
    await recordErrorUsage(usageContext);
    return openaiError(404, "Embeddings job not found", "not_found", { type: "invalid_request_error", param: null });
  }

  await recordRequestUsage(usageContext, {
    model: job.model,
    route: "embeddings.jobs.get",
    stream: false,
    reasoning: null,
  });

  if (job.status === "succeeded") {
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(
      kv,
      job.cache_profile_key,
      job.input_hashes,
      job.dimensions,
    );
    const result = vectorsByIndex.some((vec) => !vec)
      ? null
      : buildOpenAiEmbeddingsResult(job.model, vectorsByIndex, job.usage_total_tokens, job.encoding_format);
    if (!result) {
      // Cache misses are unexpected (cache TTL is longer than job TTL), but if it happens
      // there's nothing the client can do besides resubmitting the job.
      const failed: EmbeddingsJobRecord = {
        ...job,
        status: "failed",
        updated_at_ms: Date.now(),
        locked_until_ms: null,
        retry_after_seconds: null,
        error: {
          message: "Embeddings job result was unavailable; please resubmit.",
          type: "server_error",
          code: "embeddings_job_result_missing",
        },
      };
      await updateEmbeddingsJobRecord(kv, jobKey, jobLookupKey, failed);
      return json(200, buildEmbeddingsJobBody(failed, null), { "x-uos-upstream": failed.upstream });
    }
    return json(200, buildEmbeddingsJobBody(job, result), { "x-uos-upstream": job.upstream });
  }

  if (job.status === "failed") {
    return json(200, buildEmbeddingsJobBody(job, null), { "x-uos-upstream": job.upstream });
  }

  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const response = await runEmbeddingsJobAttempt({
    reqId: requestId,
    kv,
    apiKey,
    tokenSeed,
    tokenHash,
    jobKey,
    jobLookupKey,
    jobEntry: entry,
    job,
    deadlineMs,
    usageContext,
  });

  return response;
};

export const handleEmbeddingsJobGet = async (
  req: Request,
  authToken: string | null,
  jobId: string,
  usageContext?: UsageContext,
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsJobGetInternal(req, authToken, jobId, context)),
  );

const handleChatCompletionsInternal = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const body = (await readJsonBody(req)) as ChatCompletionRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = body as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, CHAT_COMPLETIONS_ALLOWED_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const warnings = buildIgnoredWarnings(
    rawRecord,
    new Set([
      "messages",
      "model",
      "stream",
      "reasoning_effort",
      "tools",
      "tool_choice",
      "parallel_tool_calls",
      "prompt_cache_key",
    ]),
  );

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  let modelRaw = (modelRawValue ?? "").trim();
  if (!modelRaw) {
    const defaultModel = await getDefaultModel();
    if (!defaultModel) return defaultModelUnavailableError();
    modelRaw = defaultModel;
  }
  const model = normalizeModelForCodex(modelRaw);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.model = modelRaw;
  const modelMetadata = await getCodexModelMetadata(model);
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, modelMetadata);
  if (modelAvailabilityError) return modelAvailabilityError;
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return openaiError(400, "messages must be an array", "invalid_request_error");
  if (messagesRaw.length === 0) return openaiError(400, "messages must be a non-empty array", "invalid_request_error");

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" });
  }

  const input: ResponseMessageItem[] = [];
  const instructionParts: string[] = [];
  for (const msg of messagesRaw) {
    if (!isRecord(msg)) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    const roleRaw = getString(msg.role);
    if (!roleRaw) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    if (roleRaw === "system" || roleRaw === "developer") {
      const converted = toResponseMessageItem(msg);
      if (!converted) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
      const instructionText = messageContentToText(converted.content).trim();
      if (instructionText) instructionParts.push(instructionText);
      continue;
    }
    const converted = toResponseMessageItem(msg);
    if (!converted) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    input.push(converted);
  }

  if (input.length === 0) {
    // Ensure upstream receives a non-empty input for system-only chats.
    input.push({
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "" }],
    });
  }

  const instructions = instructionParts.join("\n\n").trim();
  const defaultEffort = await getDefaultReasoningEffort();
  const modelReasoning = modelMetadata.reasoning;
  const defaultReasoningLabel = resolveDefaultReasoningLabel(modelReasoning, defaultEffort);
  let reasoningValue: Record<string, unknown> | undefined;
  if (reasoningEffort.value === undefined) {
    reasoningValue = { effort: reasoningEffortForCodexRequest(defaultReasoningLabel, modelReasoning) };
  } else {
    reasoningValue = { effort: reasoningEffortForCodexRequest(reasoningEffort.value, modelReasoning) };
  }
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoningValue,
    instructions,
  });
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
  ];
  applyPassthroughToCodexRequest(codexBody, rawRecord, passthroughKeys);
  codexBody.store = false;

  const stream = Boolean(body.stream);
  const reasoningLabel = resolveReasoningLabelFromEffort(reasoningEffort.value, defaultReasoningLabel);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.reasoning = reasoningLabel;
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream,
    reasoning: reasoningLabel,
  });
  const requestInferenceSignal = inferenceSignal(req);

  let routed: RoutedResponsesUpstream;
  try {
    routed = await fetchResponsesWithPaidFallback(codexBody, {
      model,
      route: "chat.completions",
      stream,
      reasoning: reasoningLabel,
      usageContext,
      clientVersion: modelMetadata.snapshot?.client_version,
      signal: requestInferenceSignal,
    });
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error, usageContext?.responseTelemetry?.provider);
  }
  const upstream = routed.response;
  const lifecycle = createYunwuTransportLifecycle(routed.paidFallback);

  if (routed.gatewayResponse) {
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return upstream;
  }
  if (!upstream.ok) {
    lifecycle.terminal("response.failed");
    recordStreamTerminalType(usageContext, "response.failed");
    await recordErrorUsage(usageContext);
    const normalized = await toOpenAiUpstreamErrorResponse(upstream, routed.provider);
    return attachResponseTelemetry(normalized, usageContext?.responseTelemetry ?? createResponseTelemetryState());
  }

  if (!upstream.body) {
    lifecycle.ambiguous();
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body", {
      headers: { "x-uos-upstream": routed.provider },
    });
  }

  let preflight: PreflightedResponsesStream;
  try {
    preflight = await preflightResponsesStream(upstream.body, requestInferenceSignal);
  } catch (error) {
    const terminalType = classifyStreamFailure(error, requestInferenceSignal, req.signal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType === "cancelled") lifecycle.cancelled();
    else lifecycle.ambiguous();
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error", {
      headers: { "x-uos-upstream": routed.provider },
    });
  }
  const response = stream
    ? streamChatCompletions(
      preflight,
      model,
      usageContext,
      routed.provider,
      lifecycle,
      requestInferenceSignal,
      req.signal,
    )
    : await completeChatCompletions(
      preflight,
      model,
      usageContext,
      routed.provider,
      lifecycle,
      requestInferenceSignal,
      req.signal,
    );
  return withUosWarning(response, warnings);
};

export const handleChatCompletions = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    (context) => handleChatCompletionsInternal(req, context),
  );

const handleResponsesInternal = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const rawBody = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = rawBody as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, RESPONSES_ALLOWED_KEYS, CODEX_RESPONSES_EXTENSION_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  if (Object.prototype.hasOwnProperty.call(rawRecord, "client_metadata")) {
    const clientMetadata = rawBody.client_metadata;
    if (
      !isRecord(clientMetadata) ||
      Array.isArray(clientMetadata) ||
      Object.values(clientMetadata).some((value) => typeof value !== "string")
    ) {
      return openaiError(
        400,
        "client_metadata must be an object with string values",
        "invalid_request_error",
        { param: "client_metadata" },
      );
    }
  }
  const warnings = buildIgnoredWarnings(
    rawRecord,
    new Set([
      "model",
      "input",
      "stream",
      "reasoning",
      "instructions",
      "tools",
      "tool_choice",
      "parallel_tool_calls",
      "prompt_cache_key",
      "text",
      "include",
      "context_management",
      "client_metadata",
    ]),
  );

  const clientWantsStream = Boolean(rawBody.stream);

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  let modelRaw = (modelRawValue ?? "").trim();
  if (!modelRaw) {
    const defaultModel = await getDefaultModel();
    if (!defaultModel) return defaultModelUnavailableError();
    modelRaw = defaultModel;
  }
  const model = normalizeModelForCodex(modelRaw);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.model = modelRaw;
  const modelMetadata = await getCodexModelMetadata(model);
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, modelMetadata);
  if (modelAvailabilityError) return modelAvailabilityError;

  const inputRaw = rawBody.input;
  let input: ResponseInputItem[];
  if (inputRaw === undefined) {
    input = [];
  } else if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    const converted: ResponseInputItem[] = [];
    let contentBuffer: MessageContentItem[] = [];

    const flushContentBuffer = () => {
      if (!contentBuffer.length) return;
      converted.push({ type: "message", role: "user", content: contentBuffer });
      contentBuffer = [];
    };

    let sawNonContentItem = false;
    for (const msg of inputRaw) {
      const mapped = normalizeResponseInputItem(msg);
      if (mapped) {
        flushContentBuffer();
        converted.push(mapped);
        sawNonContentItem = true;
        continue;
      }

      if (typeof msg === "string") {
        const contentItem: MessageContentItem = { type: "input_text", text: msg };
        if (sawNonContentItem) {
          converted.push({ type: "message", role: "user", content: [contentItem] });
        } else {
          contentBuffer.push(contentItem);
        }
        continue;
      }

      const contentItem = normalizeResponseContentItem(msg);
      if (contentItem) {
        if (sawNonContentItem) {
          converted.push({ type: "message", role: "user", content: [contentItem] });
        } else {
          contentBuffer.push(contentItem);
        }
        continue;
      }

      // Codex CLI uses the Responses API and can send additional input item types
      // (e.g. reasoning + function_call + function_call_output). Pass them through
      // so tool-calling conversations work end-to-end.
      if (isRecord(msg) && typeof msg.type === "string" && msg.type !== "message") {
        flushContentBuffer();
        converted.push(msg as ResponseInputItem);
        sawNonContentItem = true;
        continue;
      }

      return openaiError(400, "Invalid message in input[]", "invalid_request_error");
    }
    if (!sawNonContentItem || contentBuffer.length) {
      flushContentBuffer();
    }
    input = converted;
  } else {
    return openaiError(400, "input must be a string or an array", "invalid_request_error");
  }

  const reasoning = parseReasoningParam(rawBody.reasoning);
  if (!reasoning.ok) return openaiError(400, reasoning.message, "invalid_request_error", { param: "reasoning" });

  let instructions = "";
  if (Object.prototype.hasOwnProperty.call(rawRecord, "instructions")) {
    const rawInstructions = getString(rawBody.instructions);
    if (rawInstructions === null) {
      return openaiError(400, "instructions must be a string", "invalid_request_error");
    }
    instructions = rawInstructions;
  }
  const defaultEffort = await getDefaultReasoningEffort();
  const modelReasoning = modelMetadata.reasoning;
  const defaultReasoningLabel = resolveDefaultReasoningLabel(modelReasoning, defaultEffort);
  const reasoningLabel = resolveReasoningLabelFromParam(reasoning.value, defaultReasoningLabel);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.reasoning = reasoningLabel;

  let reasoningValue = normalizeReasoningParamForCodex(reasoning.value, modelReasoning);
  if (reasoningValue === undefined && reasoning.value === undefined) {
    reasoningValue = { effort: reasoningEffortForCodexRequest(defaultReasoningLabel, modelReasoning) };
  }

  const codexBody = await buildCodexRequest(model, input, { reasoning: reasoningValue, instructions });
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
    "text",
    "include",
    "context_management",
  ];
  applyPassthroughToCodexRequest(codexBody, rawRecord, passthroughKeys);
  codexBody.model = model;
  codexBody.input = input;
  codexBody.stream = true;
  codexBody.store = false;

  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "responses",
    stream: clientWantsStream,
    reasoning: reasoningLabel,
  });
  const requestInferenceSignal = inferenceSignal(req);

  let routed: RoutedResponsesUpstream;
  try {
    routed = await fetchResponsesWithPaidFallback(codexBody, {
      model,
      route: "responses",
      stream: clientWantsStream,
      reasoning: reasoningLabel,
      usageContext,
      clientVersion: modelMetadata.snapshot?.client_version,
      signal: requestInferenceSignal,
    });
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error, usageContext?.responseTelemetry?.provider);
  }
  const upstream = routed.response;
  const lifecycle = createYunwuTransportLifecycle(routed.paidFallback);

  if (routed.gatewayResponse) {
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return upstream;
  }
  if (!upstream.ok) {
    lifecycle.terminal("response.failed");
    recordStreamTerminalType(usageContext, "response.failed");
    await recordErrorUsage(usageContext);
    const normalized = await toOpenAiUpstreamErrorResponse(upstream, routed.provider);
    return attachResponseTelemetry(normalized, usageContext?.responseTelemetry ?? createResponseTelemetryState());
  }

  if (clientWantsStream) {
    if (!upstream.body) {
      lifecycle.ambiguous();
      recordStreamTerminalType(usageContext, "error");
      await recordErrorUsage(usageContext);
      return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body", {
        headers: { "x-uos-upstream": routed.provider },
      });
    }
    let preflight: PreflightedResponsesStream;
    try {
      preflight = await preflightResponsesStream(upstream.body, requestInferenceSignal);
    } catch (error) {
      const terminalType = classifyStreamFailure(error, requestInferenceSignal, req.signal);
      recordStreamTerminalType(usageContext, terminalType);
      if (terminalType === "cancelled") lifecycle.cancelled();
      else lifecycle.ambiguous();
      await recordErrorUsage(usageContext);
      return openaiError(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error", {
        headers: { "x-uos-upstream": routed.provider },
      });
    }
    const headers = new Headers(upstream.headers);
    // The gateway always emits the Responses wire format as SSE. Some
    // compatible upstreams omit (or mislabel) this header; preserve the
    // stream contract so handler-level terminal logging and clients consume
    // it as an event stream.
    headers.set("Content-Type", "text/event-stream");
    headers.set("x-uos-upstream", routed.provider);
    const responseBody = proxyResponsesStreamIterator(preflight.iterator, {
      signal: requestInferenceSignal,
      downstreamSignal: req.signal,
      onEvent: (event) => {
        lifecycle.terminal(event.type);
        recordResponsesTerminal(event, usageContext);
      },
      onFailure: (error) => {
        const terminalType = classifyStreamFailure(error, requestInferenceSignal, req.signal);
        recordStreamTerminalType(usageContext, terminalType);
        if (terminalType === "cancelled") lifecycle.cancelled();
        else lifecycle.ambiguous();
        void recordErrorUsage(usageContext);
      },
      onCancel: () => {
        recordStreamTerminalType(usageContext, "cancelled");
        lifecycle.cancelled();
        void recordErrorUsage(usageContext);
      },
    }, preflight.first);
    const response = new Response(responseBody, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
    return withUosWarning(response, warnings);
  }

  if (!upstream.body) {
    lifecycle.ambiguous();
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body", {
      headers: { "x-uos-upstream": routed.provider },
    });
  }

  let finalResponse: Record<string, unknown> | null = null;
  let finalEventType: string | null = null;
  let outputText = "";
  const outputItems: Record<string, unknown>[] = [];
  try {
    for await (const event of readResponsesStream(upstream.body, requestInferenceSignal)) {
      const ev = event.value;
      if (event.terminal) {
        lifecycle.terminal(event.type);
        recordStreamTerminalType(usageContext, event.type as ResponseStreamTerminalType);
      }
      if (event.type === "response.output_text.delta") {
        outputText += getString(ev.delta) ?? "";
        continue;
      }
      if (event.type === "response.output_item.done" && isRecord(ev.item)) {
        outputItems.push(ev.item);
        continue;
      }
      if (
        (event.type === "response.completed" || event.type === "response.failed" ||
          event.type === "response.incomplete") &&
        isRecord(ev.response)
      ) {
        finalResponse = ev.response;
        finalEventType = event.type;
        break;
      }
    }
  } catch (error) {
    const terminalType = classifyStreamFailure(error, requestInferenceSignal, req.signal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType === "cancelled") lifecycle.cancelled();
    else lifecycle.ambiguous();
    finalResponse = null;
  }
  if (!finalResponse) {
    lifecycle.ambiguous();
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error", {
      headers: { "x-uos-upstream": routed.provider },
    });
  }
  finalResponse = withAccumulatedResponseItems(finalResponse, outputItems);
  finalResponse = withAccumulatedResponseText(finalResponse, outputText);
  const usageTokens = extractUsageTokens(finalResponse.usage);
  if (finalEventType === "response.failed" || finalEventType === "response.incomplete") {
    await recordErrorUsage(usageContext);
  } else await recordCompletionUsage(usageContext, usageTokens);
  const response = json(200, finalResponse, { "x-uos-upstream": routed.provider });
  return withUosWarning(response, warnings);
};

export const handleResponses = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    (context) => handleResponsesInternal(req, context),
  );
