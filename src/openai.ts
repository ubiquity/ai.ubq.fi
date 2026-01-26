import {
  buildCodexRequest,
  CodexError,
  fetchCodexModels,
  fetchCodexResponses,
  loadCodexModelsSnapshot,
} from "./codex.ts";
import {
  DEFAULT_MODEL,
  DEFAULT_MODEL_KEY,
  DEFAULT_REASONING_EFFORT,
  DEFAULT_REASONING_EFFORT_KEY,
  normalizeReasoningEffort,
  type ReasoningEffort,
  REASONING_EFFORTS,
} from "./defaults.ts";
import { recordApiKeyUsage } from "./analytics.ts";
import { recordKernelOrgUsage, recordKernelUsage } from "./kernel_usage.ts";
import { json, openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord } from "./utils.ts";
import type { ChatCompletionRequest, MessageContentItem, ResponseMessageItem, ResponsesRequest } from "./types.ts";

const getDefaultModel = async (): Promise<string> => {
  const kv = await kvPromise;
  if (!kv) return DEFAULT_MODEL;
  const entry = await kv.get<string>(DEFAULT_MODEL_KEY);
  const model = typeof entry.value === "string" ? entry.value.trim() : "";
  return model || DEFAULT_MODEL;
};

const getDefaultReasoningEffort = async (): Promise<ReasoningEffort> => {
  const kv = await kvPromise;
  if (!kv) return DEFAULT_REASONING_EFFORT;
  const entry = await kv.get<string>(DEFAULT_REASONING_EFFORT_KEY);
  const effort = entry.value;
  if (effort && REASONING_EFFORTS.has(effort as ReasoningEffort)) return effort as ReasoningEffort;
  return DEFAULT_REASONING_EFFORT;
};


type UsageContext = Readonly<{
  keyId: string | null;
  kernelRepo: { owner: string; repo: string } | null;
  kernelOrg: { owner: string } | null;
}>;

type UsageTokens = Readonly<{
  inputTokens: number;
  outputTokens: number;
  totalTokens: number;
}>;

type UsageDelta = Readonly<{
  request_count?: number;
  stream_request_count?: number;
  non_stream_request_count?: number;
  completed_request_count?: number;
  error_request_count?: number;
  input_tokens?: number;
  output_tokens?: number;
  total_tokens?: number;
  model?: string | null;
  reasoning?: string | null;
  route?: string | null;
  seen_at_ms?: number;
}>;

const recordUsageDelta = async (context: UsageContext | undefined, delta: UsageDelta): Promise<void> => {
  if (!context) return;
  const tasks: Promise<void>[] = [];
  if (context.keyId) tasks.push(recordApiKeyUsage(context.keyId, delta));
  if (context.kernelRepo) tasks.push(recordKernelUsage(context.kernelRepo.owner, context.kernelRepo.repo, delta));
  if (context.kernelOrg) tasks.push(recordKernelOrgUsage(context.kernelOrg.owner, delta));
  if (!tasks.length) return;
  if (tasks.length === 1) {
    await tasks[0];
    return;
  }
  await Promise.all(tasks);
};

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

const recordRequestUsage = async (
  context: UsageContext | undefined,
  details: { model: string; route: string; stream: boolean; reasoning: string | null },
): Promise<void> => {
  await recordUsageDelta(context, {
    request_count: 1,
    stream_request_count: details.stream ? 1 : 0,
    non_stream_request_count: details.stream ? 0 : 1,
    model: details.model,
    reasoning: details.reasoning,
    route: details.route,
    seen_at_ms: Date.now(),
  });
};

const recordCompletionUsage = async (
  context: UsageContext | undefined,
  usage: UsageTokens | null,
): Promise<void> => {
  await recordUsageDelta(context, {
    completed_request_count: 1,
    input_tokens: usage?.inputTokens,
    output_tokens: usage?.outputTokens,
    total_tokens: usage?.totalTokens,
  });
};

const recordErrorUsage = async (context: UsageContext | undefined): Promise<void> => {
  await recordUsageDelta(context, { error_request_count: 1 });
};

