const getNonNegativeInteger = (value) =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null;

const getNonNegativeNumber = (value) =>
  typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : null;

export const readChatCompletionUsage = (value) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;

  const inputTokens = getNonNegativeInteger(value.prompt_tokens);
  const outputTokens = getNonNegativeInteger(value.completion_tokens);
  if (inputTokens === null || outputTokens === null) return null;

  const details = value.prompt_tokens_details;
  const promptTokenDetails = details && typeof details === "object" && !Array.isArray(details) ? details : null;
  const cachedInputTokens = promptTokenDetails && "cached_tokens" in promptTokenDetails
    ? getNonNegativeInteger(promptTokenDetails.cached_tokens)
    : null;
  const cacheWriteInputTokens = promptTokenDetails && "cache_write_tokens" in promptTokenDetails
    ? getNonNegativeInteger(promptTokenDetails.cache_write_tokens)
    : 0;

  return {
    inputTokens,
    outputTokens,
    cachedInputTokens: cachedInputTokens !== null && cachedInputTokens <= inputTokens ? cachedInputTokens : null,
    cacheWriteInputTokens: cacheWriteInputTokens ?? 0,
  };
};

export const formatChatTokens = (tokens) => {
  const value = getNonNegativeInteger(tokens);
  if (value === null) return null;
  const scaled = (number) => number >= 100 ? String(Math.round(number)) : String(Math.round(number * 10) / 10);
  if (value < 1_000) return String(value);
  if (value < 1_000_000) return `${scaled(value / 1_000)}K`;
  return `${scaled(value / 1_000_000)}M`;
};

export const formatChatDuration = (milliseconds) => {
  const value = getNonNegativeNumber(milliseconds);
  if (value === null) return null;
  const seconds = value / 1_000;
  if (seconds < 60) return `${Math.round(seconds * 10) / 10}s`;
  const wholeSeconds = Math.round(seconds);
  return `${Math.floor(wholeSeconds / 60)}m${wholeSeconds % 60}s`;
};

const formatTokensPerSecond = (tokensPerSecond) => {
  const value = getNonNegativeNumber(tokensPerSecond);
  if (value === null) return null;
  return value >= 10 ? String(Math.round(value)) : String(Math.round(value * 10) / 10);
};

const roundedIntegerPercent = (cacheReadTokens, denominator) => {
  const denominatorQuotient = Math.floor(denominator / 200);
  const denominatorRemainder = denominator % 200;
  let lower = 0;
  let upper = 100;
  while (lower < upper) {
    const candidate = Math.floor((lower + upper + 1) / 2);
    const factor = candidate * 2 - 1;
    if (
      cacheReadTokens >= factor * denominatorQuotient + Math.ceil(factor * denominatorRemainder / 200)
    ) {
      lower = candidate;
    } else {
      upper = candidate - 1;
    }
  }
  return lower;
};

export const formatCacheHitPercent = (cacheReadTokens, inputTokens) => {
  const cached = getNonNegativeInteger(cacheReadTokens);
  const input = getNonNegativeInteger(inputTokens);
  if (cached === null || input === null || input === 0 || cached > input) return null;
  if (cached === input) return "100";

  const integerPercent = roundedIntegerPercent(cached, input);
  if (integerPercent < 100) return String(integerPercent);

  const missedInputTokens = input - cached;
  let decimalPlaces = 1;
  let scaledDoubleGap = missedInputTokens * 200;
  const denominatorTens = Math.floor(input / 10);
  while (scaledDoubleGap <= denominatorTens) {
    scaledDoubleGap *= 10;
    decimalPlaces += 1;
  }

  const denominatorOnes = input % 10;
  let roundedLoss = 5;
  for (let loss = 1; loss < 5; loss += 1) {
    const factor = loss * 2 + 1;
    const threshold = factor * denominatorTens + Math.floor(factor * denominatorOnes / 10);
    if (scaledDoubleGap <= threshold) {
      roundedLoss = loss;
      break;
    }
  }
  return `99.${"9".repeat(decimalPlaces - 1)}${10 - roundedLoss}`;
};

