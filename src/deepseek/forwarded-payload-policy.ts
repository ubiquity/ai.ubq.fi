import { DEEPSEEK_CONTEXT_WINDOW_TOKENS } from "./index.ts";
import { CEREBRAS_GPT_OSS_120B_MODEL } from "../provider/cerebras.ts";
import { LITHOS_CONTEXT_WINDOW_TOKENS } from "../provider/lithos.ts";
import { cerebrasProviderHint } from "../request-policy.ts";
import { getString, isRecord } from "../utils.ts";
import { estimateTokens } from "../../lib/jev_compaction/state.ts";

/**
 * The declared, versioned policy that bounds how much payload this gateway
 * forwards inside one Chat Completions message.
 *
 * Why this exists: both providers on this translation count forwarded bytes -
 * base64 image data URLs and raw tool results alike - as text tokens. On
 * 2026-09-24 a single 744,586-byte `view_image` result took one session from
 * 736,213 requested tokens to 1,250,713 against a 1,048,576-token window; every
 * later request replayed the same blob and was rejected, so the thread could
 * not be resumed.
 *
 * The first stopgap cut each payload at an undeclared 64 KiB constant. This
 * policy replaces it: the limit is declared here, advertised to Codex clients
 * through the model catalog's `forwarding_policy` extension, carried in every
 * elision marker and operator log line, and versioned so a change is visible
 * rather than silent.
 *
 * - `mode: "bytes"` counts the UTF-8 bytes of one forwarded message payload.
 * - `perMessageLimit` bounds the forwarded payload including its elision marker.
 * - Reduction is deterministic (a byte prefix plus a marker naming the counts)
 *   and applies only when the request did not forbid it: an explicit
 *   `truncation: "disabled"` fails closed with HTTP 400
 *   `context_length_exceeded` instead of mutating the input.
 * - One exact stock Codex local-compaction summary envelope is exempt from the
 *   per-message cut. The envelope is a user message with one `input_text` part
 *   whose text begins with the exact 399-byte Codex `SUMMARY_PREFIX` and a
 *   newline. The prefix is request text, not proof of authority: it only selects
 *   the versioned exception, and the complete projected prompt still has to fit
 *   the selected provider's aggregate admission.
 * - Aggregate admission is a separate, versioned control over the serialized
 *   Chat body after every field is projected. UTF-8 bytes plus explicit
 *   structural overhead bound the prompt against the provider window minus an
 *   output reserve, so tools, instructions, arguments, reasoning, images, and
 *   transport structure cannot hide outside the per-message checks.
 */
export const COMPACTION_SUMMARY_POLICY = {
  version: "codex-compaction-summary/v1",
  prefix:
    "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:",
  prefixBytes: 399,
  prefixSha256: "e9b088e794a6bb9082ac053fcc760bd818d7e720ee4bcdc72c6e480de7b7cb0e",
  maxPerRequest: 1,
} as const;

export const FORWARDED_PAYLOAD_POLICY = {
  version: "deepseek-forwarded-payload/v2",
  mode: "bytes",
  perMessageLimit: 65_536,
  summaryExemption: COMPACTION_SUMMARY_POLICY,
} as const;

/**
 * The aggregate bounds are declared once and applied to every Chat provider.
 * The existing Jev estimator counts word pieces and was calibrated at 2-18%
 * above measured usage on real transcripts; its estimate is added to explicit
 * structural terms for chat framing that is absent from JSON serialization.
 * The reserve is the larger of this floor and the request's own `max_tokens`,
 * because a provider must fit both prompt and completion inside its window.
 */
export const PROMPT_ADMISSION_POLICY = {
  version: "chat-prompt-admission/v1",
  minimumOutputReserveTokens: 16_384,
  requestStructuralOverheadTokens: 256,
  messageStructuralOverheadTokens: 16,
  toolStructuralOverheadTokens: 32,
} as const;

/** Each Chat-only provider profile keeps its own measured context window. */
export const PROMPT_ADMISSION_PROFILE_WINDOWS = {
  deepseek: DEEPSEEK_CONTEXT_WINDOW_TOKENS,
  lithos: LITHOS_CONTEXT_WINDOW_TOKENS,
  // Cerebras publishes no `/v1/models` context length; its route hint owns the
  // measured 131,072-token paid window. A missing hint fails admission closed.
  cerebras: cerebrasProviderHint(CEREBRAS_GPT_OSS_120B_MODEL).context_window_tokens ?? 0,
} as const;

/** Reads one provider's admission window, or null when no verified window exists. */
export const promptWindowTokensForProfile = (profile: keyof typeof PROMPT_ADMISSION_PROFILE_WINDOWS): number | null => {
  const window = PROMPT_ADMISSION_PROFILE_WINDOWS[profile];
  return Number.isSafeInteger(window) && window > 0 ? window : null;
};