const formatErrorSnippet = (error: unknown, maxLen = 280): string => {
  const raw = error instanceof Error ? error.message : String(error);
  const trimmed = raw.trim();
  if (!trimmed) return "";
  if (trimmed.length <= maxLen) return trimmed;
  return `${trimmed.slice(0, maxLen)}...`;
};

const toCodexErrorResponse = (error: unknown): Response => {
  if (error instanceof CodexError) {
    return openaiError(error.status, error.message, error.code);
  }
  const detail = formatErrorSnippet(error);
  const message = detail ? `Codex upstream request failed: ${detail}` : "Codex upstream request failed.";
  return openaiError(502, message, "codex_upstream_unreachable");
};

const looksLikeReasoningModel = (model: string): boolean => {
  const trimmed = model.trim().toLowerCase();
  return trimmed.startsWith("gpt-5") || trimmed.startsWith("o");
};

const resolveDefaultReasoningLabel = (model: string, defaultEffort: ReasoningEffort): ReasoningEffort => {
  return looksLikeReasoningModel(model) ? defaultEffort : "none";
};

const resolveReasoningLabelFromEffort = (
  effort: ReasoningEffort | null | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (effort === undefined) return defaultLabel;
  if (effort === null) return "none";
  return effort;
};

const resolveReasoningLabelFromParam = (
  reasoning: Record<string, unknown> | null | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (reasoning === undefined) return defaultLabel;
  if (reasoning === null) return "none";
  if (!isRecord(reasoning)) return defaultLabel;
  if ("effort" in reasoning) {
    if (reasoning.effort === null) return "none";
    const effort = normalizeReasoningEffort(reasoning.effort);
    if (effort) return effort;
  }
  return defaultLabel;
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
): { ok: true; value: ReasoningEffort | null | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (typeof value !== "string") {
    return { ok: false, message: `${fieldName} must be a string or null` };
  }
  const normalized = value.trim().toLowerCase();
  if (!normalized) return { ok: false, message: `${fieldName} must be a non-empty string or null` };
  if (!REASONING_EFFORTS.has(normalized as ReasoningEffort)) {
    return { ok: false, message: `${fieldName} must be one of: none, minimal, low, medium, high, xhigh` };
  }
  return { ok: true, value: normalized as ReasoningEffort };
};

const parseReasoningParam = (
  value: unknown,
): { ok: true; value: Record<string, unknown> | null | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (!isRecord(value)) return { ok: false, message: "reasoning must be an object or null" };
  if ("effort" in value) {
    const effort = parseReasoningEffortField(value.effort, "reasoning.effort");
    if (!effort.ok) return effort;
  }
  if ("summary" in value) {
    const summary = value.summary;
    if (summary !== undefined && summary !== null && typeof summary !== "string") {
      return { ok: false, message: "reasoning.summary must be a string or null" };
    }
  }
  if ("generate_summary" in value) {
    const generateSummary = value.generate_summary;
    if (generateSummary !== undefined && generateSummary !== null && typeof generateSummary !== "string") {
      return { ok: false, message: "reasoning.generate_summary must be a string or null" };
    }
  }

  return { ok: true, value };
};

const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set([
  "messages",
  "model",
  "audio",
  "frequency_penalty",
  "function_call",
  "functions",
  "logit_bias",
  "logprobs",
  "max_completion_tokens",
  "max_tokens",
  "metadata",
  "modalities",
  "n",
  "parallel_tool_calls",
  "prediction",
  "presence_penalty",
  "prompt_cache_key",
  "prompt_cache_retention",
  "reasoning_effort",
  "response_format",
  "safety_identifier",
  "seed",
  "service_tier",
  "stop",
  "store",
  "stream",
  "stream_options",
  "temperature",
  "tool_choice",
  "tools",
  "top_logprobs",
  "top_p",
  "user",
  "verbosity",
  "web_search_options",
]);

