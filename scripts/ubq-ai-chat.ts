// ubq-ai chat, responses and informational commands, split out of scripts/ubq-ai.ts.

import type { UbqAiCommandContext } from "./ubq-ai-core.ts";
import {
  TEXT_ENCODER,
  extractChatContent,
  extractChatDelta,
  extractResponseDelta,
  extractResponseText,
  getFlagString,
  getString,
  isRecord,
  parseSseEvents,
  resolveChatMessages,
  resolveClientToken,
  resolveResponsesInput,
  streamToOut,
  writeErrText,
  writeOutText,
  writeRequestFailure,
} from "./ubq-ai-core.ts";

const handleChatStreamEvent = async (ctx: UbqAiCommandContext, ev: unknown): Promise<boolean> => {
  const { runtime, wantsJson } = ctx;
  if (ev === "[DONE]") return true;
  if (wantsJson) {
    await writeOutText(runtime, `${JSON.stringify(ev)}\n`);
    return false;
  }
  const delta = extractChatDelta(ev);
  if (delta) await runtime.out(TEXT_ENCODER.encode(delta));
  return false;
};

/** Streams one responses SSE event to stdout; returns true when the stream is finished. */
const handleResponsesStreamEvent = async (ctx: UbqAiCommandContext, ev: unknown): Promise<boolean> => {
  const { runtime, wantsJson } = ctx;
  if (ev === "[DONE]") return true;
  if (wantsJson) {
    await writeOutText(runtime, `${JSON.stringify(ev)}\n`);
    return false;
  }
  if (isRecord(ev) && getString(ev.type) === "response.completed") return true;
  const delta = extractResponseDelta(ev);
  if (delta) await runtime.out(TEXT_ENCODER.encode(delta));
  return false;
};

/** Streams a chat completions response body to stdout. */
const streamChatResponse = async (ctx: UbqAiCommandContext, req: Request): Promise<number> => {
  const { runtime, wantsJson, wantsRaw, debug } = ctx;
  await debug(`[ubq-ai] -> ${req.method} ${req.url} (stream)\n`);
  const res = await runtime.fetch(req);
  await debug(`[ubq-ai] <- ${res.status}\n`);
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    await writeErrText(runtime, `Request failed (${res.status}).\n`);
    await writeErrText(runtime, `${text || res.statusText}\n`);
    return 1;
  }
  if (!res.body) {
    await writeErrText(runtime, "Stream response missing body.\n");
    return 1;
  }

  if (wantsRaw) {
    await streamToOut(runtime, res.body);
    return 0;
  }

  for await (const ev of parseSseEvents(res.body)) {
    if (await handleChatStreamEvent(ctx, ev)) break;
  }
  if (!wantsJson) await runtime.out(TEXT_ENCODER.encode("\n"));
  return 0;
};

/** Streams a responses API response body to stdout. */
const streamResponsesResponse = async (ctx: UbqAiCommandContext, req: Request): Promise<number> => {
  const { runtime, wantsJson, wantsRaw, debug } = ctx;
  await debug(`[ubq-ai] -> ${req.method} ${req.url} (stream)\n`);
  const res = await runtime.fetch(req);
  await debug(`[ubq-ai] <- ${res.status}\n`);
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    await writeErrText(runtime, `Request failed (${res.status}).\n`);
    await writeErrText(runtime, `${text || res.statusText}\n`);
    return 1;
  }
  if (!res.body) {
    await writeErrText(runtime, "Stream response missing body.\n");
    return 1;
  }

  if (wantsRaw) {
    await streamToOut(runtime, res.body);
    return 0;
  }

  for await (const ev of parseSseEvents(res.body)) {
    if (await handleResponsesStreamEvent(ctx, ev)) break;
  }
  if (!wantsJson) await runtime.out(TEXT_ENCODER.encode("\n"));
  return 0;
};