/** The aggregate budget and observed cost for one complete projected Chat body. */
export type PromptAdmission = Readonly<{
  policy: string;
  estimatedPromptTokens: number;
  serializedBodyBytes: number;
  structuralOverheadTokens: number;
  requestedOutputTokens: number | null;
  outputReserveTokens: number;
  contextWindowTokens: number;
  promptBudgetTokens: number;
  admitted: boolean;
}>;

/** Estimates one complete projected Chat prompt against one provider window. */
export const estimatePromptAdmission = (
  body: Record<string, unknown>,
  contextWindowTokens: number,
  projectBody?: (body: Record<string, unknown>) => Record<string, unknown>
): PromptAdmission => {
  let projectedBody = body;
  if (projectBody) {
    try {
      projectedBody = projectBody(body);
    } catch {
      // The transport owns the projection error. Estimation must stay conservative.
      projectedBody = body;
    }
  }
  const serializedBody = JSON.stringify(projectedBody);
  const serializedBodyBytes = new TextEncoder().encode(serializedBody).byteLength;
  const messages = Array.isArray(projectedBody.messages) ? projectedBody.messages : [];
  const tools = Array.isArray(projectedBody.tools) ? projectedBody.tools : [];
  const structuralOverheadTokens =
    PROMPT_ADMISSION_POLICY.requestStructuralOverheadTokens +
    messages.length * PROMPT_ADMISSION_POLICY.messageStructuralOverheadTokens +
    tools.length * PROMPT_ADMISSION_POLICY.toolStructuralOverheadTokens;
  const estimatedPromptTokens = estimateTokens(serializedBody) + structuralOverheadTokens;
  const requestedOutputTokens =
    typeof projectedBody.max_tokens === "number" && Number.isSafeInteger(projectedBody.max_tokens) && projectedBody.max_tokens > 0
      ? projectedBody.max_tokens
      : null;
  const outputReserveTokens = Math.max(PROMPT_ADMISSION_POLICY.minimumOutputReserveTokens, requestedOutputTokens ?? 0);
  const promptBudgetTokens = Math.max(0, contextWindowTokens - outputReserveTokens);
  return {
    policy: PROMPT_ADMISSION_POLICY.version,
    estimatedPromptTokens,
    serializedBodyBytes,
    structuralOverheadTokens,
    requestedOutputTokens,
    outputReserveTokens,
    contextWindowTokens,
    promptBudgetTokens,
    admitted: estimatedPromptTokens <= promptBudgetTokens,
  };
};

/** The client-facing explanation for an aggregate admission refusal. */
export const promptAdmissionFailureMessage = (admission: PromptAdmission, providerLabel: string): string =>
  `the complete projected ${providerLabel} prompt is an estimated ${admission.estimatedPromptTokens} tokens (the calibrated estimator over ${admission.serializedBodyBytes} serialized UTF-8 bytes plus ${admission.structuralOverheadTokens} structural tokens), above the ${admission.promptBudgetTokens}-token budget from a ${admission.contextWindowTokens}-token window minus a ${admission.outputReserveTokens}-token output reserve under ${admission.policy}. Reduce the prompt; no payload was dispatched.`;

/** The reduction decision one request authorizes for oversized forwarded payloads. */
export type ForwardedPayloadReduction = "reduce" | "reject";

/** One payload the declared policy reduced, as recorded in the operator log. */
export type ForwardedPayloadElision = Readonly<{
  path: string;
  callId: string | null;
  kind: "tool_output" | "tool_arguments" | "image" | "agent_message" | "message" | "reasoning";
  originalBytes: number;
  forwardedBytes: number;
  omittedBytes: number;
}>;

const forwardedBytes = (value: string): number => new TextEncoder().encode(value).byteLength;

/** Replaces oversized function-call arguments with a parseable JSON elision envelope. */
export const reduceToolArgumentsPayload = (
  value: string,
  path: string,
  callId: string,
  limit: number
): Readonly<{ content: string; elision: ForwardedPayloadElision }> | null => {
  const originalBytes = forwardedBytes(value);
  const message = `[gateway: ${FORWARDED_PAYLOAD_POLICY.version} elided ${originalBytes} bytes omitted; the model did not receive these tool-call arguments; this route forwards at most ${limit} bytes per message]`;
  const content = JSON.stringify({
    gateway_elision: {
      policy: FORWARDED_PAYLOAD_POLICY.version,
      kind: "tool_arguments",
      originalBytes,
      omittedBytes: originalBytes,
      message,
    },
  });
  const contentBytes = forwardedBytes(content);
  if (contentBytes > limit) return null;
  return {
    content,
    elision: {
      path,
      callId,
      kind: "tool_arguments",
      originalBytes,
      forwardedBytes: contentBytes,
      omittedBytes: originalBytes,
    },
  };
};

/** One exact stock local-compaction summary forwarded above the generic bound. */
export type ForwardedPayloadExemption = Readonly<{
  path: string;
  kind: "compaction_summary";
  summaryPolicy: string;
  prefixSha256: string;
  bytes: number;
  contentParts: 1;
  perMessageLimit: number;
}>;