const RESPONSES_ALLOWED_KEYS = new Set([
  "background",
  "conversation",
  "include",
  "input",
  "instructions",
  "max_output_tokens",
  "max_tool_calls",
  "metadata",
  "model",
  "parallel_tool_calls",
  "previous_response_id",
  "prompt",
  "prompt_cache_key",
  "prompt_cache_retention",
  "reasoning",
  "safety_identifier",
  "service_tier",
  "store",
  "stream",
  "stream_options",
  "temperature",
  "text",
  "tool_choice",
  "tools",
  "top_logprobs",
  "top_p",
  "truncation",
  "user",
]);

const findUnknownKey = (record: Record<string, unknown>, allowed: ReadonlySet<string>): string | null => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key)) return key;
  }
  return null;
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
      if (partType === "image_url") {
        const image = isRecord(part.image_url) ? part.image_url : null;
        url = image ? getString(image.url) : null;
      } else {
        url = getString(part.image_url);
      }
      const trimmed = (url ?? "").trim();
      if (trimmed) items.push({ type: "input_image", image_url: trimmed });
      continue;
    }
  }

  if (items.length > 0) return items;
  return [{ type: textItemType, text: "" }];
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
  const trimmed = model.trim();
  if (!trimmed) return "gpt-5.1-codex-mini";
  if (trimmed === "gpt-5.2-chat-latest") return "gpt-5.2";
  if (trimmed === "gpt-5.1-chat-latest") return "gpt-5.1";
  if (trimmed === "gpt-5-chat-latest") return "gpt-5.2";
  return trimmed;
};

const normalizeModelEntry = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = getString(value.id) ?? getString(value.slug) ?? getString(value.model) ?? getString(value.name);
  if (!id) return null;
  const normalized: Record<string, unknown> = { ...value, id };
  const object = getString(value.object);
  const ownedBy = getString(value.owned_by);
  normalized.object = object ?? "model";
  normalized.owned_by = ownedBy ?? "openai";
  return normalized;
};

const normalizeModelList = (payload: unknown): { object: "list"; data: Record<string, unknown>[] } | null => {
  if (!isRecord(payload)) return null;
  const data = Array.isArray(payload.data) ? payload.data : null;
  if (data) {
    const normalized = data.map(normalizeModelEntry).filter(Boolean) as Record<string, unknown>[];
    return { object: "list", data: normalized };
  }
  const models = Array.isArray(payload.models) ? payload.models : null;
  if (models) {
    const normalized = models.map(normalizeModelEntry).filter(Boolean) as Record<string, unknown>[];
    return { object: "list", data: normalized };
  }
  return null;
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
    if (partType === "image_url") {
      const image = isRecord(value.image_url) ? value.image_url : null;
      url = image ? getString(image.url) : null;
    } else {
      url = getString(value.image_url);
    }
    const trimmed = (url ?? "").trim();
    if (!trimmed) return null;
    return { type: "input_image", image_url: trimmed };
  }

  return null;
};

const normalizeResponseInputItem = (value: unknown): ResponseMessageItem | null => {
  if (!isRecord(value)) return null;
  const itemType = getString(value.type);
  if (itemType && itemType !== "message") return null;
  return toResponseMessageItem(value);
};

const parseSseEvents = async function* (stream: ReadableStream<Uint8Array>): AsyncGenerator<unknown> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      const normalized = buffer.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
      const parts = normalized.split("\n\n");
      buffer = parts.pop() ?? "";
      for (const part of parts) {
        if (!part.trim()) continue;
        const lines = part.split("\n");
        const dataLines = lines.filter((line) => line.startsWith("data:"));
        const data = dataLines.map((line) => line.slice(5).trim()).join("\n");
        if (!data) continue;
        try {
          yield JSON.parse(data);
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          console.warn("[ai.ubq.fi] SSE parse error:", message);
          continue;
        }
      }
    }
  } finally {
    try {
      await reader.cancel();
    } catch {
      // ignore
    }
  }
};

const collectResponsesStreamUsage = async (
  stream: ReadableStream<Uint8Array>,
  usageContext?: UsageContext,
): Promise<void> => {
  if (!usageContext?.keyId && !usageContext?.kernelRepo && !usageContext?.kernelOrg) return;
  try {
    for await (const ev of parseSseEvents(stream)) {
      if (!isRecord(ev)) continue;
      if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
        const usageTokens = extractUsageTokens(ev.response.usage);
        await recordCompletionUsage(usageContext, usageTokens);
        return;
      }
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to parse responses usage stream:", error);
  }
};