const runHealthCommand = async (ctx: UbqAiCommandContext): Promise<number> => {
  const { runtime, endpoint, doFetchWithDebug } = ctx;
  const req = new Request(endpoint("/health"), { method: "GET", headers: { Accept: "application/json" } });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runInfoCommand = async (ctx: UbqAiCommandContext): Promise<number> => {
  const { runtime, endpoint, doFetchWithDebug } = ctx;
  const req = new Request(endpoint("/"), { method: "GET", headers: { Accept: "application/json" } });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runWhoamiCommand = async (ctx: UbqAiCommandContext): Promise<number> => {
  const { runtime, flags, endpoint, doFetchWithDebug } = ctx;
  const token = resolveClientToken(flags, runtime);
  if (!token) {
    await writeErrText(runtime, "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n");
    return 2;
  }
  const req = new Request(endpoint("/uos/auth"), {
    method: "GET",
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: "application/json",
    },
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runModelsCommand = async (ctx: UbqAiCommandContext): Promise<number> => {
  const { runtime, flags, endpoint, doFetchWithDebug } = ctx;
  const token = resolveClientToken(flags, runtime);
  if (!token) {
    await writeErrText(runtime, "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n");
    return 2;
  }
  const req = new Request(endpoint("/v1/models"), {
    method: "GET",
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: "application/json",
    },
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runChatCommand = async (ctx: UbqAiCommandContext): Promise<number> => {
  const { runtime, flags, endpoint, wantsJson, wantsStream, doFetchWithDebug } = ctx;
  const token = resolveClientToken(flags, runtime);
  if (!token) {
    await writeErrText(runtime, "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n");
    return 2;
  }
  const model = (getFlagString(flags, "model") ?? "").trim();

  const resolved = await resolveChatMessages(ctx);
  if (!resolved.ok) return resolved.exitCode;
  const messages = resolved.messages;

  const body: Record<string, unknown> = { messages, stream: wantsStream };
  if (model) body.model = model;

  const reasoningEffortRaw = (getFlagString(flags, "reasoning-effort") ?? "").trim();
  if (reasoningEffortRaw) {
    body.reasoning_effort = reasoningEffortRaw;
  }

  const req = new Request(endpoint("/v1/chat/completions"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
      Accept: wantsStream ? "text/event-stream" : "application/json",
    },
    body: JSON.stringify(body),
  });

  if (wantsStream) return await streamChatResponse(ctx, req);

  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);

  if (wantsJson) {
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  const content = extractChatContent(result.json);
  if (content !== null) {
    await writeOutText(runtime, `${content}\n`);
    return 0;
  }
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runResponsesCommand = async (ctx: UbqAiCommandContext): Promise<number> => {
  const { runtime, flags, endpoint, wantsJson, wantsStream, doFetchWithDebug } = ctx;
  const token = resolveClientToken(flags, runtime);
  if (!token) {
    await writeErrText(runtime, "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n");
    return 2;
  }
  const model = (getFlagString(flags, "model") ?? "").trim();
  const instructionsRaw = getFlagString(flags, "instructions");
  const instructions = typeof instructionsRaw === "string" ? instructionsRaw.trim() : "";

  const resolved = await resolveResponsesInput(ctx);
  if (!resolved.ok) return resolved.exitCode;
  const input = resolved.input;

  const body: Record<string, unknown> = { input, stream: wantsStream };
  if (model) body.model = model;
  body.instructions = instructions;

  const reasoningEffortRaw = (getFlagString(flags, "reasoning-effort") ?? "").trim();
  if (reasoningEffortRaw) {
    body.reasoning = { effort: reasoningEffortRaw };
  }

  const req = new Request(endpoint("/v1/responses"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
      Accept: wantsStream ? "text/event-stream" : "application/json",
    },
    body: JSON.stringify(body),
  });

  if (wantsStream) return await streamResponsesResponse(ctx, req);

  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);

  if (wantsJson) {
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  const text = extractResponseText(result.json);
  if (text !== null) {
    await writeOutText(runtime, `${text}\n`);
    return 0;
  }
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

export { runChatCommand, runHealthCommand, runInfoCommand, runModelsCommand, runResponsesCommand, runWhoamiCommand };
