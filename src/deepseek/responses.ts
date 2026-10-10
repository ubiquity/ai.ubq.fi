import {
  DeepSeekError,
  deepSeekCachedPromptTokens,
  type DeepSeekFinishDisposition,
  deepSeekFinishDisposition,
  deepSeekReasoningTokens,
  deepSeekThinkingModeActive,
  deepSeekThinkingToolChoiceConflict,
  deepSeekToolChoiceThinkingConflictMessage,
  deepSeekUpstreamModelFor,
  projectDeepSeekReasoningEffort,
  readDeepSeekApiKey,
} from "./index.ts";
import { LITHOS_REASONING_LEVELS, lithosCachedPromptTokens, lithosReasoningTokens, lithosUpstreamModelFor, requireLithosApiKey } from "../provider/lithos.ts";
import {
  CEREBRAS_MODELS,
  CerebrasError,
  cerebrasCachedPromptTokens,
  cerebrasReasoningTokens,
  cerebrasUpstreamModelFor,
  readCerebrasApiKey,
} from "../provider/cerebras.ts";
import { cerebrasProviderHint } from "../request-policy.ts";

/**
 * Responses <-> DeepSeek Chat Completions adapter.
 *
 * The Codex CLI speaks only the Responses API (`wire_api = "chat"` was removed
 * from the client), so this module translates a Responses request into a Chat
 * Completions body, a Chat completion back into a Responses object, and a Chat
 * SSE stream into the Responses event sequence the client consumes.
 *
 * The translation is not required by a provider gap: DeepSeek now serves a
 * native Responses endpoint (`POST /responses` and `/v1/responses`, probed
 * 2026-09-21). It stays the right seam because the provider's native endpoint is
 * documented as stateless with several control parameters ignored, and because
 * the `reasoning_content` fill this adapter applies to tool-bearing tails is a
 * measured provider requirement a native response would have to reproduce. A
 * migration is a separate evidence-driven evaluation, not an assumed cure.
 *
 * Only the gateway-known subset is translated. Anything else fails closed with
 * an `invalid_request_error` rather than being forwarded as an approximation.
 *
 * Provider-specific behavior is not hard-coded here: one `ChatOnlyResponsesProfile`
 * per upstream carries the model table, the reasoning projection, the finish
 * vocabulary, the usage counters and the capability checks, and the exported
 * translations take the profile they serve. DeepSeek and LithosAI therefore
 * share this one translation instead of a second copy drifting from it. Every
 * entry point defaults to the DeepSeek profile, so the route that shipped
 * against this module is unchanged.
 */

export type DeepSeekResponsesFailure = Readonly<{ ok: false; message: string; param: string; code?: string }>;

export type DeepSeekResponsesResult<T> = Readonly<{ ok: true; value: T }> | DeepSeekResponsesFailure;

export const failure = (param: string, message: string, code?: string): DeepSeekResponsesFailure => ({
  ok: false,
  message,
  param,
  ...(code === undefined ? {} : { code }),
});

/**
 * One provider's answers to the questions this translation asks. A provider
 * difference belongs in its profile; the translation itself stays shared.
 *
 * `projectReasoningEffort` returns null when the provider does not accept the
 * requested tier, and the request then fails closed with an
 * `invalid_request_error` rather than sending a value the provider refuses.
 */
export type ChatOnlyResponsesProfile = Readonly<{
  id: "deepseek" | "lithos" | "cerebras";
  /** Human name used in client-facing error text. */
  label: string;
  /** Canonical upstream model for a client-facing id, or null when the id is not this provider's. */
  upstreamModelFor: (model: string) => string | null;
  projectReasoningEffort: (effort: string) => string | null;
  /** The existing finish vocabulary, reused: both providers report the same reasons this adapter maps. */
  finishDisposition: (reason: unknown) => DeepSeekFinishDisposition;
  thinkingToolChoiceConflict: (reasoningEffort: unknown, thinking: unknown, toolChoice: unknown) => string | null;
  toolChoiceThinkingConflictMessage: (conflict: string, field: string) => string;
  cachedPromptTokens: (value: Record<string, unknown>, promptTokens: number) => number | null;
  reasoningTokens: (value: Record<string, unknown>, completionTokens: number) => number | null;
  /** Throws the provider's own not-configured error when its API key is absent. */
  requireApiKey: () => void;
  /** True when the provider reports stream usage only behind `stream_options.include_usage`. */
  requiresStreamUsageOption: boolean;
  /** True when the resolved mode permits sampling controls (temperature, top_p). */
  allowsSamplingControls: (reasoningEffort: unknown, thinking: unknown) => boolean;
}>;

