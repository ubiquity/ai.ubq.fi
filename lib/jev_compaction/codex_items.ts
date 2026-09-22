/**
 * Ported from 0x4007/fast-jev-compaction (MIT), commit
 * c1eab5fdbd6bde7d67f2da070116496558836884, `codex/codex-items.ts`. Mechanical
 * changes: Deno `.ts` specifiers importing this directory instead of `dist/`,
 * repository formatting (type aliases, not interfaces; no nested ternaries), and
 * non-consumed exports made module-private. The Codex item mapping, prompt
 * exclusion, opaque-payload redaction and verbatim renderer are unchanged. See
 * ./LICENSE and ./README.md.
 *
 * Pure mapping between Codex Responses API input items and the compaction
 * library `Message` shape, plus the deterministic renderer that turns Jev's
 * keep/drop decisions back into one text summary. No I/O, no Deno/Node APIs.
 */
import { truncatedResultText } from "./compact.ts";
import type { CallDecision, CompactResult, Message, ToolResult, ToolUse } from "./types.ts";

type UnknownRecord = Record<string, unknown>;

/** First line of every rendered summary; never the library's own prefix. */
export const SUMMARY_MARKER = "# fast-jev-compaction memory summary";

/**
 * Body of Codex's built-in local-compaction prompt
 * (`codex-rs/prompts/templates/compact/prompt.md`). Used only to recognise the
 * prompt Codex appends as the final input item, never to detect whether a
 * request is a compaction request (that is header-only).
 */
const COMPACTION_PROMPT_MARKERS = ["CONTEXT CHECKPOINT COMPACTION", "Create a handoff summary for another LLM"] as const;

/** Refuse to answer with a larger summary than this; never truncate pinned data. */
export const MAX_SUMMARY_CHARS = 400_000;

type TextEntry = {
  kind: "text";
  itemType: string;
  role: "user" | "assistant" | "developer" | "system";
  text: string;
};

type CallEntry = {
  kind: "call";
  itemType: string;
  callId: string;
  tool: string;
  input: UnknownRecord;
  inputJson: string;
};

type ResultEntry = {
  kind: "result";
  itemType: string;
  callId: string;
  text: string;
  isError: boolean;
};

type TranscriptEntry = TextEntry | CallEntry | ResultEntry;

export type PromptExclusion = {
  /** Index in the original `input` array. */
  index: number;
  chars: number;
  reason: "known-prompt" | "trailing-prompt-assumed";
};

export type CodexTranscript = {
  /** Original item order, injected compaction prompt removed. */
  entries: TranscriptEntry[];
  /** One library message per surviving entry, same order. */
  messages: Message[];
  exclusion: PromptExclusion | null;
  /** Items rendered as an opaque placeholder instead of plaintext. */
  opaqueItems: number;
};

function record(value: unknown): UnknownRecord | null {
  return value !== null && typeof value === "object" && !Array.isArray(value) ? (value as UnknownRecord) : null;
}

function str(value: unknown): string {
  return typeof value === "string" ? value : "";
}

/** Text of a `message` item's content parts; images/files become placeholders. */
function messageText(content: unknown): string {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  const parts: string[] = [];
  for (const raw of content) {
    if (typeof raw === "string") {
      parts.push(raw);
      continue;
    }
    const part = record(raw);
    if (!part) continue;
    const type = str(part.type);
    if (typeof part.text === "string") parts.push(part.text);
    else if (type === "input_image" || type === "image") parts.push("[image]");
    else if (type === "input_file") parts.push("[file]");
    else if (type === "refusal" && typeof part.refusal === "string") parts.push(part.refusal);
  }
  return parts.join("\n");
}

/** Tool input: the parsed `arguments` JSON, or a wrapper when it is not an object. */
function parseToolArguments(argumentsText: string): UnknownRecord {
  const trimmed = argumentsText.trim();
  if (trimmed.length === 0) return {};
  let parsed: unknown;
  try {
    parsed = JSON.parse(trimmed);
  } catch {
    return { unparsed_arguments: argumentsText };
  }
  const obj = record(parsed);
  if (obj) return obj;
  return { value: parsed };
}

function isErrorOutput(item: UnknownRecord): boolean {
  return item.is_error === true || item.success === false;
}

/** Output text of a `*_call_output` item; objects are serialised, never dropped. */
function outputText(output: unknown): string {
  if (typeof output === "string") return output;
  if (output === undefined || output === null) return "[no output]";
  try {
    return JSON.stringify(output) ?? String(output);
  } catch {
    return "[unserialisable output]";
  }
}

const OPAQUE_KEY = /encrypt|ciphertext|opaque|blob|signature/i;
const OPAQUE_NOTICE = (chars: number) => `[opaque payload, ${chars} chars omitted from this memory]`;

