// Request handler for the `ubiquity/deepseek-v4.1-flash` synthetic model:
// dispatch each hop of the measured provider waterfall in order and advance
// only on infrastructure or serving failures. See
// docs/deepseek-waterfall-model-plan.md for the semantics and phase 2 scope.

import { openaiError } from "../http.ts";
import { recordAttemptedProvider, type DeepSeekWaterfallFallbackReason, type UsageContext } from "../openai-telemetry.ts";
import { isProviderEnabled, loadProviderSelectionCached, type ProviderSelection } from "../provider/selection.ts";
import { handleLithosResponses } from "../provider/lithos-handlers.ts";
import { handleOpenRouterResponses } from "../provider/openrouter-handlers.ts";
import { handleDeepSeekResponses } from "./handlers.ts";
import {
  DEEPSEEK_WATERFALL_MODEL_ID,
  DEEPSEEK_WATERFALL_PROVIDER_MODEL,
  DEEPSEEK_WATERFALL_REASONING_LEVELS,
  type DeepSeekWaterfallProvider,
  deepSeekWaterfallFailureReason,
  deepSeekWaterfallPlan,
  deepSeekWaterfallProviderConfigured,
  deepSeekWaterfallRequestedEffort,
  isDeepSeekWaterfallReasoningLevel,
  isDeepSeekWaterfallRetryStatus,
} from "./waterfall.ts";

export type DeepSeekWaterfallDispatch = (
  input: Readonly<{
    provider: DeepSeekWaterfallProvider;
    req: Request;
    rawRecord: Record<string, unknown>;
    model: string;
    usageContext: UsageContext | undefined;
  }>
) => Promise<Response>;

export type DeepSeekWaterfallOptions = Readonly<{
  /** Injected transports; production wires the three direct provider handlers. */
  dispatch?: DeepSeekWaterfallDispatch;
  /** Overrides for tests; production reads the operator selection and credentials. */
  selection?: ProviderSelection | null;
  enabled?: (provider: DeepSeekWaterfallProvider) => boolean;
  configured?: (provider: DeepSeekWaterfallProvider) => boolean;
}>;

const defaultDispatch: DeepSeekWaterfallDispatch = ({ provider, req, rawRecord, model, usageContext }) => {
  if (provider === "openrouter") return handleOpenRouterResponses(req, rawRecord, model, usageContext);
  if (provider === "lithos") return handleLithosResponses(req, rawRecord, model, usageContext);
  return handleDeepSeekResponses(req, rawRecord, model, usageContext);
};

const withAttemptedProviders = (response: Response, attempted: readonly string[]): Response => {
  const headers = new Headers(response.headers);
  headers.set("x-uos-attempted-providers", attempted.join(","));
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
};

const unsupportedEffortResponse = (effort: string): Response =>
  openaiError(
    400,
    `The model '${DEEPSEEK_WATERFALL_MODEL_ID}' supports reasoning efforts ${DEEPSEEK_WATERFALL_REASONING_LEVELS.join(", ")}; '${effort}' is not supported.`,
    "unsupported_reasoning_effort",
    { param: "reasoning.effort" }
  );

const unavailableResponse = (): Response =>
  openaiError(503, "No DeepSeek waterfall provider is enabled and configured.", "deepseek_waterfall_unavailable", { type: "server_error" });

const transportFailureResponse = (provider: DeepSeekWaterfallProvider): Response =>
  openaiError(502, `DeepSeek waterfall provider '${provider}' failed before responding.`, "deepseek_waterfall_transport_failure", { type: "server_error" });

type HopResult = Readonly<{ delivered: boolean; response: Response; failure: number | "transport" }>;

const attemptHop = async (
  dispatch: DeepSeekWaterfallDispatch,
  provider: DeepSeekWaterfallProvider,
  req: Request,
  rawRecord: Record<string, unknown>,
  usageContext: UsageContext | undefined
): Promise<HopResult> => {
  const model = DEEPSEEK_WATERFALL_PROVIDER_MODEL[provider];
  try {
    const response = await dispatch({ provider, req, rawRecord: { ...rawRecord, model }, model, usageContext });
    if (isDeepSeekWaterfallRetryStatus(response.status)) return { delivered: false, response, failure: response.status };
    return { delivered: true, response, failure: 0 };
  } catch (error) {
    // A client cancellation is not a provider failure and must not spend the
    // next hop's latency budget.
    if (req.signal.aborted) throw error;
    return { delivered: false, response: transportFailureResponse(provider), failure: "transport" };
  }
};

export const handleDeepSeekWaterfallResponses = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  usageContext?: UsageContext,
  options: DeepSeekWaterfallOptions = {}
): Promise<Response> => {
  const effort = deepSeekWaterfallRequestedEffort(rawRecord);
  if (effort !== null && !isDeepSeekWaterfallReasoningLevel(effort)) return unsupportedEffortResponse(effort);

  const selection = options.selection === undefined ? await loadProviderSelectionCached() : options.selection;
  const enabled = options.enabled ?? ((provider: DeepSeekWaterfallProvider) => isProviderEnabled(provider, selection));
  const configured = options.configured ?? deepSeekWaterfallProviderConfigured;
  const plan = deepSeekWaterfallPlan({ enabled, configured });
  if (!plan.length) return unavailableResponse();

  const dispatch = options.dispatch ?? defaultDispatch;
  const attempted: string[] = [];
  let fallbackReason: DeepSeekWaterfallFallbackReason | null = null;
  let finalResponse = unavailableResponse();
  for (const provider of plan) {
    if (req.signal.aborted) break;
    const result = await attemptHop(dispatch, provider, req, rawRecord, usageContext);
    attempted.push(provider);
    recordAttemptedProvider(usageContext, provider);
    finalResponse = result.response;
    if (result.delivered) break;
    fallbackReason ??= deepSeekWaterfallFailureReason(provider, result.failure);
  }
  if (fallbackReason !== null && usageContext?.responseTelemetry) usageContext.responseTelemetry.fallbackReason = fallbackReason;
  return withAttemptedProviders(finalResponse, attempted);
};
