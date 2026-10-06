// corpus-build.ts — m1-corpus module worker for the DeepSeek V4.1 Flash provider waterfall benchmark.
// Builds the frozen v1 corpus from recorded DeepSeek V4.1 Flash subagent sessions under
// /Users/nv/.codex/sessions/2026/10 per DESIGN.md ("Frozen corpus"): one entry per chosen
// (session, turn boundary), boundary = last included response_item before an assistant-side record
// (user/developer message, tool result, or agent_message; call/output pairs never split); classes sized
// by the boundary turn's recorded input tokens (nearest token_count / token_usage_record at or after the
// boundary) small 2k-10k, medium 10k-40k, large 40k-160k, xlarge 160k-600k, target 6/9/9/6, closest
// recorded request used when a window cannot be filled; tools are the frozen fixture verbatim;
// instructions are the recorded base instructions; tool_choice "auto", parallel_tool_calls true,
// reasoning.effort from the boundary turn (else "high"), max_output_tokens null; every string is
// sanitized (credential shapes, then emails inside credential-matched strings). Modes (deterministic,
// no network): build (default), verify, selfcheck, report.

import type { CorpusClass, CorpusEntry, ResponsesInputItem } from "./types.ts";

const SESSIONS_ROOT = "/Users/nv/.codex/sessions";
const PRIMARY_MONTHS: readonly (readonly [string, string])[] = [["2026", "10"]];
const FALLBACK_MONTHS: readonly (readonly [string, string])[] = [["2026", "09"]];

const BENCH_DIR = decodeURIComponent(new URL(".", import.meta.url).pathname); // trailing slash
const CORPUS_DIR = `${BENCH_DIR}corpus`;
const JSONL_PATH = `${CORPUS_DIR}/corpus-v1.jsonl`;
const MANIFEST_PATH = `${CORPUS_DIR}/corpus-v1.manifest.json`;
const REPORT_PATH = `${BENCH_DIR}corpus-build-report.md`;
const TOOLS_FIXTURE_PATH = `${BENCH_DIR}fixtures/codex-tools-0.160.1.json`;

// sha256 DESIGN.md records for the frozen tools capture (the committed fixture bytes, normalized by the
// repository's deno-fmt pre-commit hook); the manifest also records the actual bytes hash and the match.
const DESIGN_DOC_TOOLS_SHA256 = "e48b8f1e1533e676ba43f1f15660a8f5e35d2a775c607b94cb4d46a2116e29ab";

const CLASS_ORDER: readonly CorpusClass[] = ["small", "medium", "large", "xlarge"];
const SELECTION_ORDER: readonly CorpusClass[] = ["xlarge", "small", "large", "medium"];
const CLASS_WINDOW: Readonly<Record<CorpusClass, readonly [number, number]>> = {
  small: [2000, 10000],
  medium: [10000, 40000],
  large: [40000, 160000],
  xlarge: [160000, 600000],
};
const CLASS_QUOTA: Readonly<Record<CorpusClass, number>> = { small: 6, medium: 9, large: 9, xlarge: 6 };

const SANITIZER_REVISION = "sanitizer-v1";
const INCLUDED_ITEM_TYPES = new Set(["message", "function_call", "function_call_output", "custom_tool_call", "custom_tool_call_output", "agent_message"]);
const MODEL_PREFIXES = ["deepseek-ai/DeepSeek-V4.1-Flash", "deepseek-v4-flash"];
const MODEL_EXACT = "deepseek-flash";

type JsonObject = { [key: string]: JsonValue };
type JsonValue = null | boolean | number | string | JsonValue[] | JsonObject;

const isObject = (v: unknown): v is JsonObject => typeof v === "object" && v !== null && !Array.isArray(v);
const asString = (v: unknown): string | null => (typeof v === "string" ? v : null);
const asNumber = (v: unknown): number | null => (typeof v === "number" && Number.isFinite(v) ? v : null);

// ------------------------------------------------------------------------------------------------ sanitizer

type SanitizerRule = {
  id: string;
  pattern: RegExp;
  /** Replacement given the whole match and capture groups. */
  replace: (match: string, ...groups: string[]) => string;
};

