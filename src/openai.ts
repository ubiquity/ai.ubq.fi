import { buildCodexRequest, codexInstructionsPromise, CodexError, fetchCodexResponses } from "./codex.ts";
import { json, openaiError } from "./http.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord } from "./utils.ts";
import type { ChatCompletionRequest, MessageContentItem, ResponseMessageItem, ResponsesRequest } from "./types.ts";

type ReasoningEffort = "none" | "minimal" | "low" | "medium" | "high" | "xhigh";

const REASONING_EFFORTS: ReadonlySet<ReasoningEffort> = new Set(["none", "minimal", "low", "medium", "high", "xhigh"]);

const DEFAULT_MODEL = "gpt-5-chat-latest";
const DEFAULT_REASONING_EFFORT: ReasoningEffort = "xhigh";

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

const streamChatCompletions = (upstream: Response, model: string): Response => {
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

const completeChatCompletions = async (upstream: Response, model: string): Promise<Response> => {
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
      const u = isRecord(ev.response.usage) ? ev.response.usage : null;
      if (u) {
        const promptTokens = typeof u.input_tokens === "number" ? u.input_tokens : null;
        const completionTokens = typeof u.output_tokens === "number" ? u.output_tokens : null;
        const totalTokens = typeof u.total_tokens === "number" ? u.total_tokens : null;
        if (promptTokens !== null && completionTokens !== null && totalTokens !== null) {
          usage = {
            prompt_tokens: promptTokens,
            completion_tokens: completionTokens,
            total_tokens: totalTokens,
          };
        }
      }
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

export const handleModels = (): Response =>
  json(
    200,
    {
      object: "list",
      data: [
        { id: "gpt-5-chat-latest", object: "model", owned_by: "openai" },
        { id: "gpt-5.2-chat-latest", object: "model", owned_by: "openai" },
        { id: "gpt-5.1-chat-latest", object: "model", owned_by: "openai" },
        { id: "gpt-5.1-codex-max", object: "model", owned_by: "openai" },
        { id: "gpt-5.1-codex", object: "model", owned_by: "openai" },
        { id: "gpt-5.1-codex-mini", object: "model", owned_by: "openai" },
        { id: "gpt-5.2", object: "model", owned_by: "openai" },
        { id: "gpt-5.1", object: "model", owned_by: "openai" },
      ],
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );

export const handleChatCompletions = async (req: Request): Promise<Response> => {
  const body = (await readJsonBody(req)) as ChatCompletionRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const modelRaw = (getString(body.model) ?? "").trim() || DEFAULT_MODEL;
  const model = normalizeModelForCodex(modelRaw);
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return openaiError(400, "messages must be an array", "invalid_request_error");
  if (messagesRaw.length === 0) return openaiError(400, "messages must be a non-empty array", "invalid_request_error");

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) return openaiError(400, reasoningEffort.message, "invalid_request_error");

  const input: ResponseMessageItem[] = [];
  for (const msg of messagesRaw) {
    const converted = toResponseMessageItem(msg);
    if (!converted) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    input.push(converted);
  }

  const defaultReasoning = looksLikeReasoningModel(model) ? { effort: DEFAULT_REASONING_EFFORT } : undefined;
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoningEffort.value === undefined ? defaultReasoning : { effort: reasoningEffort.value },
  });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
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

  const stream = Boolean(body.stream);
  return stream ? streamChatCompletions(upstream, model) : await completeChatCompletions(upstream, model);
};

export const handleResponses = async (req: Request): Promise<Response> => {
  const rawBody = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const clientWantsStream = Boolean(rawBody.stream);

  const modelRaw = (getString(rawBody.model) ?? "").trim() || DEFAULT_MODEL;
  const model = normalizeModelForCodex(modelRaw);

  const inputRaw = rawBody.input;
  let input: ResponseMessageItem[];
  if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    if (inputRaw.length === 0) return openaiError(400, "input must be a non-empty array", "invalid_request_error");
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

  const defaultReasoning = looksLikeReasoningModel(model) ? { effort: DEFAULT_REASONING_EFFORT } : undefined;
  const reasoningValue = reasoning.value !== undefined ? reasoning.value : defaultReasoning;

  const codexBody = await buildCodexRequest(model, input, { reasoning: reasoningValue });
  const passthroughKeys = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "metadata",
    "max_output_tokens",
    "temperature",
    "top_p",
    "seed",
    "truncation",
    "response_format",
    "user",
    "include",
    "store",
    "instructions",
  ];
  const rawRecord = rawBody as Record<string, unknown>;
  for (const key of passthroughKeys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = rawRecord[key];
    }
  }
  codexBody.model = model;
  codexBody.input = input;
  codexBody.stream = true;

  const instructions = getString(codexBody.instructions);
  if (!instructions?.trim()) {
    codexBody.instructions = await codexInstructionsPromise;
  }

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
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

  if (clientWantsStream) {
    const headers = new Headers(upstream.headers);
    headers.set("x-ubq-upstream", "chatgpt_codex");
    return new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
  }

  if (!upstream.body) return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");

  let finalResponse: unknown | null = null;
  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
      finalResponse = ev.response;
      break;
    }
  }
  if (!finalResponse) return openaiError(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error");
  return json(200, finalResponse, { "x-ubq-upstream": "chatgpt_codex" });
};