/** The provider's own key check, mirroring `requireDeepSeekApiKey` in `./deepseek.ts`. */
const requireDeepSeekResponsesApiKey = (): void => {
  if (!readDeepSeekApiKey()) throw new DeepSeekError("The requested model is not configured.", "deepseek_api_key_missing", 503);
};

export const DEEPSEEK_RESPONSES_PROFILE: ChatOnlyResponsesProfile = {
  id: "deepseek",
  label: "DeepSeek",
  upstreamModelFor: deepSeekUpstreamModelFor,
  projectReasoningEffort: (effort: string) => projectDeepSeekReasoningEffort(effort),
  finishDisposition: deepSeekFinishDisposition,
  thinkingToolChoiceConflict: deepSeekThinkingToolChoiceConflict,
  toolChoiceThinkingConflictMessage: deepSeekToolChoiceThinkingConflictMessage,
  cachedPromptTokens: deepSeekCachedPromptTokens,
  reasoningTokens: deepSeekReasoningTokens,
  requireApiKey: requireDeepSeekResponsesApiKey,
  // DeepSeek reports usage on its final content chunk only when the request
  // asks for it, so the streaming body carries `stream_options.include_usage`.
  requiresStreamUsageOption: true,
  allowsSamplingControls: (reasoningEffort: unknown, thinking: unknown): boolean => !deepSeekThinkingModeActive(reasoningEffort, thinking),
};

/** The seven tiers this provider accepted on 2026-09-23, as a membership set. */
const LITHOS_REASONING_LEVEL_SET: ReadonlySet<string> = new Set(LITHOS_REASONING_LEVELS);

/**
 * Projects a requested reasoning tier onto the LithosAI wire value, or null
 * when the provider does not accept it.
 *
 * The tier is sent verbatim: unlike DeepSeek there is no advanced Codex preset
 * to translate, and `ultra` is NOT mapped to `max` because this provider
 * refuses `ultra`. The provider accepted exactly these seven lowercase names,
 * so a case-insensitive match forwards the canonical spelling and everything
 * else fails closed instead of being sent.
 */
const projectLithosReasoningEffort = (effort: string): string | null => {
  const level = effort.trim().toLowerCase();
  return LITHOS_REASONING_LEVEL_SET.has(level) ? level : null;
};

/**
 * No thinking-mode `tool_choice` restriction was observed or documented on this
 * provider, so this profile reports no conflict rather than importing DeepSeek's
 * measured restriction. The message builder completes the profile contract; it
 * is unreachable while this function returns null.
 */
const lithosThinkingToolChoiceConflict = (_reasoningEffort: unknown, _thinking: unknown, _toolChoice: unknown): string | null => null;

const lithosToolChoiceThinkingConflictMessage = (conflict: string, field: string): string =>
  `tool_choice '${conflict}' is not supported while ${field} keeps LithosAI thinking mode active; set ${field} to 'none' or use tool_choice 'auto'`;

/**
 * The LithosAI profile.
 *
 * The usage counters are the transport's own exported guards
 * (`lithosCachedPromptTokens`, `lithosReasoningTokens`), so the transport and
 * this adapter cannot drift on what a readable measurement is. Only the tier
 * predicate stays local, because it is this adapter's request-side decision:
 * `src/lithos.ts` forwards whatever tier it is given and never judges one.
 */
export const LITHOS_RESPONSES_PROFILE: ChatOnlyResponsesProfile = {
  id: "lithos",
  label: "LithosAI",
  upstreamModelFor: lithosUpstreamModelFor,
  projectReasoningEffort: projectLithosReasoningEffort,
  // `length` means the same output-budget truncation DeepSeek reports, so the
  // one shared mapping is reused rather than a second vocabulary invented.
  finishDisposition: deepSeekFinishDisposition,
  thinkingToolChoiceConflict: lithosThinkingToolChoiceConflict,
  toolChoiceThinkingConflictMessage: lithosToolChoiceThinkingConflictMessage,
  cachedPromptTokens: lithosCachedPromptTokens,
  reasoningTokens: lithosReasoningTokens,
  requireApiKey: requireLithosApiKey,
  // Usage arrives unconditionally on every streaming call and is never gated
  // on `stream_options.include_usage`, which this provider's wire does not use.
  requiresStreamUsageOption: false,
  allowsSamplingControls: (reasoningEffort: unknown, thinking: unknown): boolean => !deepSeekThinkingModeActive(reasoningEffort, thinking),
};

