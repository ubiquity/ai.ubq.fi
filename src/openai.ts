import { buildCodexRequest, fetchCodexResponses } from "./codex.ts";
import { json, openaiError } from "./http.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord } from "./utils.ts";
import type { ChatCompletionRequest, MessageContentItem, ResponseMessageItem, ResponsesRequest } from "./types.ts";

type ReasoningEffort = "none" | "minimal" | "low" | "medium" | "high" | "xhigh";

const REASONING_EFFORTS: ReadonlySet<ReasoningEffort> = new Set(["none", "minimal", "low", "medium", "high", "xhigh"]);

const DEFAULT_MODEL = "gpt-5-chat-latest";
const DEFAULT_REASONING_EFFORT: ReasoningEffort = "xhigh";

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

const extractTextParts = (content: unknown): string => {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  const parts: string[] = [];
  for (const item of content) {
    if (!isRecord(item)) continue;
    const type = getString(item.type);
    if (type === "text") {
      const text = getString(item.text);
      if (text) parts.push(text);
    } else if (type === "input_text") {
      const text = getString(item.text);
      if (text) parts.push(text);
    } else if (type === "output_text") {
      const text = getString(item.text);
      if (text) parts.push(text);
    }
  }
  return parts.join("");
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
  const text = extractTextParts(message.content);
  const content: MessageContentItem[] = role === "assistant"
    ? [{ type: "output_text", text }]
    : [{ type: "input_text", text }];
  return { type: "message", role, content };
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
      const parts = buffer.split("\n\n");
      buffer = parts.pop() ?? "";
      for (const part of parts) {
        if (!part.trim()) continue;
        const lines = part.split("\n");
        const dataLines = lines.filter((line) => line.startsWith("data:"));
        const data = dataLines.map((line) => line.slice(5).trim()).join("\n");
        if (!data) continue;
        try {
          yield JSON.parse(data);
        } catch {
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
    return openaiError(502, "Upstream response missing body", "bad_gateway");
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
  if (!upstream.body) return openaiError(502, "Upstream response missing body", "bad_gateway");

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
    return openaiError(502, "Upstream request failed", "bad_gateway");
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
  const body = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const modelRaw = (getString(body.model) ?? "").trim() || DEFAULT_MODEL;
  const model = normalizeModelForCodex(modelRaw);
  const inputRaw = body.input;

  let input: ResponseMessageItem[];
  if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    input = [];
    for (const item of inputRaw) {
      const converted = toResponseMessageItem(item);
      if (!converted) return openaiError(400, "Invalid item in input[]", "invalid_request_error");
      input.push(converted);
    }
  } else {
    return openaiError(400, "input must be a string or an array", "invalid_request_error");
  }

  const reasoning = parseReasoningParam(body.reasoning);
  if (!reasoning.ok) return openaiError(400, reasoning.message, "invalid_request_error");

  const defaultReasoning = looksLikeReasoningModel(model) ? { effort: DEFAULT_REASONING_EFFORT } : undefined;
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoning.value === undefined ? defaultReasoning : reasoning.value,
  });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    return openaiError(502, "Upstream request failed", "bad_gateway");
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

  const clientWantsStream = Boolean(body.stream);
  if (clientWantsStream) {
    const headers = new Headers(upstream.headers);
    headers.set("x-ubq-upstream", "chatgpt_codex");
    return new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
  }

  if (!upstream.body) return openaiError(502, "Upstream response missing body", "bad_gateway");

  let finalResponse: unknown | null = null;
  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
      finalResponse = ev.response;
      break;
    }
  }
  if (!finalResponse) return openaiError(502, "Upstream stream ended unexpectedly", "bad_gateway");
  return json(200, finalResponse, { "x-ubq-upstream": "chatgpt_codex" });
};