const streamChatCompletions = (upstream: Response, model: string, usageContext?: UsageContext): Response => {
  if (!upstream.body) {
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
  }

  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
      let created = Math.floor(Date.now() / 1000);
      let sentRole = false;

      try {
        for await (const ev of parseSseEvents(upstream.body!)) {
          if (!isRecord(ev)) continue;
          const type = getString(ev.type);
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
            continue;
          }

          if (type === "response.completed") {
            const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
            await recordCompletionUsage(usageContext, usageTokens);
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
            controller.close();
            return;
          }
        }

        controller.enqueue(encoder.encode("data: [DONE]\n\n"));
        controller.close();
      } catch (error) {
        controller.error(error);
      }
    },
  });

  return new Response(stream, {
    status: 200,
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      "x-ubq-upstream": "chatgpt_codex",
    },
  });
};

const completeChatCompletions = async (
  upstream: Response,
  model: string,
  usageContext?: UsageContext,
): Promise<Response> => {
  if (!upstream.body) return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");

  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let content = "";
  let usage: Record<string, unknown> | null = null;

  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    const type = getString(ev.type);
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
      break;
    }
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
  return json(200, body, { "x-ubq-upstream": "chatgpt_codex" });
};

export const handleModels = async (): Promise<Response> => {
  const snapshot = await loadCodexModelsSnapshot();
  if (snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0) {
    const normalized = normalizeModelList({ models: snapshot.models });
    if (normalized) {
      return json(200, normalized, { "x-ubq-upstream": snapshot.source || "codex_cli" });
    }
  }

  let upstream: Response;
  try {
    upstream = await fetchCodexModels();
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream models fetch failed:", error);
    return toCodexErrorResponse(error);
  }

  if (!upstream.ok) {
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  const payloadText = await upstream.text().catch(() => "");
  let parsed: unknown = null;
  try {
    parsed = payloadText ? JSON.parse(payloadText) : null;
  } catch {
    parsed = null;
  }

  const normalized = normalizeModelList(parsed);
  if (!normalized) {
    return openaiError(502, "Upstream models response did not include a model list.", "codex_upstream_invalid");
  }

  return json(200, normalized, { "x-ubq-upstream": "chatgpt_codex" });
};

export const handleChatCompletions = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
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
  const modelRaw = (modelRawValue ?? "").trim() || await getDefaultModel();
  const model = normalizeModelForCodex(modelRaw);
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return openaiError(400, "messages must be an array", "invalid_request_error");
  if (messagesRaw.length === 0) return openaiError(400, "messages must be a non-empty array", "invalid_request_error");

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) return openaiError(400, reasoningEffort.message, "invalid_request_error");

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
  const defaultReasoningLabel = resolveDefaultReasoningLabel(model, defaultEffort);
  let reasoningValue: Record<string, unknown> | null | undefined;
  if (reasoningEffort.value === undefined) {
    reasoningValue = looksLikeReasoningModel(model) ? { effort: defaultReasoningLabel } : undefined;
  } else if (reasoningEffort.value === null) {
    reasoningValue = null;
  } else {
    reasoningValue = { effort: reasoningEffort.value };
  }
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoningValue,
    instructions,
  });
  const passthroughKeys = ["tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key"];
  for (const key of passthroughKeys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = rawRecord[key];
    }
  }
  codexBody.store = false;

  const stream = Boolean(body.stream);
  const reasoningLabel = resolveReasoningLabelFromEffort(reasoningEffort.value, defaultReasoningLabel);
  await recordRequestUsage(usageContext, { model: modelRaw, route: "chat.completions", stream, reasoning: reasoningLabel });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error);
  }

  if (!upstream.ok) {
    await recordErrorUsage(usageContext);
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  if (!upstream.body) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
  }

  const response = stream ? streamChatCompletions(upstream, model, usageContext) : await completeChatCompletions(
    upstream,
    model,
    usageContext,
  );
  return withUosWarning(response, warnings);
};