function redact(value: unknown, depth = 0): unknown {
  if (typeof value === "string") return value;
  if (value === null || typeof value !== "object") return value;
  if (depth > 6) return "[deeply nested value omitted]";
  if (Array.isArray(value)) return value.map((entry) => redact(entry, depth + 1));
  const out: UnknownRecord = {};
  for (const [key, entry] of Object.entries(value as UnknownRecord)) {
    if (OPAQUE_KEY.test(key)) {
      out[key] = typeof entry === "string" ? OPAQUE_NOTICE(entry.length) : OPAQUE_NOTICE(0);
      continue;
    }
    out[key] = redact(entry, depth + 1);
  }
  return out;
}

/**
 * Bounded, redacted rendering of an item the adapter does not model. Opaque
 * payloads (encrypted reasoning/compaction blobs) become a notice instead of
 * usable plaintext; unknown items stay represented so nothing vanishes mutely.
 */
function opaquePlaceholder(itemType: string, item: UnknownRecord, limit = 2000): { text: string; opaque: boolean } {
  let json = "";
  try {
    json = JSON.stringify(redact(item)) ?? "";
  } catch {
    json = "[unserialisable item]";
  }
  const opaque = json.includes("opaque payload,");
  const clipped = json.length > limit ? `${json.slice(0, limit)}… [${json.length - limit} chars omitted]` : json;
  return {
    text: `[${itemType || "unknown item"}]${opaque ? " (contains an opaque payload)" : ""} ${clipped}`,
    opaque,
  };
}

function messageEntry(item: UnknownRecord): TextEntry {
  const role = str(item.role);
  let normalized: TextEntry["role"] = "user";
  if (role === "assistant") normalized = "assistant";
  else if (role === "developer" || role === "system") normalized = role;
  return {
    kind: "text",
    itemType: "message",
    role: normalized,
    text: messageText(item.content),
  };
}

function callEntry(item: UnknownRecord): CallEntry {
  const functionCall = str(item.type) === "function_call";
  let rawInput: UnknownRecord;
  if (functionCall) {
    rawInput = parseToolArguments(str(item.arguments));
  } else if (typeof item.input === "string") {
    rawInput = parseToolArguments(item.input);
  } else {
    rawInput = record(item.input) ?? {};
  }
  let inputJson = "";
  try {
    inputJson = JSON.stringify(rawInput);
  } catch {
    inputJson = "[unserialisable tool input]";
  }
  return {
    kind: "call",
    itemType: str(item.type),
    callId: str(item.call_id),
    tool: functionCall ? str(item.name) : str(item.name) || "custom_tool",
    input: rawInput,
    inputJson,
  };
}

function resultEntry(item: UnknownRecord): ResultEntry {
  return {
    kind: "result",
    itemType: str(item.type),
    callId: str(item.call_id),
    text: outputText(item.output),
    isError: isErrorOutput(item),
  };
}

function entryToMessage(entry: TranscriptEntry): Message {
  if (entry.kind === "text") {
    return { role: entry.role === "assistant" ? "assistant" : "user", text: entry.text, toolUses: [] };
  }
  if (entry.kind === "call") {
    const toolUse: ToolUse = {
      tool_use_id: entry.callId,
      tool: entry.tool,
      input: entry.input,
    };
    return { role: "assistant", text: "", toolUses: [toolUse] };
  }
  const toolResult: ToolResult = { tool_use_id: entry.callId, text: entry.text };
  if (entry.isError) toolResult.isError = true;
  return { role: "user", text: "", toolUses: [], toolResults: [toolResult] };
}

/**
 * The trailing item Codex appends for local compaction. Codex always appends
 * its prompt (built-in `SUMMARIZATION_PROMPT` or the configured
 * `compact_prompt`) as the final user message, so only that item is excluded —
 * earlier real user content is kept even when it looks similar.
 */
function compactionPromptIndex(items: readonly UnknownRecord[]): PromptExclusion | null {
  const index = items.length - 1;
  if (index < 0) return null;
  const item: UnknownRecord | undefined = items[index];
  if (!item || str(item.type) !== "message" || str(item.role) !== "user") return null;
  const text = messageText(item.content);
  if (text.trim().length === 0) return null;
  const known = COMPACTION_PROMPT_MARKERS.some((marker) => text.includes(marker));
  return {
    index,
    chars: text.length,
    reason: known ? "known-prompt" : "trailing-prompt-assumed",
  };
}

/**
 * Maps a request `input` array to library messages in original order. Returns
 * `null` when `input` is not an array of items (caller treats that as failure).
 */
export function parseCodexInput(input: unknown): CodexTranscript | null {
  if (!Array.isArray(input)) return null;
  const rawItems = input.map((item) => record(item) ?? { type: "unknown", value: item });
  const exclusion = compactionPromptIndex(rawItems);
  const entries: TranscriptEntry[] = [];
  let opaqueItems = 0;
  rawItems.forEach((item, index) => {
    if (exclusion && index === exclusion.index) return;
    const type = str(item.type);
    if (type === "message") {
      entries.push(messageEntry(item));
      return;
    }
    if (type === "function_call" || type === "custom_tool_call") {
      entries.push(callEntry(item));
      return;
    }
    if (type === "function_call_output" || type === "custom_tool_call_output") {
      entries.push(resultEntry(item));
      return;
    }
    const placeholder = opaquePlaceholder(type, item);
    if (placeholder.opaque) opaqueItems += 1;
    entries.push({
      kind: "text",
      itemType: type || "unknown",
      role: "user",
      text: placeholder.text,
    });
  });
  return {
    entries,
    messages: entries.map(entryToMessage),
    exclusion,
    opaqueItems,
  };
}