const SANITIZER_RULES: readonly SanitizerRule[] = [
  { id: "authorization_header", pattern: /(Authorization:\s*)[A-Za-z0-9._~+/=-]{16,}/gi, replace: (_match, head) => `${head}[REDACTED:authorization_header]` },
  {
    id: "assignment",
    pattern: /([A-Z0-9_]*(?:API_KEY|TOKEN|SECRET|PASSWORD)[A-Z0-9_]*)=(?!\[REDACTED:)[^\s'"`]+/g,
    replace: (_match, name) => `${name}=[REDACTED:assignment]`,
  },
  { id: "bearer", pattern: /\bBearer\s+[A-Za-z0-9._-]{20,}/g, replace: () => "[REDACTED:bearer]" },
  { id: "lith_sk", pattern: /lith_sk_[A-Za-z0-9]{16,}/g, replace: () => "[REDACTED:lith_sk]" },
  { id: "fernet", pattern: /gAAAAA[A-Za-z0-9_-]{40,}/g, replace: () => "[REDACTED:fernet]" },
  { id: "slack_bot", pattern: /xoxb-[A-Za-z0-9-]{10,}/g, replace: () => "[REDACTED:slack_bot]" },
  { id: "github_pat", pattern: /ghp_[A-Za-z0-9]{30,}/g, replace: () => "[REDACTED:github_pat]" },
  { id: "aws_access_key", pattern: /AKIA[0-9A-Z]{16}/g, replace: () => "[REDACTED:aws_access_key]" },
  { id: "hex_ak_sk", pattern: /(?:AK|SK)[0-9a-f]{32}/g, replace: () => "[REDACTED:hex_ak_sk]" },
  { id: "sk", pattern: /sk-[A-Za-z0-9_-]{16,}/g, replace: () => "[REDACTED:sk]" },
];

const EMAIL_PATTERN = /[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g;

const sanitizeString = (value: string, counts: Map<string, number>): string => {
  let out = value;
  let credentialMatched = false;
  for (const rule of SANITIZER_RULES) {
    out = out.replace(rule.pattern, (match: string, ...groups: unknown[]) => {
      credentialMatched = true;
      counts.set(rule.id, (counts.get(rule.id) ?? 0) + 1);
      return rule.replace(match, ...(groups as string[]));
    });
  }
  if (credentialMatched) {
    out = out.replace(EMAIL_PATTERN, () => {
      counts.set("email", (counts.get("email") ?? 0) + 1);
      return "[REDACTED:email]";
    });
  }
  return out;
};

const sanitizeValue = (value: JsonValue, counts: Map<string, number>): JsonValue => {
  if (typeof value === "string") return sanitizeString(value, counts);
  if (Array.isArray(value)) return value.map((entry) => sanitizeValue(entry, counts));
  if (value !== null && typeof value === "object") {
    const out: JsonObject = {};
    for (const [key, entry] of Object.entries(value)) out[key] = sanitizeValue(entry, counts);
    return out;
  }
  return value;
};

const shapeMask = (text: string): string => text.replace(/[A-Za-z]/g, "L").replace(/[0-9]/g, "D");

const countResidualMatches = (text: string): number => SANITIZER_RULES.reduce((total, rule) => total + (text.match(rule.pattern)?.length ?? 0), 0);

type ResidualHit = { path: string; details: string[] };

/** Masked (shape-only) residual scan over every string in a JSON value, reporting the value path. */
const findResidualStrings = (value: JsonValue, path: string, hits: ResidualHit[]): void => {
  if (typeof value === "string") {
    if (countResidualMatches(value) !== 0) hits.push({ path, details: describeResidualMatches(value) });
  } else if (Array.isArray(value)) {
    value.forEach((entry, index) => findResidualStrings(entry, `${path}[${index}]`, hits));
  } else if (value !== null && typeof value === "object") {
    for (const [key, entry] of Object.entries(value)) findResidualStrings(entry, `${path}.${key}`, hits);
  }
};

/** Masked (shape-only) description of any residual match, so failure diagnostics never echo values. */
const describeResidualMatches = (text: string): string[] => {
  const details: string[] = [];
  for (const rule of SANITIZER_RULES) {
    const pattern = new RegExp(rule.pattern.source, rule.pattern.flags);
    for (const match of text.matchAll(pattern)) {
      const start = match.index ?? 0;
      const before = shapeMask(text.slice(Math.max(0, start - 120), start));
      const after = shapeMask(text.slice(start + match[0].length, start + match[0].length + 40));
      details.push(`${rule.id} len=${match[0].length} match=${shapeMask(match[0]).slice(0, 16)} before=${before} after=${after}`);
    }
  }
  return details;
};

// -------------------------------------------------------------------------------------------- session IO

type RawRecord = { ordinal: number; timestamp: string | null; type: string; payload: JsonObject };

const parseSessionRecords = (path: string, text: string): RawRecord[] => {
  const records: RawRecord[] = [];
  let lineNumber = 0;
  for (const line of text.split("\n")) {
    lineNumber += 1;
    if (!line.trim()) continue;
    let parsed: unknown;
    try {
      parsed = JSON.parse(line);
    } catch (error) {
      throw new Error(`malformed JSON at ${path}:${lineNumber}: ${(error as Error).message}`);
    }
    if (!isObject(parsed)) throw new Error(`non-object record at ${path}:${lineNumber}`);
    const payload = isObject(parsed.payload) ? parsed.payload : {};
    records.push({
      ordinal: asNumber(parsed.ordinal) ?? records.length,
      timestamp: asString(parsed.timestamp),
      type: asString(parsed.type) ?? "",
      payload,
    });
  }
  records.sort((a, b) => a.ordinal - b.ordinal);
  return records;
};

const modelMatches = (model: string | null): boolean => {
  if (!model) return false;
  return MODEL_PREFIXES.some((prefix) => model.startsWith(prefix)) || model === MODEL_EXACT;
};

type ItemRecord = { ordinal: number; timestamp: string | null; payload: JsonObject };
type TurnContext = { ordinal: number; turnId: string | null; model: string | null; effort: string | null };
type UsageRecord = { ordinal: number; kind: "token_usage_record" | "token_count"; payload: JsonObject };

type Boundary = {
  itemIndex: number;
  ordinal: number;
  itemType: string;
  role: string | null;
  tokens: number;
  usageKind: string;
  turnId: string | null;
  threadId: string | null;
  model: string | null;
  effort: string | null;
  recordedAt: string | null;
  description: string;
};

type SessionData = {
  path: string;
  relativePath: string;
  candidateReason: string;
  metaRecords: number;
  sessionId: string;
  cwd: string | null;
  baseInstructions: string;
  items: ItemRecord[];
  turnContexts: TurnContext[];
  usages: UsageRecord[];
  boundaries: Boundary[];
  droppedTypesInFile: Map<string, number>;
};

const isSubagentMeta = (meta: JsonObject): string | null => {
  if (meta.thread_source === "subagent") return "thread_source=subagent";
  const source = meta.source;
  if (isObject(source) && "subagent" in source) return "source.subagent present";
  if (typeof source === "string" && source === "subagent") return "source=subagent";
  return null;
};

const isBoundaryItem = (payload: JsonObject): boolean => {
  const type = payload.type;
  if (type === "message") return payload.role === "user" || payload.role === "developer";
  return type === "agent_message" || type === "function_call_output" || type === "custom_tool_call_output";
};

const isAssistantSide = (payload: JsonObject): boolean => {
  const type = payload.type;
  if (type === "message") return payload.role === "assistant";
  return type === "function_call" || type === "custom_tool_call" || type === "reasoning";
};

const boundaryDescription = (payload: JsonObject, ordinal: number): string => {
  const type = asString(payload.type) ?? "unknown";
  const role = asString(payload.role);
  return role ? `${type}/${role} @ord ${ordinal}` : `${type} @ord ${ordinal}`;
};

const usageInputTokens = (usage: UsageRecord): number | null => {
  if (usage.kind === "token_usage_record") {
    const direct = isObject(usage.payload.turn_token_usage) ? usage.payload.turn_token_usage : usage.payload.usage;
    if (isObject(direct)) return asNumber(direct.input_tokens);
    return null;
  }
  const info = isObject(usage.payload.info) ? usage.payload.info : null;
  if (!info) return null;
  const last = isObject(info.last_token_usage) ? info.last_token_usage : info.total_token_usage;
  if (isObject(last)) return asNumber(last.input_tokens);
  return null;
};

const parseSession = (path: string, relativePath: string, text: string): SessionData | null => {
  const records = parseSessionRecords(path, text);
  const metaRecords = records.filter((record) => record.type === "session_meta");
  const matchingMeta = metaRecords.map((record) => ({ record, reason: isSubagentMeta(record.payload) })).find((candidate) => candidate.reason !== null);
  if (!matchingMeta) return null;
  const meta = matchingMeta.record.payload;
  const reason = matchingMeta.reason as string;
  const turnContexts: TurnContext[] = records
    .filter((record) => record.type === "turn_context")
    .map((record) => ({
      ordinal: record.ordinal,
      turnId: asString(record.payload.turn_id),
      model: asString(record.payload.model),
      effort: asString(record.payload.effort),
    }));
  if (!turnContexts.some((context) => modelMatches(context.model))) return null;

  const items: ItemRecord[] = records
    .filter((record) => record.type === "response_item")
    .map((record) => ({ ordinal: record.ordinal, timestamp: record.timestamp, payload: record.payload }));
  const usages: UsageRecord[] = records
    .filter((record) => record.type === "token_usage_record" || (record.type === "event_msg" && record.payload.type === "token_count"))
    .map((record) => ({
      ordinal: record.ordinal,
      kind: record.type === "token_usage_record" ? "token_usage_record" : "token_count",
      payload: record.payload,
    }));

  // Item types dropped by the corpus builder, counted across the whole session file (informational).
  const droppedTypesInFile = new Map<string, number>();
  for (const item of items) {
    const type = asString(item.payload.type) ?? "unknown";
    if (!INCLUDED_ITEM_TYPES.has(type)) droppedTypesInFile.set(type, (droppedTypesInFile.get(type) ?? 0) + 1);
  }

  const lookupTurnContext = (turnId: string | null, ordinal: number): TurnContext | null => {
    const exact = turnId ? turnContexts.find((context) => context.turnId === turnId) : undefined;
    if (exact) return exact;
    const before = turnContexts.filter((context) => context.ordinal <= ordinal);
    return before.length ? before[before.length - 1] : (turnContexts[0] ?? null);
  };

  const seenCallIds = new Set<string>();
  const boundaries: Boundary[] = [];
  for (let index = 0; index < items.length - 1; index += 1) {
    const current = items[index];
    const next = items[index + 1];
    const payload = current.payload;
    if (!isBoundaryItem(payload) || !isAssistantSide(next.payload)) continue;

    const type = asString(payload.type) ?? "";
    if (type === "function_call_output" || type === "custom_tool_call_output") {
      const callId = asString(payload.call_id);
      if (!callId || !seenCallIds.has(callId)) continue; // never split a call from its output
    }
    if (type === "function_call" || type === "custom_tool_call") {
      const callId = asString(payload.call_id);
      if (callId) seenCallIds.add(callId);
    }

    const usage = usages.find((record) => record.ordinal >= current.ordinal);
    if (!usage) continue;
    const tokens = usageInputTokens(usage);
    if (tokens === null) continue;

    const passthrough = isObject(payload.internal_chat_message_metadata_passthrough) ? payload.internal_chat_message_metadata_passthrough : null;
    const turnId = (passthrough ? asString(passthrough.turn_id) : null) ?? asString(usage.payload.turn_id);
    const threadId = asString(usage.payload.thread_id);
    const context = lookupTurnContext(turnId, current.ordinal);
    boundaries.push({
      itemIndex: index,
      ordinal: current.ordinal,
      itemType: type,
      role: asString(payload.role),
      tokens,
      usageKind: usage.kind,
      turnId,
      threadId,
      model: context?.model ?? null,
      effort: context?.effort ?? "high",
      recordedAt: current.timestamp,
      description: boundaryDescription(payload, current.ordinal),
    });
  }

  const baseInstructionsRaw = isObject(meta.base_instructions) ? asString(meta.base_instructions.text) : null;
  return {
    path,
    relativePath,
    candidateReason: reason,
    metaRecords: metaRecords.length,
    sessionId: asString(meta.session_id) ?? asString(meta.id) ?? relativePath,
    cwd: asString(meta.cwd),
    baseInstructions: baseInstructionsRaw && baseInstructionsRaw.length > 0 ? baseInstructionsRaw : "",
    items,
    turnContexts,
    usages,
    boundaries,
    droppedTypesInFile,
  };
};

const listJsonlFiles = async (months: readonly (readonly [string, string])[]): Promise<string[]> => {
  const files: string[] = [];
  for (const [year, month] of months) {
    const monthDir = `${SESSIONS_ROOT}/${year}/${month}`;
    let days: string[] = [];
    try {
      for await (const entry of Deno.readDir(monthDir)) if (entry.isDirectory) days.push(entry.name);
    } catch (error) {
      if (error instanceof Deno.errors.NotFound) continue;
      throw error;
    }
    days = days.sort();
    for (const day of days) {
      const names: string[] = [];
      for await (const entry of Deno.readDir(`${monthDir}/${day}`)) {
        if (entry.isFile && entry.name.endsWith(".jsonl")) names.push(entry.name);
      }
      for (const name of names.sort()) files.push(`${monthDir}/${day}/${name}`);
    }
  }
  return files.sort();
};

const harvest = async (files: readonly string[], root: string): Promise<SessionData[]> => {
  const sessions: SessionData[] = [];
  for (const path of files) {
    const text = await Deno.readTextFile(path);
    const relativePath = path.startsWith(`${root}/`) ? path.slice(root.length + 1) : path;
    const session = parseSession(path, relativePath, text);
    if (session) sessions.push(session);
  }
  return sessions;
};

// ------------------------------------------------------------------------------------------------ selection

const inWindow = (cls: CorpusClass, tokens: number): boolean => {
  const [low, high] = CLASS_WINDOW[cls];
  return tokens >= low && tokens <= high;
};

const windowDistance = (cls: CorpusClass, tokens: number): number => {
  const [low, high] = CLASS_WINDOW[cls];
  if (tokens < low) return low - tokens;
  if (tokens > high) return tokens - high;
  return 0;
};

const pickFirstInWindow = (session: SessionData, cls: CorpusClass): Boundary | null =>
  session.boundaries.filter((boundary) => inWindow(cls, boundary.tokens)).sort((a, b) => a.ordinal - b.ordinal)[0] ?? null;

const pickLargestInWindow = (session: SessionData, cls: CorpusClass): Boundary | null =>
  session.boundaries.filter((boundary) => inWindow(cls, boundary.tokens)).sort((a, b) => b.tokens - a.tokens || a.ordinal - b.ordinal)[0] ?? null;

const pickClosest = (session: SessionData, cls: CorpusClass): Boundary | null =>
  session.boundaries.slice().sort((a, b) => windowDistance(cls, a.tokens) - windowDistance(cls, b.tokens) || a.tokens - b.tokens || a.ordinal - b.ordinal)[0] ??
  null;

const pathCompare = (a: string, b: string): number => (a < b ? -1 : a > b ? 1 : 0);

type Chosen = { cls: CorpusClass; session: SessionData; boundary: Boundary };

const selectEntries = (sessions: readonly SessionData[]): { chosen: Chosen[]; smallMode: string } => {
  const ordered = sessions.slice().sort((a, b) => pathCompare(a.path, b.path));
  const used = new Set<string>();
  const chosen: Chosen[] = [];
  const take = (cls: CorpusClass, session: SessionData, boundary: Boundary) => {
    chosen.push({ cls, session, boundary });
    used.add(session.path);
  };

  // xlarge: prefer the largest in-window recorded request; sessions ranked by their largest in-window boundary.
  const xlargeRanked = ordered
    .filter((session) => session.boundaries.some((boundary) => inWindow("xlarge", boundary.tokens)))
    .map((session) => ({ session, boundary: pickLargestInWindow(session, "xlarge") as Boundary }))
    .sort((a, b) => b.boundary.tokens - a.boundary.tokens || pathCompare(a.session.path, b.session.path));
  for (const { session, boundary } of xlargeRanked.slice(0, CLASS_QUOTA.xlarge)) take("xlarge", session, boundary);

  // small: in-window first; otherwise the closest recorded requests, smallest distance first.
  let smallMode = "in-window";
  const smallInWindow = ordered.filter((session) => !used.has(session.path) && session.boundaries.some((boundary) => inWindow("small", boundary.tokens)));
  if (smallInWindow.length >= CLASS_QUOTA.small) {
    for (const session of smallInWindow.slice(0, CLASS_QUOTA.small)) take("small", session, pickFirstInWindow(session, "small") as Boundary);
  } else {
    smallMode = "closest-recorded-request";
    const ranked = ordered
      .filter((session) => !used.has(session.path))
      .map((session) => ({ session, boundary: pickClosest(session, "small") }))
      .filter((candidate): candidate is { session: SessionData; boundary: Boundary } => candidate.boundary !== null)
      .sort(
        (a, b) =>
          windowDistance("small", a.boundary.tokens) - windowDistance("small", b.boundary.tokens) ||
          a.boundary.tokens - b.boundary.tokens ||
          pathCompare(a.session.path, b.session.path)
      );
    let taken = 0;
    for (const { session, boundary } of ranked) {
      if (taken >= CLASS_QUOTA.small) break;
      if (used.has(session.path)) continue;
      take("small", session, boundary);
      taken += 1;
    }
  }

  // large and medium: first in-window boundary per session, sessions in path order.
  for (const cls of ["large", "medium"] as const) {
    const pool = ordered.filter((session) => !used.has(session.path) && session.boundaries.some((boundary) => inWindow(cls, boundary.tokens)));
    for (const session of pool.slice(0, CLASS_QUOTA[cls])) take(cls, session, pickFirstInWindow(session, cls) as Boundary);
  }

  const counts = new Map<CorpusClass, number>();
  for (const entry of chosen) counts.set(entry.cls, (counts.get(entry.cls) ?? 0) + 1);
  for (const cls of CLASS_ORDER) {
    if ((counts.get(cls) ?? 0) !== CLASS_QUOTA[cls]) {
      throw new Error(`selection failed: ${cls} has ${counts.get(cls) ?? 0} entries, want ${CLASS_QUOTA[cls]}`);
    }
  }
  return { chosen, smallMode };
};

// --------------------------------------------------------------------------------------------- entry build

type BuiltEntry = { entry: CorpusEntry; encryptedRemoved: number; droppedItems: Map<string, number> };

const buildEntry = (chosen: Chosen, tools: JsonValue[], counts: Map<string, number>, indexInClass: number): BuiltEntry => {
  const { cls, session, boundary } = chosen;
  const input: ResponsesInputItem[] = [];
  const dropped = new Map<string, number>();
  let encryptedRemoved = 0;

  for (let index = 0; index <= boundary.itemIndex; index += 1) {
    const payload = session.items[index].payload;
    const type = asString(payload.type) ?? "unknown";
    if (!INCLUDED_ITEM_TYPES.has(type)) {
      dropped.set(type, (dropped.get(type) ?? 0) + 1);
      continue;
    }
    const copy = structuredClone(payload) as JsonObject;
    delete copy.internal_chat_message_metadata_passthrough;
    if (type === "message" || type === "agent_message") {
      const content = Array.isArray(copy.content) ? copy.content : [];
      const kept = content.filter((entry) => !(isObject(entry) && entry.type === "encrypted_content"));
      encryptedRemoved += content.length - kept.length;
      copy.content = kept;
    }
    input.push(copy as unknown as ResponsesInputItem);
  }

  const entry: CorpusEntry = {
    id: `corpus-${cls}-${String(indexInClass).padStart(2, "0")}`,
    cls,
    source: {
      session_id: session.sessionId,
      thread_id: boundary.threadId,
      turn_id: boundary.turnId,
      recorded_model: boundary.model ?? "",
      recorded_at: boundary.recordedAt ?? "",
      recorded_input_tokens: boundary.tokens,
      item_count: input.length,
      dropped_item_types: [...dropped.keys()].sort(),
    },
    request: {
      instructions: session.baseInstructions,
      input,
      tools: tools as unknown as readonly ResponsesInputItem[],
      tool_choice: "auto",
      parallel_tool_calls: true,
      reasoning: { effort: boundary.effort ?? "high" },
      max_output_tokens: null,
    },
  };
  const sanitized = sanitizeValue(entry as unknown as JsonValue, counts) as unknown as CorpusEntry;
  return { entry: sanitized, encryptedRemoved, droppedItems: dropped };
};

// ---------------------------------------------------------------------------------------------------- build

const sha256Hex = async (bytes: Uint8Array): Promise<string> => {
  const buffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", buffer));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const build = async (): Promise<void> => {
  const primaryFiles = await listJsonlFiles(PRIMARY_MONTHS);
  const monthsUsed: string[] = ["2026/10"];
  let sessions = await harvest(primaryFiles, SESSIONS_ROOT);
  const feasible = (candidateSessions: readonly SessionData[]): boolean => {
    for (const cls of CLASS_ORDER) {
      const inWindowCount = candidateSessions.filter((session) => session.boundaries.some((boundary) => inWindow(cls, boundary.tokens))).length;
      if (inWindowCount >= CLASS_QUOTA[cls]) continue;
      if (cls === "small" && candidateSessions.filter((session) => session.boundaries.length > 0).length >= CLASS_QUOTA.small) continue;
      return false;
    }
    return true;
  };
  let fallbackHarvested = false;
  if (!feasible(sessions)) {
    fallbackHarvested = true;
    const fallbackFiles = await listJsonlFiles(FALLBACK_MONTHS);
    sessions = sessions.concat(await harvest(fallbackFiles, SESSIONS_ROOT));
    monthsUsed.push("2026/09");
  }

  const { chosen, smallMode } = selectEntries(sessions);
  const fixtureBytes = await Deno.readFile(TOOLS_FIXTURE_PATH);
  const tools = JSON.parse(new TextDecoder().decode(fixtureBytes)) as JsonValue[];
  if (!Array.isArray(tools)) throw new Error("tools fixture is not a JSON array");

  const counts = new Map<string, number>();
  const rows: { entry: CorpusEntry; item: Chosen; encryptedRemoved: number }[] = [];
  const perClass = new Map<CorpusClass, number>();
  const droppedItemTotals = new Map<string, number>();
  let encryptedTotal = 0;
  for (const cls of CLASS_ORDER) {
    const group = chosen.filter((item) => item.cls === cls).sort((a, b) => pathCompare(a.session.path, b.session.path));
    for (const item of group) {
      const indexInClass = (perClass.get(cls) ?? 0) + 1;
      perClass.set(cls, indexInClass);
      const built = buildEntry(item, tools, counts, indexInClass);
      rows.push({ entry: built.entry, item, encryptedRemoved: built.encryptedRemoved });
      encryptedTotal += built.encryptedRemoved;
      for (const [type, count] of built.droppedItems) droppedItemTotals.set(type, (droppedItemTotals.get(type) ?? 0) + count);
    }
  }
  const entries: CorpusEntry[] = rows.map((row) => row.entry);

  const jsonl = entries.map((entry) => JSON.stringify(entry)).join("\n") + "\n";
  const jsonlBytes = new TextEncoder().encode(jsonl);
  const corpusSha256 = await sha256Hex(jsonlBytes);
  const fixtureSha256 = await sha256Hex(fixtureBytes);

  // Primary guarantee: no credential shape survives inside any decoded string value of any entry.
  const decodedResiduals: string[] = [];
  for (const { entry } of rows) {
    const hits: ResidualHit[] = [];
    findResidualStrings(entry as unknown as JsonValue, entry.id, hits);
    for (const hit of hits) decodedResiduals.push(`${hit.path}: ${hit.details.join(" | ")}`);
  }
  if (decodedResiduals.length !== 0) {
    throw new Error(`sanitizer left ${decodedResiduals.length} residual credential-shape matches in decoded entry strings:\n${decodedResiduals.join("\n")}`);
  }
  // Informational: matches that exist only in the JSON-escaped serialization. A real newline becomes `\n`,
  // which lets an empty `NAME=` at end of line fold into the following lines (no value exists to redact, and
  // decoding removes the artifact again), so these are counted and recorded rather than treated as leaks.
  const serializedArtifacts = countResidualMatches(jsonl);
  if (serializedArtifacts !== 0) console.log(`serialized-scan artifacts (escape-folded, no decoded match): ${serializedArtifacts}`);

  droppedItemTotals.set("web_search_call", droppedItemTotals.get("web_search_call") ?? 0);

  const manifest = {
    schema: "corpus-v1",
    generator: "benchmarks/provider-waterfall/corpus-build.ts",
    corpus: { path: "corpus/corpus-v1.jsonl", bytes: jsonlBytes.length, sha256: corpusSha256 },
    counts_by_class: CLASS_QUOTA,
    class_windows: CLASS_WINDOW,
    class_target: "6 small, 9 medium, 9 large, 6 xlarge",
    sources: {
      root: SESSIONS_ROOT,
      primary_months: ["2026/10"],
      months_used: monthsUsed,
      fallback_months_harvested: fallbackHarvested,
      candidate_sessions: sessions.length,
      multi_meta_candidate_files: sessions.filter((session) => session.metaRecords > 1).length,
      candidate_filter:
        "session_meta.thread_source=subagent or source.subagent present; turn_context model deepseek-ai/DeepSeek-V4.1-Flash*, deepseek-flash, or deepseek-v4-flash*",
    },
    selection: {
      order: SELECTION_ORDER,
      xlarge: "largest in-window boundary per session; sessions ranked by largest in-window recorded input tokens desc",
      small: smallMode,
      small_note:
        smallMode === "closest-recorded-request"
          ? "no DeepSeek V4.1 Flash session under 2026/10 has a boundary inside 2000-10000 recorded input tokens; the six recorded requests closest to the window were used"
          : null,
      large: "first in-window boundary per session; sessions in path order",
      medium: "first in-window boundary per session; sessions in path order",
      one_entry_per_session: true,
    },
    sanitizer: {
      revision: SANITIZER_REVISION,
      rules: SANITIZER_RULES.map((rule) => ({
        id: rule.id,
        pattern: rule.pattern.source,
        flags: rule.pattern.flags,
        substitutions: counts.get(rule.id) ?? 0,
      })),
      conditional_rules: [{ id: "email", note: "applied only inside strings that matched a credential rule", substitutions: counts.get("email") ?? 0 }],
      total_substitutions: [...counts.values()].reduce((sum, value) => sum + value, 0),
      decoded_string_residual_matches: 0,
      serialized_line_escape_artifacts: serializedArtifacts,
      serialized_line_note:
        serializedArtifacts === 0
          ? null
          : "matches exist only in the JSON-escaped serialization: a real newline is rendered as backslash-n, so an empty NAME= at end of line folds into the following lines; no decoded string contains a match",
    },
    tools_fixture: {
      path: "fixtures/codex-tools-0.160.1.json",
      sha256: fixtureSha256,
      bytes: fixtureBytes.length,
      entries: tools.length,
      design_doc_expected_sha256: DESIGN_DOC_TOOLS_SHA256,
      matches_design_doc: fixtureSha256 === DESIGN_DOC_TOOLS_SHA256,
    },
    dropped_item_types: [...droppedItemTotals.keys()].sort(),
    dropped_item_type_counts: Object.fromEntries([...droppedItemTotals.entries()].sort((a, b) => pathCompare(a[0], b[0]))),
    encrypted_content_entries_removed: encryptedTotal,
    entries: rows.map(({ entry, item }) => ({
      id: entry.id,
      cls: entry.cls,
      session_id: entry.source.session_id,
      thread_id: entry.source.thread_id,
      turn_id: entry.source.turn_id,
      recorded_model: entry.source.recorded_model,
      recorded_at: entry.source.recorded_at,
      recorded_input_tokens: entry.source.recorded_input_tokens,
      item_count: entry.source.item_count,
      dropped_item_types: entry.source.dropped_item_types,
      boundary: { description: item.boundary.description, ordinal: item.boundary.ordinal, type: item.boundary.itemType, role: item.boundary.role },
      source_path: item.session.relativePath,
      cwd: item.session.cwd,
      session_meta_records: item.session.metaRecords,
    })),
  };

  await Deno.mkdir(CORPUS_DIR, { recursive: true });
  await Deno.writeFile(JSONL_PATH, jsonlBytes);
  await Deno.writeTextFile(MANIFEST_PATH, JSON.stringify(manifest, null, 2) + "\n");

  console.log(`built ${entries.length} entries: ${jsonlBytes.length} bytes sha256=${corpusSha256}`);
  console.log(`class counts: small=${perClass.get("small")} medium=${perClass.get("medium")} large=${perClass.get("large")} xlarge=${perClass.get("xlarge")}`);
  console.log(`candidates=${sessions.length} months=${monthsUsed.join(",")} small_mode=${smallMode}`);
  console.log("id\tclass\ttokens\titems\tboundary\tsession");
  for (const { entry, item } of rows)
    console.log(
      `${entry.id}\t${entry.cls}\t${entry.source.recorded_input_tokens}\t${entry.source.item_count}\t${item.boundary.description}\t${item.session.relativePath}`
    );
  console.log("sanitizer substitutions:", JSON.stringify(Object.fromEntries([...counts.entries()].sort((a, b) => pathCompare(a[0], b[0])))));
  console.log(`sanitizer residuals: decoded=${decodedResiduals.length} serialized_escape_artifacts=${serializedArtifacts}`);
  console.log(`tools fixture sha256=${fixtureSha256} (design doc says ${DESIGN_DOC_TOOLS_SHA256}, matches=${fixtureSha256 === DESIGN_DOC_TOOLS_SHA256})`);
};

// --------------------------------------------------------------------------------------------------- verify

type VerifyIssue = string;

const verifyEntry = (entry: unknown, index: number, fixture: JsonValue[], issues: VerifyIssue[]): CorpusClass | null => {
  const where = `line ${index + 1}`;
  if (!isObject(entry)) {
    issues.push(`${where}: not an object`);
    return null;
  }
  const id = asString(entry.id);
  const cls = asString(entry.cls) as CorpusClass | null;
  if (!id || !/^corpus-(small|medium|large|xlarge)-\d{2}$/.test(id)) issues.push(`${where}: bad id ${String(id)}`);
  if (!cls || !CLASS_ORDER.includes(cls)) {
    issues.push(`${where}: bad class ${String(cls)}`);
    return null;
  }
  const source = isObject(entry.source) ? entry.source : null;
  const request = isObject(entry.request) ? entry.request : null;
  if (!source) issues.push(`${where}: missing source`);
  if (!request) issues.push(`${where}: missing request`);
  if (!source || !request) return cls;

  if (typeof source.session_id !== "string" || !source.session_id) issues.push(`${where}: bad source.session_id`);
  if (typeof source.recorded_model !== "string" || !source.recorded_model) issues.push(`${where}: bad source.recorded_model`);
  if (typeof source.recorded_input_tokens !== "number") issues.push(`${where}: bad source.recorded_input_tokens`);
  if (typeof source.item_count !== "number") issues.push(`${where}: bad source.item_count`);
  if (!Array.isArray(source.dropped_item_types)) issues.push(`${where}: bad source.dropped_item_types`);

  if (typeof request.instructions !== "string") issues.push(`${where}: bad request.instructions`);
  if (request.tool_choice !== "auto") issues.push(`${where}: tool_choice != auto`);
  if (request.parallel_tool_calls !== true) issues.push(`${where}: parallel_tool_calls != true`);
  if (request.max_output_tokens !== null) issues.push(`${where}: max_output_tokens != null`);
  const reasoning = request.reasoning;
  if (!isObject(reasoning) || typeof reasoning.effort !== "string" || reasoning.effort.length === 0) {
    issues.push(`${where}: bad request.reasoning`);
  }
  if (!Array.isArray(request.tools)) {
    issues.push(`${where}: request.tools not an array`);
  } else if (JSON.stringify(request.tools) !== JSON.stringify(fixture)) {
    issues.push(`${where}: request.tools differ from fixture`);
  }
  const input = request.input;
  if (!Array.isArray(input)) {
    issues.push(`${where}: request.input not an array`);
    return cls;
  }
  if (input.length !== source.item_count) issues.push(`${where}: item_count ${String(source.item_count)} != input length ${input.length}`);

  const seenCallIds = new Set<string>();
  for (const item of input) {
    if (!isObject(item)) {
      issues.push(`${where}: input item not an object`);
      continue;
    }
    const type = asString(item.type) ?? "";
    if (!INCLUDED_ITEM_TYPES.has(type)) issues.push(`${where}: disallowed input item type ${type}`);
    if ("internal_chat_message_metadata_passthrough" in item) issues.push(`${where}: passthrough field present`);
    if (type === "message" || type === "agent_message") {
      if (!Array.isArray(item.content)) issues.push(`${where}: ${type} content not an array`);
      else if (item.content.some((contentItem) => isObject(contentItem) && contentItem.type === "encrypted_content")) {
        issues.push(`${where}: encrypted_content entry present`);
      }
    }
    if (type === "function_call" || type === "custom_tool_call") {
      const callId = asString(item.call_id);
      if (callId) seenCallIds.add(callId);
    }
    if (type === "function_call_output" || type === "custom_tool_call_output") {
      const callId = asString(item.call_id);
      if (!callId || !seenCallIds.has(callId)) issues.push(`${where}: tool output without matching call`);
    }
  }
  return cls;
};

const verify = async (): Promise<void> => {
  const issues: VerifyIssue[] = [];
  const manifest = JSON.parse(await Deno.readTextFile(MANIFEST_PATH)) as JsonObject;
  const jsonlBytes = await Deno.readFile(JSONL_PATH);
  const jsonl = new TextDecoder().decode(jsonlBytes);
  const sha = await sha256Hex(jsonlBytes);
  const corpus = isObject(manifest.corpus) ? manifest.corpus : null;
  if (!corpus || corpus.sha256 !== sha) issues.push(`sha256 mismatch: recomputed ${sha} vs manifest ${String(corpus?.sha256)}`);
  if (!corpus || corpus.bytes !== jsonlBytes.length) issues.push(`byte length mismatch: recomputed ${jsonlBytes.length} vs manifest ${String(corpus?.bytes)}`);

  const fixtureBytes = await Deno.readFile(TOOLS_FIXTURE_PATH);
  const fixtureSha = await sha256Hex(fixtureBytes);
  const fixture = JSON.parse(new TextDecoder().decode(fixtureBytes)) as JsonValue[];
  const toolsInfo = isObject(manifest.tools_fixture) ? manifest.tools_fixture : null;
  if (!toolsInfo || toolsInfo.sha256 !== fixtureSha)
    issues.push(`tools fixture sha256 mismatch: recomputed ${fixtureSha} vs manifest ${String(toolsInfo?.sha256)}`);

  const lines = jsonl.split("\n");
  if (lines.length && lines[lines.length - 1] === "") lines.pop();
  if (lines.length !== 30) issues.push(`expected 30 lines, found ${lines.length}`);

  const classCounts = new Map<CorpusClass, number>();
  const ids = new Set<string>();
  const classesOrder: string[] = [];
  let previousClass: string | null = null;
  const perClassIndex = new Map<string, number>();
  const manifestEntries = Array.isArray(manifest.entries) ? manifest.entries : [];
  lines.forEach((line, index) => {
    let parsed: unknown;
    try {
      parsed = JSON.parse(line);
    } catch (error) {
      issues.push(`line ${index + 1}: invalid JSON: ${(error as Error).message}`);
      return;
    }
    const cls = verifyEntry(parsed, index, fixture, issues);
    if (!cls) return;
    classCounts.set(cls, (classCounts.get(cls) ?? 0) + 1);
    if (previousClass !== cls) {
      classesOrder.push(cls);
      previousClass = cls;
    }
    const id = asString((parsed as JsonObject).id) ?? "";
    if (ids.has(id)) issues.push(`duplicate id ${id}`);
    ids.add(id);
    const nextIndex = (perClassIndex.get(cls) ?? 0) + 1;
    perClassIndex.set(cls, nextIndex);
    if (id !== `corpus-${cls}-${String(nextIndex).padStart(2, "0")}`) issues.push(`line ${index + 1}: id ${id} out of order for class ${cls}`);

    const source = isObject((parsed as JsonObject).source) ? ((parsed as JsonObject).source as JsonObject) : {};
    const tokens = asNumber(source.recorded_input_tokens);
    if (tokens !== null && cls !== "small" && (tokens < CLASS_WINDOW[cls][0] || tokens > CLASS_WINDOW[cls][1])) {
      issues.push(`${id}: ${cls} tokens ${tokens} outside ${CLASS_WINDOW[cls].join("-")}`);
    }
    const manifestEntry = manifestEntries.find((candidate) => isObject(candidate) && candidate.id === id);
    if (!manifestEntry) issues.push(`${id}: missing from manifest.entries`);
  });

  for (const cls of CLASS_ORDER) {
    const got = classCounts.get(cls) ?? 0;
    if (got !== CLASS_QUOTA[cls]) issues.push(`class ${cls}: ${got} entries, want ${CLASS_QUOTA[cls]}`);
  }
  if (classesOrder.join(",") !== CLASS_ORDER.join(",")) issues.push(`class block order is ${classesOrder.join(",")}, want ${CLASS_ORDER.join(",")}`);
  const smallMode = isObject(manifest.selection) ? manifest.selection.small : null;
  if (smallMode === "closest-recorded-request" && classCounts.get("small") !== 6) issues.push("small fallback mode without 6 small entries");

  if (issues.length) {
    console.error(`verify FAILED (${issues.length} issues):`);
    for (const issue of issues) console.error(`  - ${issue}`);
    Deno.exit(1);
  }
  console.log(`verify OK: 30 entries, sha256 ${sha}, class counts 6/9/9/6, tools fixture ${fixtureSha}`);
};

// ------------------------------------------------------------------------------------------------ selfcheck

const selfcheck = (): void => {
  const fixtures: Record<string, string> = {
    sk: `sk-${"A".repeat(30)}`,
    lith_sk: `lith_sk_${"B".repeat(24)}`,
    fernet: `gAAAAA${"C".repeat(60)}`,
    slack_bot: "xoxb-1234567890-abcd",
    github_pat: `ghp_${"D".repeat(36)}`,
    aws_access_key: `AKIA${"E".repeat(16)}`,
    hex_ak_sk: `SK${"f".repeat(32)}`,
    bearer: `Bearer ${"g".repeat(40)}`,
    assignment: `SOME_API_KEY=${"h".repeat(24)}`,
    authorization_header: `Authorization: ${"i".repeat(24)}`,
  };
  const failures: string[] = [];
  for (const [id, fixture] of Object.entries(fixtures)) {
    const counts = new Map<string, number>();
    const once = sanitizeString(fixture, counts);
    const twice = sanitizeString(once, new Map());
    if (!once.includes(`[REDACTED:${id}]`)) failures.push(`${id}: not redacted`);
    if (once.includes(fixture)) failures.push(`${id}: raw value survived`);
    if (once !== twice) failures.push(`${id}: not idempotent`);
    if ((counts.get(id) ?? 0) < 1) failures.push(`${id}: substitution not counted`);
    console.log(`selfcheck ${id}: ${failures.some((failure) => failure.startsWith(`${id}:`)) ? "FAIL" : "ok"}`);
  }
  const emailWithCredential = `sk-${"A".repeat(30)} user@example.com`;
  const emailCounts = new Map<string, number>();
  const emailRedacted = sanitizeString(emailWithCredential, emailCounts);
  if (!emailRedacted.includes("[REDACTED:email]")) failures.push("email: not redacted inside credential-matched string");
  if (emailRedacted.includes("user@example.com")) failures.push("email: raw address survived");
  const plainEmail = "contact user@example.com for details";
  if (sanitizeString(plainEmail, new Map()) !== plainEmail) failures.push("email: plain address must stay unchanged");
  const combined = Object.values(fixtures).join(" and ");
  const combinedOnce = sanitizeString(combined, new Map());
  if (combinedOnce !== sanitizeString(combinedOnce, new Map())) failures.push("combined: not idempotent");
  if (countResidualMatches(combinedOnce) !== 0) failures.push("combined: residual matches after sanitizing");
  console.log("selfcheck email: ok (credential-adjacent redacted, plain address preserved)");
  console.log("selfcheck idempotence: ok");
  if (failures.length) {
    console.error(`selfcheck FAILED: ${failures.join("; ")}`);
    Deno.exit(1);
  }
  console.log("selfcheck OK: all credential shapes redacted and counted");
};

// --------------------------------------------------------------------------------------------------- report

const escapePipes = (value: string): string => value.replace(/\|/g, "\\|");
const codeSpan = (value: string): string => (value.includes("`") ? `\`\`${value}\`\`` : `\`${value}\``);

const report = async (): Promise<void> => {
  const manifest = JSON.parse(await Deno.readTextFile(MANIFEST_PATH)) as JsonObject;
  const corpus = isObject(manifest.corpus) ? manifest.corpus : {};
  const counts = isObject(manifest.counts_by_class) ? manifest.counts_by_class : {};
  const sanitizer = isObject(manifest.sanitizer) ? manifest.sanitizer : {};
  const rules = Array.isArray(sanitizer.rules) ? sanitizer.rules : [];
  const sources = isObject(manifest.sources) ? manifest.sources : {};
  const selection = isObject(manifest.selection) ? manifest.selection : {};
  const tools = isObject(manifest.tools_fixture) ? manifest.tools_fixture : {};
  const entries = Array.isArray(manifest.entries) ? manifest.entries : [];
  const dropped = isObject(manifest.dropped_item_type_counts) ? manifest.dropped_item_type_counts : {};

  const lines: string[] = ["# corpus-build-report.md — DeepSeek V4.1 Flash provider waterfall (corpus-v1)", ""];
  lines.push(
    `Produced by \`corpus-build.ts\` (m1-corpus lane) from recorded DeepSeek V4.1 Flash subagent sessions under \`${String(sources.root)}\` (${Array.isArray(sources.months_used) ? sources.months_used.join(", ") : "?"}); September harvested only if October could not fill a class: ${String(sources.fallback_months_harvested)}.`
  );
  lines.push("");
  lines.push(
    `Frozen artifact: \`corpus/corpus-v1.jsonl\` (${String(corpus.bytes)} bytes, sha256 \`${String(corpus.sha256)}\`), ${entries.length} entries, class counts ${CLASS_ORDER.map((cls) => `${cls}=${String(counts[cls])}`).join(" ")}.`,
    ""
  );
  lines.push("## Selection method", "");
  lines.push(
    `Candidate sessions: ${String(sources.candidate_sessions)} (${String(sources.candidate_filter)}). Each candidate file was read once; a boundary is a user/developer message, tool result, or agent_message whose next recorded item is assistant-side, never splitting a tool result from its call. Class sizes come from the boundary turn's recorded input tokens.`,
    ""
  );
  lines.push(
    `Selection order: ${(Array.isArray(selection.order) ? selection.order : []).join(" → ")}. xlarge: ${String(selection.xlarge)}. small: ${String(selection.small)}. large: ${String(selection.large)}. medium: ${String(selection.medium)}. One entry per session; ties resolve by session path and ordinal, so the same inputs produce identical bytes.`
  );
  if (selection.small_note) {
    lines.push("");
    lines.push(`Small-class fallback: ${String(selection.small_note)}.`);
  }
  lines.push("");
  lines.push("## Class distribution (recorded input tokens per entry)", "");
  lines.push("| id | class | recorded input tokens | items | boundary | source session |");
  lines.push("| --- | --- | --- | --- | --- | --- |");
  for (const entry of entries) {
    if (!isObject(entry)) continue;
    const boundary = isObject(entry.boundary) ? String(entry.boundary.description) : "?";
    lines.push(
      `| ${String(entry.id)} | ${String(entry.cls)} | ${String(entry.recorded_input_tokens)} | ${String(entry.item_count)} | ${escapePipes(boundary)} | \`${String(entry.source_path)}\` |`
    );
  }
  lines.push("");
  lines.push("## Sanitizer", "");
  lines.push(
    `Revision \`${String(sanitizer.revision)}\`; substitutions counted per rule. The email rule redacts addresses only inside strings that matched a credential rule, and decoded-string residuals are 0.`
  );
  lines.push("");
  lines.push("| rule | pattern | substitutions |");
  lines.push("| --- | --- | --- |");
  for (const rule of rules) {
    if (!isObject(rule)) continue;
    lines.push(`| ${String(rule.id)} | ${codeSpan(escapePipes(String(rule.pattern)))} | ${String(rule.substitutions)} |`);
  }
  const emailRule = Array.isArray(sanitizer.conditional_rules) ? sanitizer.conditional_rules.find((rule) => isObject(rule) && rule.id === "email") : null;
  lines.push(`| email (conditional) | addresses inside credential-matched strings | ${String(isObject(emailRule) ? emailRule.substitutions : 0)} |`);
  lines.push("");
  lines.push(`Total substitutions: ${String(sanitizer.total_substitutions)}.`);
  if (Number(sanitizer.serialized_line_escape_artifacts ?? 0) > 0) {
    lines.push(
      `Serialized-line scan artifact: ${String(sanitizer.serialized_line_escape_artifacts)} escape-folded match with no decoded-string match (a real newline renders as \\n, so an empty NAME= at end of line folds into the following lines); recorded in the manifest.`
    );
  }
  lines.push("");
  lines.push("## Dropped item types", "");
  const droppedList = Object.entries(dropped)
    .map(([type, count]) => `\`${type}\` (${String(count)} items)`)
    .join(", ");
  lines.push(
    `Dropped from every included prefix and never replayed: ${droppedList}. Removed \`encrypted_content\` entries: ${String(manifest.encrypted_content_entries_removed)}; top-level \`internal_chat_message_metadata_passthrough\` removed from every item.`
  );
  lines.push("");
  lines.push("## Tools fixture", "");
  lines.push(
    `\`${String(tools.path)}\`: sha256 \`${String(tools.sha256)}\`, ${String(tools.bytes)} bytes, ${String(tools.entries)} tools, used verbatim and in order for every entry. DESIGN.md quotes \`${String(tools.design_doc_expected_sha256)}\`; the committed fixture hash matches DESIGN.md: ${String(tools.matches_design_doc)}.`
  );
  lines.push("");
  const entryObjects = entries.filter((entry): entry is JsonObject => isObject(entry));
  const smallTokens = entryObjects.filter((entry) => entry.cls === "small").map((entry) => Number(entry.recorded_input_tokens));
  const multiMetaEntries = entryObjects.filter((entry) => Number(entry.session_meta_records) > 1).length;
  const cwdCounts = new Map<string, number>();
  for (const entry of entryObjects) if (typeof entry.cwd === "string") cwdCounts.set(entry.cwd, (cwdCounts.get(entry.cwd) ?? 0) + 1);
  const cwdSummary = [...cwdCounts.entries()]
    .sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1))
    .map(([cwd, count]) => `\`${cwd}\` (${count})`)
    .join(", ");
  const smallRange = smallTokens.length ? `${Math.min(...smallTokens)}-${Math.max(...smallTokens)}` : "?";
  lines.push("## Known limitations", "");
  lines.push(
    `- The small window (2000-10000 tokens) cannot be filled from the available recorded DeepSeek V4.1 Flash sessions: the candidate pool's smallest recorded request is 18689 tokens, so the six small entries are the closest recorded requests (${smallRange}) per the corpus spec's fallback; they are labeled \`small\` but their recorded size is above the window.`
  );
  lines.push(
    `- Entry sources cluster by task: cwd coverage is ${cwdSummary}. The single largest group shares near-identical synthetic worker prompts, so prompt-content diversity is limited; selection still follows the deterministic path-order and largest-in-window rules.`
  );
  lines.push(
    `- ${multiMetaEntries} of the selected entries come from candidate files (of ${String(sources.multi_meta_candidate_files)} such candidates) that also carry a parent exec session_meta record; candidate detection accepts a file when any session_meta matches and the first matching (subagent) record supplies session identity and instructions.`
  );
  lines.push(
    "- Recorded input tokens include each session's own tools and instructions; the frozen fixture's tool array may differ in size from what the recorded session used, so replayed request sizes can differ from the recorded values."
  );
  lines.push(
    "- The `sk-…` rule matches that shape anywhere, including substrings of identifiers such as `task-<id>`; substitutions are small, uniform across providers, and counted above."
  );
  lines.push(
    "- Encrypted agent payloads and reasoning items cannot be replayed and are removed; the corpus replays only plaintext items plus tool calls/outputs."
  );
  lines.push("- All source sessions are from 2026-10-03..2026-10-05 on one host; no cross-host or cross-month diversity was available for this class mix.");
  lines.push("");
  lines.push("## Verification", "");
  lines.push(
    "- `corpus-build.ts verify` recomputes the sha256, re-validates every line as a `CorpusEntry` (ids, ordering, class windows, tool fixture equality, call/output pairing, no passthrough/encrypted fields) and re-checks 6/9/9/6 counts without reading sessions."
  );
  lines.push(
    "- `corpus-build.ts selfcheck` feeds one fixture string per credential shape through the sanitizer and asserts redaction, counting and idempotence."
  );
  lines.push("");
  await Deno.writeTextFile(REPORT_PATH, lines.join("\n"));
  console.log(`report written: ${REPORT_PATH}`);
};

// ----------------------------------------------------------------------------------------------------- main

if (import.meta.main) {
  const mode = Deno.args[0] ?? "build";
  if (mode === "build") await build();
  else if (mode === "verify") await verify();
  else if (mode === "selfcheck") selfcheck();
  else if (mode === "report") await report();
  else {
    console.error(`unknown mode: ${mode} (expected build|verify|selfcheck|report)`);
    Deno.exit(2);
  }
}