/** The provider's own key check, mirroring `requireCerebrasApiKey` in `../provider/cerebras.ts`. */
const requireCerebrasResponsesApiKey = (): void => {
  if (!readCerebrasApiKey()) throw new CerebrasError("The requested model is not configured.", "cerebras_api_key_missing", 503);
};

/**
 * Every tier the Cerebras route advertises for any of its ids, as a membership
 * set. The per-id lists live with the route's hints and are enforced by the
 * Cerebras transport, which refuses a tier the requested model does not accept;
 * this adapter only has to stop a tier the route never advertised at all.
 */
const CEREBRAS_REASONING_LEVEL_SET: ReadonlySet<string> = new Set(
  CEREBRAS_MODELS.flatMap((id) =>
    (cerebrasProviderHint(id).supported_reasoning_levels ?? []).filter((level): level is string => typeof level === "string")
  ).map((level) => level.trim().toLowerCase())
);

/**
 * Projects a requested reasoning tier onto the Cerebras wire value, or null
 * when no id on this route advertises it.
 *
 * The tier is sent verbatim, exactly like LithosAI: there is no advanced Codex
 * preset to translate, and `ultra` is NOT mapped to `max` because this route
 * refuses it. Whether the requested model itself accepts the (advertised) tier
 * is the transport's per-id decision, so `none` reaches `gpt-oss-120b` and is
 * refused there rather than being pre-filtered here against one model's list.
 */
const projectCerebrasReasoningEffort = (effort: string): string | null => {
  const level = effort.trim().toLowerCase();
  return CEREBRAS_REASONING_LEVEL_SET.has(level) ? level : null;
};

/**
 * No thinking-mode `tool_choice` restriction was observed or documented on this
 * provider, so this profile reports no conflict rather than importing DeepSeek's
 * measured restriction. The message builder completes the profile contract; it
 * is unreachable while this function returns null.
 */
const cerebrasThinkingToolChoiceConflict = (_reasoningEffort: unknown, _thinking: unknown, _toolChoice: unknown): string | null => null;

const cerebrasToolChoiceThinkingConflictMessage = (conflict: string, field: string): string =>
  `tool_choice '${conflict}' is not supported while ${field} keeps Cerebras reasoning active; set ${field} to 'none' or use tool_choice 'auto'`;

/**
 * The Cerebras profile.
 *
 * The usage counters are the transport's own exported guards
 * (`cerebrasCachedPromptTokens`, `cerebrasReasoningTokens`), so the transport
 * and this adapter cannot drift on what a readable measurement is. Only the
 * tier predicate stays local, because it is this adapter's request-side
 * decision and the provider's per-id enforcement stays in its own transport.
 */
export const CEREBRAS_RESPONSES_PROFILE: ChatOnlyResponsesProfile = {
  id: "cerebras",
  label: "Cerebras",
  upstreamModelFor: cerebrasUpstreamModelFor,
  projectReasoningEffort: projectCerebrasReasoningEffort,
  // `length` means the same output-budget truncation DeepSeek reports, so the
  // one shared mapping is reused rather than a second vocabulary invented.
  finishDisposition: deepSeekFinishDisposition,
  thinkingToolChoiceConflict: cerebrasThinkingToolChoiceConflict,
  toolChoiceThinkingConflictMessage: cerebrasToolChoiceThinkingConflictMessage,
  cachedPromptTokens: cerebrasCachedPromptTokens,
  reasoningTokens: cerebrasReasoningTokens,
  requireApiKey: requireCerebrasResponsesApiKey,
  // This route buffers the provider's non-streaming answer and replays it, so
  // there is no upstream stream that could carry `stream_options`.
  requiresStreamUsageOption: false,
  allowsSamplingControls: (reasoningEffort: unknown, thinking: unknown): boolean => !deepSeekThinkingModeActive(reasoningEffort, thinking),
};

/** The Responses identity a flattened Chat tool name was derived from. */
export type OriginalToolName = Readonly<{ name: string; namespace: string | null }>;

/** Maps a flattened Chat tool name back to the identity the client asked for. */
export const originalTool = (name: string, toolNames: ReadonlyMap<string, OriginalToolName>): OriginalToolName =>
  toolNames.get(name) ?? { name, namespace: null };

/** Maps a flattened Chat tool name back to the name the client asked for. */
export const originalToolName = (name: string, toolNames: ReadonlyMap<string, OriginalToolName>): string => originalTool(name, toolNames).name;