/** Builds the operator record for one exact summary admitted above the generic bound. */
export const compactionSummaryExemption = (path: string, bytes: number): ForwardedPayloadExemption => ({
  path,
  kind: "compaction_summary",
  summaryPolicy: COMPACTION_SUMMARY_POLICY.version,
  prefixSha256: COMPACTION_SUMMARY_POLICY.prefixSha256,
  bytes,
  contentParts: 1,
  perMessageLimit: FORWARDED_PAYLOAD_POLICY.perMessageLimit,
});

/** The result of inspecting a Responses input for the one allowed summary envelope. */
export type CompactionSummaryInspection = Readonly<{ ok: true; index: number | null }> | Readonly<{ ok: false; path: string; message: string }>;

const malformedSummaryMessage = (detail: string): string =>
  `${detail}; ${COMPACTION_SUMMARY_POLICY.version} exempts at most ${COMPACTION_SUMMARY_POLICY.maxPerRequest} exact user input_text envelope beginning with the ${COMPACTION_SUMMARY_POLICY.prefixBytes}-byte Codex summary prefix and a newline. The summary was not reduced or forwarded.`;

const summaryFailure = (path: string, detail: string): CompactionSummaryInspection => ({ ok: false, path, message: malformedSummaryMessage(detail) });

const summaryContent = (item: Record<string, unknown>): Readonly<{ content: unknown; parts: unknown[]; texts: (string | null)[] }> => {
  const content = item.content;
  const parts = Array.isArray(content) ? content : [];
  const texts = typeof content === "string" ? [content] : parts.map((part) => (isRecord(part) ? getString(part.text) : null));
  return { content, parts, texts };
};

const isExactSummaryEnvelope = (item: Record<string, unknown>, content: unknown, parts: unknown[]): boolean => {
  const part = parts[0];
  return (
    (getString(item.type) ?? "message") === "message" &&
    getString(item.role) === "user" &&
    Array.isArray(content) &&
    content.length === 1 &&
    isRecord(part) &&
    !Array.isArray(part) &&
    getString(part.type) === "input_text" &&
    typeof part.text === "string" &&
    part.text.startsWith(`${COMPACTION_SUMMARY_POLICY.prefix}\n`) &&
    part.text.length > COMPACTION_SUMMARY_POLICY.prefix.length + 1
  );
};

const stringSummaryFailure = (value: string, path: string, detail: string): CompactionSummaryInspection | null =>
  value.startsWith(COMPACTION_SUMMARY_POLICY.prefix) ? summaryFailure(path, detail) : null;

const inspectSummaryItem = (item: unknown, index: number): CompactionSummaryInspection | null => {
  const path = `input[${index}]`;
  if (typeof item === "string") return stringSummaryFailure(item, path, "is a string item carrying a summary prefix");
  if (!isRecord(item) || Array.isArray(item)) return null;
  const { content, parts, texts } = summaryContent(item);
  if (!texts.some((text) => text?.startsWith(COMPACTION_SUMMARY_POLICY.prefix))) return null;
  if (!isExactSummaryEnvelope(item, content, parts))
    return summaryFailure(path, "carries a summary prefix outside the exact one-part user input_text envelope");
  return { ok: true, index };
};

/**
 * Finds the one readable local-compaction summary Codex can adopt. Codex
 * 0.163.0 strips `internal_chat_message_metadata_passthrough` for every
 * non-OpenAI provider, so the exact envelope itself is the only accepted
 * classifier. A request with two summary envelopes, or a prefix-bearing
 * message that is not exactly one user input_text part, fails closed.
 */
export const inspectCompactionSummaryInput = (input: unknown): CompactionSummaryInspection => {
  if (typeof input === "string") return stringSummaryFailure(input, "input", "is a string carrying a summary prefix") ?? { ok: true, index: null };
  if (!Array.isArray(input)) return { ok: true, index: null };
  let found: number | null = null;
  for (let index = 0; index < input.length; index += 1) {
    const inspection = inspectSummaryItem(input[index], index);
    if (inspection === null) continue;
    if (!inspection.ok) return inspection;
    if (found !== null) return summaryFailure(`input[${index}]`, "adds a second summary envelope");
    found = index;
  }
  return { ok: true, index: found };
};

/** Records each exact summary admitted whole above the generic per-message bound. */
export const logForwardedPayloadExemptions = (
  exemptions: readonly ForwardedPayloadExemption[],
  context: Readonly<{ requestId?: string; model?: string; promptAdmission?: PromptAdmission }> = {}
): void => {
  for (const exemption of exemptions) {
    console.info(
      "[ai.ubq.fi] forwarding_exemption",
      JSON.stringify({
        policy: FORWARDED_PAYLOAD_POLICY.version,
        request_id: context.requestId ?? null,
        model: context.model ?? null,
        prompt_admission: context.promptAdmission ?? null,
        ...exemption,
        prefixBytes: COMPACTION_SUMMARY_POLICY.prefixBytes,
      })
    );
  }
};