export const formatChatStatsLine = (stats = {}) => {
  const groups = [];
  const turns = getNonNegativeInteger(stats.turns) ?? 1;
  const steps = getNonNegativeInteger(stats.steps) ?? 1;
  if (steps > 0) groups.push(`${turns} turns · ${steps} steps`);

  const durations = [];
  const llmDuration = formatChatDuration(stats.llmMs);
  if (llmDuration !== null) durations.push(`LLM ${llmDuration}`);
  const toolDuration = formatChatDuration(stats.toolMs);
  if (toolDuration !== null) durations.push(`Tool call ${toolDuration}`);
  if (durations.length > 0) groups.push(durations.join(" · "));

  const usage = readChatCompletionUsage(stats.usage);
  const speeds = [];
  const ttftDuration = formatChatDuration(stats.ttftMs);
  if (ttftDuration !== null) speeds.push(`TTFT avg ${ttftDuration}`);
  const decodeMs = getNonNegativeNumber(stats.decodeMs);
  const decodeTokens = getNonNegativeInteger(stats.decodeTokens);
  if (decodeTokens !== null && decodeMs !== null && decodeMs > 0) {
    const throughput = formatTokensPerSecond(decodeTokens / (decodeMs / 1_000));
    if (throughput !== null) speeds.push(`${throughput} tok/s`);
  }
  if (speeds.length > 0) groups.push(speeds.join(" · "));

  if (usage) {
    const cacheHit = formatCacheHitPercent(usage.cachedInputTokens, usage.inputTokens);
    if (cacheHit !== null) groups.push(`Cache hit ${cacheHit}%`);
    const input = formatChatTokens(usage.inputTokens);
    const output = formatChatTokens(usage.outputTokens);
    if (input !== null && output !== null) groups.push(`Input ${input} tok · Output ${output} tok`);
  }

  return groups.join(" | ");
};

export const splitChatSseEvents = (buffer, flush = false) => {
  let remaining = typeof buffer === "string" ? buffer : "";
  const events = [];

  for (;;) {
    const boundary = remaining.match(/(?:\r\n|\r(?!\n)|\n)(?:\r\n|\r(?!\n)|\n)/);
    if (!boundary || boundary.index === undefined) break;
    events.push(remaining.slice(0, boundary.index));
    remaining = remaining.slice(boundary.index + boundary[0].length);
  }

  if (flush && remaining.trim()) {
    events.push(remaining);
    remaining = "";
  }

  return { events, remaining };
};

export const parseChatCompletionStreamEvent = (dataText) => {
  const trimmed = typeof dataText === "string" ? dataText.trim() : "";
  if (!trimmed) return { kind: "empty" };
  if (trimmed === "[DONE]") return { kind: "done" };

  let payload;
  try {
    payload = JSON.parse(trimmed);
  } catch {
    return { kind: "invalid" };
  }
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) return { kind: "invalid" };

  const error = payload.error;
  if (error !== undefined && error !== null) {
    if (typeof error !== "object" || Array.isArray(error)) return { kind: "invalid" };
    const message = typeof error.message === "string" && error.message.trim()
      ? error.message.trim()
      : "The response stream failed.";
    return { kind: "error", message };
  }

  if (!Array.isArray(payload.choices)) return { kind: "invalid" };
  if (payload.usage !== undefined && payload.usage !== null) {
    if (
      typeof payload.usage !== "object" || Array.isArray(payload.usage) ||
      readChatCompletionUsage(payload.usage) === null
    ) {
      return { kind: "invalid" };
    }
  }
  if (payload.choices.length === 0) {
    return payload.usage && typeof payload.usage === "object"
      ? { kind: "event", delta: "", usage: payload.usage }
      : { kind: "invalid" };
  }

  const choice = payload.choices[0];
  if (!choice || typeof choice !== "object" || Array.isArray(choice)) return { kind: "invalid" };
  const delta = choice.delta;
  if (!delta || typeof delta !== "object" || Array.isArray(delta)) return { kind: "invalid" };
  if (delta.content !== undefined && delta.content !== null && typeof delta.content !== "string") {
    return { kind: "invalid" };
  }
  if (delta.refusal !== undefined && delta.refusal !== null && typeof delta.refusal !== "string") {
    return { kind: "invalid" };
  }

  const content = typeof delta.content === "string" ? delta.content : "";
  const refusal = typeof delta.refusal === "string" ? delta.refusal : "";
  return {
    kind: "event",
    delta: `${content}${refusal}`,
    usage: payload.usage,
  };
};

export const parseChatSseEvent = (rawEvent) => {
  const dataText = (typeof rawEvent === "string" ? rawEvent : "")
    .split(/\r\n|\r|\n/)
    .filter((line) => line.startsWith("data:"))
    .map((line) => line.slice("data:".length).trimStart())
    .join("\n")
    .trim();
  return parseChatCompletionStreamEvent(dataText);
};

export const createChatMessageElement = (documentRef, role, text) => {
  const message = documentRef.createElement("div");
  message.dataset.message = role;
  const content = documentRef.createElement("div");
  content.dataset.messageContent = "";
  content.textContent = text;
  message.appendChild(content);
  return message;
};

export const setChatMessageContent = (message, text) => {
  const content = message.querySelector("[data-message-content]");
  if (!content) throw new Error("Chat message content is missing.");
  content.textContent = text;
};

export const appendChatMessageStats = (message, stats) => {
  const line = formatChatStatsLine(stats);
  if (!line) return null;
  const existing = message.querySelector("[data-message-stats]");
  if (existing) existing.remove();
  const footer = message.ownerDocument.createElement("div");
  footer.dataset.messageStats = "";
  footer.textContent = line;
  footer.title = line;
  message.appendChild(footer);
  return footer;
};