const ROLE_LABEL: Record<TextEntry["role"], string> = {
  user: "user",
  assistant: "assistant",
  developer: "developer instructions",
  system: "system instructions",
};

export type RenderOptions = {
  decisions: readonly CallDecision[];
  /**
   * Library short call id (`t1`, `t2`, ...) to the Codex `call_id`. Built with
   * `collectToolCalls(messages, preserveRecentMessages)`, the same public
   * pairing the library used, so decisions cannot drift out of alignment.
   */
  callIds: ReadonlyMap<string, string>;
  headChars: number;
  stats?: CompactResult["stats"];
};

/** Renders the surviving transcript; kept text is emitted verbatim. */
export function renderSummary(transcript: CodexTranscript, options: RenderOptions): string {
  const decisionByCall = new Map<string, CallDecision>();
  const droppedCalls = new Set<string>();
  for (const decision of options.decisions) {
    const callId = options.callIds.get(decision.id) ?? decision.id;
    decisionByCall.set(callId, decision);
    if (decision.action === "drop_call") droppedCalls.add(callId);
  }
  const out: string[] = [
    SUMMARY_MARKER,
    "Tool calls and results below were classified by TypeSafe Jev (keep / truncate / drop the call).",
    "Text kept verbatim is copied from the original transcript; this memory is text only, so structured",
    "tool-call replay and opaque items (encrypted reasoning or compaction payloads) are not restored.",
    "",
  ];
  for (const entry of transcript.entries) {
    if (entry.kind === "text") {
      out.push(`[${ROLE_LABEL[entry.role]}]`, entry.text, "");
      continue;
    }
    if (entry.kind === "call") {
      if (droppedCalls.has(entry.callId)) continue;
      const decision = decisionByCall.get(entry.callId);
      out.push(
        `[tool call ${entry.callId || "(no call id)"}] ${entry.tool || "(unnamed tool)"} — ${decision ? decision.action : "no decision (kept)"}`,
        `input: ${entry.inputJson}`,
        ""
      );
      continue;
    }
    if (droppedCalls.has(entry.callId)) continue;
    const decision = decisionByCall.get(entry.callId);
    if (!decision) {
      out.push(`[tool result ${entry.callId || "(no call id)"} — no matching call in this transcript]`, entry.text, "");
      continue;
    }
    if (decision.action === "drop_result") {
      out.push(
        `[tool result ${entry.callId} — truncated, original was ${entry.text.length} chars]`,
        truncatedResultText(entry.text, entry.isError, options.headChars),
        ""
      );
      continue;
    }
    out.push(`[tool result ${entry.callId} — kept verbatim]`, entry.text, "");
  }
  const stats = options.stats;
  const excluded = transcript.exclusion;
  let statsLine = "calls=0 kept=0 results_dropped=0 calls_dropped=0 pinned=0 chars_before=0 chars_after=0 requests=0 stage=none";
  if (stats) {
    statsLine = [
      `calls=${stats.resultsDropped + stats.callsDropped + stats.kept + stats.pinned}`,
      `kept=${stats.kept}`,
      `results_dropped=${stats.resultsDropped}`,
      `calls_dropped=${stats.callsDropped}`,
      `pinned=${stats.pinned}`,
      `chars_before=${stats.charsBefore}`,
      `chars_after=${stats.charsAfter}`,
      `requests=${stats.requests}`,
      `stage=${stats.stateStage || "none"}`,
    ].join(" ");
  }
  out.push(
    "[fast-jev-compaction stats] " +
      [
        `items=${transcript.entries.length}`,
        `excluded_prompt_chars=${excluded ? excluded.chars : 0}`,
        `excluded_prompt_reason=${excluded ? excluded.reason : "none"}`,
        `opaque_items=${transcript.opaqueItems}`,
        statsLine,
      ].join(" ")
  );
  return out.join("\n");
}

export type TurnMetadata = {
  request_kind?: string;
  compaction?: { implementation?: string; [key: string]: unknown };
  [key: string]: unknown;
};

/** Parses `x-codex-turn-metadata`; malformed or absent metadata is `null`. */
export function parseTurnMetadata(header: string | null): TurnMetadata | null {
  if (!header) return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(header);
  } catch {
    return null;
  }
  return record(parsed) as TurnMetadata | null;
}

/**
 * Interception predicate: header metadata only, no prompt or body inspection.
 * An explicit compaction request with another implementation is *not* adopted.
 */
export function isResponsesCompaction(metadata: TurnMetadata | null): boolean {
  return metadata?.request_kind === "compaction" && record(metadata.compaction)?.implementation === "responses";
}