export const handleResponses = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const rawBody = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = rawBody as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, RESPONSES_ALLOWED_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
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
    ]),
  );

  const clientWantsStream = Boolean(rawBody.stream);

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  const modelRaw = (modelRawValue ?? "").trim() || await getDefaultModel();
  const model = normalizeModelForCodex(modelRaw);

  const inputRaw = rawBody.input;
  let input: ResponseMessageItem[];
  if (inputRaw === undefined) {
    input = [];
  } else if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    const converted: ResponseMessageItem[] = [];
    let contentBuffer: MessageContentItem[] = [];

    const flushContentBuffer = () => {
      if (!contentBuffer.length) return;
      converted.push({ type: "message", role: "user", content: contentBuffer });
      contentBuffer = [];
    };

    let sawMessage = false;
    for (const msg of inputRaw) {
      const mapped = normalizeResponseInputItem(msg);
      if (mapped) {
        flushContentBuffer();
        converted.push(mapped);
        sawMessage = true;
        continue;
      }
      const contentItem = normalizeResponseContentItem(msg);
      if (!contentItem) return openaiError(400, "Invalid message in input[]", "invalid_request_error");
      if (sawMessage) {
        converted.push({ type: "message", role: "user", content: [contentItem] });
      } else {
        contentBuffer.push(contentItem);
      }
    }
    if (!sawMessage || contentBuffer.length) {
      flushContentBuffer();
    }
    input = converted;
  } else {
    return openaiError(400, "input must be a string or an array", "invalid_request_error");
  }

  const reasoning = parseReasoningParam(rawBody.reasoning);
  if (!reasoning.ok) return openaiError(400, reasoning.message, "invalid_request_error");

  let instructions = "";
  if (Object.prototype.hasOwnProperty.call(rawRecord, "instructions")) {
    const rawInstructions = getString(rawBody.instructions);
    if (rawInstructions === null) {
      return openaiError(400, "instructions must be a string", "invalid_request_error");
    }
    instructions = rawInstructions;
  }
  const defaultEffort = await getDefaultReasoningEffort();
  const defaultReasoningLabel = resolveDefaultReasoningLabel(model, defaultEffort);
  const reasoningLabel = resolveReasoningLabelFromParam(reasoning.value, defaultReasoningLabel);

  let reasoningValue = reasoning.value;
  if (reasoningValue === undefined && looksLikeReasoningModel(model)) {
    reasoningValue = { effort: defaultReasoningLabel };
  }

  const codexBody = await buildCodexRequest(model, input, { reasoning: reasoningValue, instructions });
  const passthroughKeys = ["tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key", "text", "include"];
  for (const key of passthroughKeys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = rawRecord[key];
    }
  }
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

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error);
  }

  if (!upstream.ok) {
    await recordErrorUsage(usageContext);
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  if (clientWantsStream) {
    if (!upstream.body) {
      await recordErrorUsage(usageContext);
      return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
    }
    const headers = new Headers(upstream.headers);
    headers.set("x-ubq-upstream", "chatgpt_codex");
    if (usageContext?.keyId || usageContext?.kernelRepo || usageContext?.kernelOrg) {
      const [clientStream, analyticsStream] = upstream.body.tee();
      void collectResponsesStreamUsage(analyticsStream, usageContext);
      const response = new Response(clientStream, {
        status: upstream.status,
        statusText: upstream.statusText,
        headers,
      });
      return withUosWarning(response, warnings);
    }
    const response = new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
    return withUosWarning(response, warnings);
  }

  if (!upstream.body) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
  }

  let finalResponse: Record<string, unknown> | null = null;
  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
      finalResponse = ev.response;
      break;
    }
  }
  if (!finalResponse) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error");
  }
  const usageTokens = extractUsageTokens(finalResponse.usage);
  await recordCompletionUsage(usageContext, usageTokens);
  const response = json(200, finalResponse, { "x-ubq-upstream": "chatgpt_codex" });
  return withUosWarning(response, warnings);
};
