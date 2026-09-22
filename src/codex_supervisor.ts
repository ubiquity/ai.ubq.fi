import { json, openaiError } from "./http.ts";
import { openSupervisorConnection, type SupervisorConnection } from "./codex_supervisor_transport.ts";

/**
 * Read-only Codex supervisor inventory.
 *
 * This module answers one question for a super admin: which Codex sessions are
 * running where, right now, and what is each one waiting on. It never steers a
 * session: it connects to existing Codex app-server control sockets, calls only
 * allowlisted read methods, and reports `unknown` whenever a read fails instead
 * of guessing.
 *
 * Optional configuration lives in `.data/codex-supervisor.json` relative to the
 * runtime working directory:
 *
 *   { "sources": [{ "id": "local", "name": "This Mac", "socketPath": "/…/app-server-control.sock", "codexHome": "/…/.codex" }] }
 *
 * Live thread fields (title, model, effort, cwd, lineage, runtime state) come
 * from each machine's own app-server for local and remote sources alike. Only a
 * source that names an explicit `codexHome` may additionally read that machine's
 * read-only state database for persisted branch and token usage. Sources are
 * trusted local configuration: HTTP input can select a configured source id, but
 * can never add a path.
 */

const SUPERVISOR_CONFIG_PATH = ".data/codex-supervisor.json";

const MAX_SOURCES = 8;
const SOURCE_CONCURRENCY = 2;
const MAX_SOURCE_NAME_LENGTH = 64;
export const SOURCE_ID_PATTERN = /^[a-z0-9][a-z0-9_-]{0,31}$/;
export const THREAD_ID_PATTERN = /^[A-Za-z0-9._:-]{1,200}$/;

const INVENTORY_LIMIT = 60;
const LIST_PAGE_LIMIT = 50;
const MAX_LIST_PAGES = 2;
const LIVE_PROBE_LIMIT = 24;
const EXTRA_LOADED_LIMIT = 8;
const PROBE_CONCURRENCY = 4;
const OVERALL_DEADLINE_MS = 9_000;

const SNAPSHOT_TTL_MS = 4_000;
const RECENT_METADATA_LIMIT = 60;
const STATE_DATABASE_PATTERN = /^state_(\d+)\.sqlite$/;
const SQLITE_MODULE_SPECIFIER = "node:sqlite";

const FOLLOW_POLL_MS = 2_000;
const FOLLOW_HEARTBEAT_EVERY = 5;
const FOLLOW_MAX_DURATION_MS = 600_000;
const FOLLOW_MAX_ENTRIES = 120;
const FOLLOW_MAX_TEXT_CHARS = 4_000;
const FOLLOW_MAX_COMMAND_CHARS = 400;

/** Every non-interactive source kind, requested explicitly so subagents are never hidden. */
const LIST_SOURCE_KINDS = [
  "cli",
  "vscode",
  "exec",
  "appServer",
  "subAgent",
  "subAgentReview",
  "subAgentCompact",
  "subAgentThreadSpawn",
  "subAgentOther",
  "unknown",
];

const METADATA_COLUMNS =
  "id, title, name, preview, cwd, model, reasoning_effort, tokens_used, git_branch, updated_at, updated_at_ms, archived, thread_source, source";

const TERMINAL_TURN_STATES: ReadonlySet<string> = new Set(["completed", "interrupted", "failed", "declined", "cancelled", "canceled", "errored", "aborted"]);

type SupervisorState = "active" | "idle" | "stale" | "unknown" | "system_error";

export type SupervisorSource = {
  id: string;
  name: string;
  socketPath: string;
  codexHome: string | null;
};

type SupervisorConfig = {
  sources: SupervisorSource[];
  configPath: string;
  configLoaded: boolean;
  notes: string[];
};

type SupervisorQuotaBucket = {
  limitId: string;
  limitName: string | null;
  usedPercent: number | null;
  windowDurationMins: number | null;
  resetsAtMs: number | null;
  planType: string | null;
  reachedType: string | null;
};

type SupervisorQuota = {
  accountScope: "shared_codex_account";
  buckets: SupervisorQuotaBucket[];
  resetCreditsAvailable: number | null;
};

type SupervisorSession = {
  id: string;
  sourceId: string;
  machine: string;
  title: string | null;
  titleSource: "thread" | "metadata" | null;
  cwd: string | null;
  branch: string | null;
  model: string | null;
  effort: string | null;
  provider: string | null;
  state: SupervisorState;
  activeFlags: string[];
  loaded: boolean | null;
  waitingOnApproval: boolean;
  waitingOnUserInput: boolean;
  sourceKind: string | null;
  parentThreadId: string | null;
  childIds: string[] | null;
  lastActivityAtMs: number | null;
  lastSampledAtMs: number;
  tokensUsed: number | null;
  usageSource: "state_db" | null;
  sampled: boolean;
  unavailable: string[];
};

type SupervisorSourceView = {
  id: string;
  name: string;
  kind: "local" | "remote";
  hostname: string | null;
  state: "ok" | "unavailable";
  reason: string | null;
  metadata: { available: boolean; database: string | null; reason: string | null };
  inventory: { listed: number; sampled: number; returned: number; truncated: boolean };
  lineage: "available" | "unavailable";
  quota: SupervisorQuota | null;
  quotaReason: string | null;
  durationMs: number;
  sampledAtMs: number;
};

type SupervisorSnapshot = {
  sampledAtMs: number;
  view: "codex_app_server_read_only";
  sources: SupervisorSourceView[];
  sessions: SupervisorSession[];
  counts: {
    total: number;
    active: number;
    waiting: number;
    idle: number;
    stale: number;
    unknown: number;
    systemError: number;
  };
  coverage: { listed: number; sampled: number; truncated: boolean; notes: string[] };
};

type JsonRecord = Record<string, unknown>;

const isRecord = (value: unknown): value is JsonRecord => typeof value === "object" && value !== null && !Array.isArray(value);

const isUnknownArray = (value: unknown): value is unknown[] => Array.isArray(value);

const asString = (value: unknown): string | null => (typeof value === "string" ? value : null);

const asFiniteNumber = (value: unknown): number | null => (typeof value === "number" && Number.isFinite(value) ? value : null);

const asBoolean = (value: unknown): boolean | null => (typeof value === "boolean" ? value : null);

const joinPath = (base: string, leaf: string): string => (base.endsWith("/") ? `${base}${leaf}` : `${base}/${leaf}`);

/** Accepts either Unix seconds or Unix milliseconds and returns milliseconds. */
export const normalizeEpochMs = (value: unknown): number | null => {
  const numeric = asFiniteNumber(value);
  if (numeric === null || numeric <= 0) return null;
  return numeric < 1_000_000_000_000 ? Math.round(numeric * 1000) : Math.round(numeric);
};

const textOrNull = (value: unknown): string | null => {
  const text = asString(value);
  if (text === null) return null;
  const trimmed = text.trim();
  return trimmed.length > 0 ? trimmed : null;
};

const uniqueStrings = (values: readonly string[]): string[] => [...new Set(values)];

const boundedText = (value: string, limit: number): string => (value.length <= limit ? value : `${value.slice(0, limit)}…`);

const nowMs = (): number => Date.now();

const readHostname = (): string | null => {
  try {
    return Deno.hostname();
  } catch {
    return null;
  }
};

const waitForPoll = (ms: number, signal: AbortSignal): Promise<void> =>
  new Promise<void>((resolve) => {
    const onAbort = (): void => {
      clearTimeout(timer);
      resolve();
    };
    const timer = setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve();
    }, ms);
    signal.addEventListener("abort", onAbort, { once: true });
  });

const mapWithConcurrency = async <TItem, TResult>(items: readonly TItem[], limit: number, run: (item: TItem) => Promise<TResult>): Promise<TResult[]> => {
  const results: TResult[] = [];
  let next = 0;
  const workerCount = Math.max(1, Math.min(limit, items.length));
  const workers: Promise<void>[] = [];
  for (let index = 0; index < workerCount; index += 1) {
    workers.push(
      (async () => {
        while (next < items.length) {
          const current = next;
          next += 1;
          results[current] = await run(items[current]);
        }
      })()
    );
  }
  await Promise.all(workers);
  return results;
};

/* ------------------------------------------------------------------ config */

const supervisorSourceOf = (entry: unknown, seen: ReadonlySet<string>, notes: string[]): SupervisorSource | null => {
  if (!isRecord(entry)) {
    notes.push("ignored a source that is not an object");
    return null;
  }
  const id = textOrNull(entry.id);
  const name = textOrNull(entry.name);
  const socketPath = textOrNull(entry.socketPath);
  const codexHome = entry.codexHome === undefined || entry.codexHome === null ? null : textOrNull(entry.codexHome);
  if (!id || !SOURCE_ID_PATTERN.test(id)) {
    notes.push("ignored a source whose id must match /^[a-z0-9][a-z0-9_-]{0,31}$/");
    return null;
  }
  if (seen.has(id)) {
    notes.push(`ignored duplicate source id ${id}`);
    return null;
  }
  if (!name || name.length > MAX_SOURCE_NAME_LENGTH) {
    notes.push(`ignored source ${id}: name must be 1-${MAX_SOURCE_NAME_LENGTH} characters`);
    return null;
  }
  if (!socketPath?.startsWith("/")) {
    notes.push(`ignored source ${id}: socketPath must be an absolute path`);
    return null;
  }
  if (codexHome !== null && !codexHome.startsWith("/")) {
    notes.push(`ignored source ${id}: codexHome must be an absolute path`);
    return null;
  }
  return { id, name, socketPath, codexHome };
};

/** Validates the optional configuration file. Pure, so tests can drive it directly. */
export const parseSupervisorConfig = (raw: unknown): { sources: SupervisorSource[]; notes: string[] } => {
  const notes: string[] = [];
  if (!isRecord(raw)) return { sources: [], notes: ["configuration must be a JSON object"] };
  const entries = isUnknownArray(raw.sources) ? raw.sources : [];
  if (entries.length === 0) return { sources: [], notes: ["configuration lists no sources"] };
  const sources: SupervisorSource[] = [];
  const seen = new Set<string>();
  for (const entry of entries.slice(0, MAX_SOURCES)) {
    const source = supervisorSourceOf(entry, seen, notes);
    if (!source) continue;
    seen.add(source.id);
    sources.push(source);
  }
  if (entries.length > MAX_SOURCES) notes.push(`only the first ${MAX_SOURCES} sources are used`);
  return { sources, notes };
};

/** The fallback source: this machine's own Codex home, with no configuration file at all. */
export const defaultSupervisorSource = (env: (name: string) => string | undefined, notes: string[]): SupervisorSource | null => {
  const home = (env("HOME") ?? "").trim();
  const codexHome = (env("CODEX_HOME") ?? "").trim() || (home ? joinPath(home, ".codex") : "");
  if (!codexHome) {
    notes.push("HOME and CODEX_HOME are both unavailable, so no local source could be derived");
    return null;
  }
  return { id: "local", name: "Local Codex", socketPath: joinPath(codexHome, "app-server-control/app-server-control.sock"), codexHome };
};

const readEnvironment = (name: string): string | undefined => {
  try {
    return Deno.env.get(name);
  } catch {
    return undefined;
  }
};

export const resolveSupervisorConfig = async (options?: { cwd?: string; env?: (name: string) => string | undefined }): Promise<SupervisorConfig> => {
  const cwd = options?.cwd ?? Deno.cwd();
  const env = options?.env ?? readEnvironment;
  const configPath = joinPath(cwd, SUPERVISOR_CONFIG_PATH);
  const notes: string[] = [];
  let raw: unknown = null;
  let configLoaded = false;
  try {
    raw = JSON.parse(await Deno.readTextFile(configPath));
    configLoaded = true;
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) notes.push(`${SUPERVISOR_CONFIG_PATH} could not be read; using the default local source`);
  }
  if (configLoaded) {
    const parsed = parseSupervisorConfig(raw);
    notes.push(...parsed.notes);
    if (parsed.sources.length > 0) return { sources: parsed.sources, configPath, configLoaded, notes };
    notes.push("no usable source in the configuration file; using the default local source");
  }
  const fallback = defaultSupervisorSource(env, notes);
  return { sources: fallback ? [fallback] : [], configPath, configLoaded, notes };
};

/* ---------------------------------------------------------------- metadata */

type ThreadMetadata = {
  title: string | null;
  preview: string | null;
  cwd: string | null;
  model: string | null;
  effort: string | null;
  tokensUsed: number | null;
  gitBranch: string | null;
  updatedAtMs: number | null;
  archived: boolean | null;
  threadSource: string | null;
  parentThreadId: string | null;
  agentNickname: string | null;
};

type SqliteStatementLike = { all: (...params: unknown[]) => unknown[] };
type SqliteDatabaseLike = { prepare: (sql: string) => SqliteStatementLike; close: () => void };
type SqliteDatabaseConstructor = new (path: string, options?: { readOnly?: boolean }) => SqliteDatabaseLike;

const openReadOnlySqlite = async (path: string): Promise<SqliteDatabaseLike | null> => {
  try {
    const module: unknown = await import(SQLITE_MODULE_SPECIFIER);
    if (!isRecord(module)) return null;
    const { DatabaseSync } = module;
    if (typeof DatabaseSync !== "function") return null;
    return new (DatabaseSync as SqliteDatabaseConstructor)(path, { readOnly: true });
  } catch {
    return null;
  }
};

const findStateDatabase = async (codexHome: string): Promise<string | null> => {
  let best: { path: string; version: number } | null = null;
  try {
    for await (const entry of Deno.readDir(codexHome)) {
      if (!entry.isFile) continue;
      const match = STATE_DATABASE_PATTERN.exec(entry.name);
      if (!match) continue;
      const version = Number(match[1]);
      if (!Number.isSafeInteger(version)) continue;
      if (!best || version > best.version) best = { path: joinPath(codexHome, entry.name), version };
    }
  } catch {
    return null;
  }
  return best ? best.path : null;
};

const lineageFromSourceJson = (value: unknown): { parentThreadId: string | null; agentNickname: string | null } => {
  const raw = textOrNull(value);
  if (!raw) return { parentThreadId: null, agentNickname: null };
  try {
    const parsed: unknown = JSON.parse(raw);
    if (!isRecord(parsed)) return { parentThreadId: null, agentNickname: null };
    const nested = isRecord(parsed.subagent) ? parsed.subagent : null;
    const parent = textOrNull(parsed.parent_thread_id) ?? (nested ? textOrNull(nested.parent_thread_id) : null);
    const nickname = textOrNull(parsed.agent_nickname) ?? (nested ? textOrNull(nested.agent_nickname) : null);
    return { parentThreadId: parent, agentNickname: nickname };
  } catch {
    return { parentThreadId: null, agentNickname: null };
  }
};

const metadataFromRow = (row: JsonRecord): ThreadMetadata => {
  const lineage = lineageFromSourceJson(row.source);
  return {
    title: textOrNull(row.name) ?? textOrNull(row.title),
    preview: textOrNull(row.preview),
    cwd: textOrNull(row.cwd),
    model: textOrNull(row.model),
    effort: textOrNull(row.reasoning_effort),
    tokensUsed: asFiniteNumber(row.tokens_used),
    gitBranch: textOrNull(row.git_branch),
    updatedAtMs: normalizeEpochMs(row.updated_at_ms) ?? normalizeEpochMs(row.updated_at),
    archived: asBoolean(row.archived),
    threadSource: textOrNull(row.thread_source),
    parentThreadId: lineage.parentThreadId,
    agentNickname: lineage.agentNickname,
  };
};

/**
 * Reads persisted thread metadata with a read-only connection.
 *
 * `ids === null` returns the most recently updated threads, which is the honest
 * fallback when app-server cannot be reached. History databases and rollout
 * files are never opened.
 */
const readThreadMetadata = async (
  codexHome: string,
  ids: readonly string[] | null
): Promise<{ database: string | null; rows: Map<string, ThreadMetadata>; reason: string | null }> => {
  const rows = new Map<string, ThreadMetadata>();
  if (ids !== null && ids.length === 0) return { database: null, rows, reason: null };
  const databasePath = await findStateDatabase(codexHome);
  if (!databasePath) return { database: null, rows, reason: "state database not found" };
  const database = await openReadOnlySqlite(databasePath);
  if (!database) return { database: databasePath, rows, reason: "state database could not be opened read-only" };
  try {
    const statement =
      ids === null
        ? database.prepare(`SELECT ${METADATA_COLUMNS} FROM threads ORDER BY updated_at DESC LIMIT ?`)
        : database.prepare(`SELECT ${METADATA_COLUMNS} FROM threads WHERE id IN (${ids.map(() => "?").join(",")})`);
    const result = ids === null ? statement.all(RECENT_METADATA_LIMIT) : statement.all(...ids);
    for (const value of result) {
      if (!isRecord(value)) continue;
      const id = textOrNull(value.id);
      if (!id) continue;
      rows.set(id, metadataFromRow(value));
    }
    return { database: databasePath, rows, reason: null };
  } catch {
    return { database: databasePath, rows, reason: "state database query failed" };
  } finally {
    try {
      database.close();
    } catch {
      // A close failure cannot change the result already read.
    }
  }
};

/* ---------------------------------------------------------------- sampling */

type ListedThread = {
  id: string;
  name: string | null;
  preview: string | null;
  cwd: string | null;
  model: string | null;
  effort: string | null;
  sourceKind: string | null;
  parentThreadId: string | null;
  updatedAtMs: number | null;
  modelProvider: string | null;
};

type ThreadProbe = {
  runtimeStatus: string | null;
  activeFlags: string[];
  turnStatus: string | null;
  thread: ListedThread | null;
  sampled: boolean;
};

const listedThreadOf = (value: unknown): ListedThread | null => {
  if (!isRecord(value)) return null;
  const id = textOrNull(value.id);
  if (!id || !THREAD_ID_PATTERN.test(id)) return null;
  return {
    id,
    name: textOrNull(value.name),
    preview: textOrNull(value.preview),
    cwd: textOrNull(value.cwd),
    model: textOrNull(value.model),
    effort: textOrNull(value.reasoningEffort),
    sourceKind: textOrNull(value.source),
    parentThreadId: textOrNull(value.parentThreadId),
    updatedAtMs: normalizeEpochMs(value.updatedAt),
    modelProvider: textOrNull(value.modelProvider),
  };
};

const newestTurnOf = (result: unknown): { id: string | null; status: string | null; items: unknown } | null => {
  if (!isRecord(result) || !isUnknownArray(result.data)) return null;
  const first = result.data[0];
  if (!isRecord(first)) return null;
  return { id: textOrNull(first.id), status: textOrNull(first.status), items: first.items };
};

const listThreadParams = (cursor: string | null): JsonRecord => {
  const params: JsonRecord = {
    limit: LIST_PAGE_LIMIT,
    useStateDbOnly: true,
    sourceKinds: LIST_SOURCE_KINDS,
    sortKey: "updated_at",
    sortDirection: "desc",
  };
  if (cursor) params.cursor = cursor;
  return params;
};

const appendListedThreads = (threads: ListedThread[], result: unknown): void => {
  if (!isRecord(result) || !isUnknownArray(result.data)) return;
  for (const entry of result.data) {
    const thread = listedThreadOf(entry);
    if (thread) threads.push(thread);
  }
};

const listThreads = async (connection: SupervisorConnection, signal: AbortSignal): Promise<{ threads: ListedThread[]; truncated: boolean }> => {
  const threads: ListedThread[] = [];
  let cursor: string | null = null;
  let truncated = false;
  for (let page = 0; page < MAX_LIST_PAGES; page += 1) {
    const result = await connection.call("thread/list", listThreadParams(cursor), signal);
    appendListedThreads(threads, result);
    cursor = isRecord(result) ? textOrNull(result.nextCursor) : null;
    if (!cursor || threads.length >= INVENTORY_LIMIT) break;
  }
  if (cursor) truncated = true;
  if (threads.length > INVENTORY_LIMIT) {
    threads.length = INVENTORY_LIMIT;
    truncated = true;
  }
  return { threads, truncated };
};

const probeThread = async (connection: SupervisorConnection, threadId: string, signal: AbortSignal): Promise<ThreadProbe> => {
  let runtimeStatus: string | null = null;
  let activeFlags: string[] = [];
  let threadSummary: ListedThread | null = null;
  let sampled = true;
  try {
    const result = await connection.call("thread/read", { threadId, includeTurns: false }, signal);
    const thread = isRecord(result) && isRecord(result.thread) ? result.thread : null;
    threadSummary = thread ? listedThreadOf(thread) : null;
    const status = thread && isRecord(thread.status) ? thread.status : null;
    runtimeStatus = status ? asString(status.type) : null;
    if (status && isUnknownArray(status.activeFlags)) {
      activeFlags = status.activeFlags.filter((flag): flag is string => typeof flag === "string");
    }
  } catch {
    sampled = false;
  }
  let turnStatus: string | null = null;
  try {
    const result = await connection.call("thread/turns/list", { threadId, limit: 1, sortDirection: "desc", itemsView: "notLoaded" }, signal);
    const turn = newestTurnOf(result);
    turnStatus = turn ? turn.status : null;
  } catch {
    sampled = false;
  }
  return { runtimeStatus, activeFlags, turnStatus, thread: threadSummary, sampled };
};

/**
 * The single state decision. Runtime status from `thread/read` is authoritative:
 * recency is never treated as activity, and a failed read is `unknown`, never `idle`.
 */
export const classifySupervisorState = (input: { runtimeStatus: string | null; turnStatus: string | null; loaded: boolean }): SupervisorState => {
  const runtime = input.runtimeStatus;
  if (runtime === "active") return "active";
  if (runtime === "systemError") return "system_error";
  if (runtime === "idle") return "idle";
  if (runtime === "notLoaded") {
    if (input.turnStatus !== null && !TERMINAL_TURN_STATES.has(input.turnStatus)) return "stale";
    return input.turnStatus !== null && TERMINAL_TURN_STATES.has(input.turnStatus) ? "idle" : "unknown";
  }
  if (input.turnStatus !== null && !TERMINAL_TURN_STATES.has(input.turnStatus)) return input.loaded ? "active" : "stale";
  return "unknown";
};

const quotaBucketOf = (value: unknown, fallbackId: string): SupervisorQuotaBucket | null => {
  if (!isRecord(value)) return null;
  const primary = isRecord(value.primary) ? value.primary : null;
  return {
    limitId: textOrNull(value.limitId) ?? fallbackId,
    limitName: textOrNull(value.limitName),
    usedPercent: asFiniteNumber(value.usedPercent) ?? (primary ? asFiniteNumber(primary.usedPercent) : null),
    windowDurationMins: asFiniteNumber(value.windowDurationMins) ?? (primary ? asFiniteNumber(primary.windowDurationMins) : null),
    resetsAtMs: normalizeEpochMs(value.resetsAt) ?? (primary ? normalizeEpochMs(primary.resetsAt) : null),
    planType: textOrNull(value.planType),
    reachedType: textOrNull(value.rateLimitReachedType),
  };
};

const quotaBucketsOf = (value: unknown): SupervisorQuotaBucket[] => {
  if (!isRecord(value)) return [];
  const buckets: SupervisorQuotaBucket[] = [];
  for (const [key, entry] of Object.entries(value)) {
    const bucket = quotaBucketOf(entry, key);
    if (bucket) buckets.push(bucket);
  }
  return buckets;
};

/**
 * Projects the shared account quota from one `account/rateLimits/read` result.
 * One bucket per limit id; the same account-wide window is reported by every
 * machine signed in to the account, so callers must never sum buckets from
 * different sources.
 */
export const quotaFromRateLimits = (result: unknown): SupervisorQuota | null => {
  const root = isRecord(result) && isRecord(result.rateLimits) ? result.rateLimits : null;
  if (!root) return null;
  const buckets = quotaBucketsOf(isRecord(result) ? result.rateLimitsByLimitId : null);
  if (buckets.length === 0) {
    const fallback = quotaBucketOf(root, "codex");
    if (fallback) buckets.push(fallback);
  }
  const credits = isRecord(result) && isRecord(result.rateLimitResetCredits) ? asFiniteNumber(result.rateLimitResetCredits.availableCount) : null;
  if (buckets.length === 0) return null;
  return { accountScope: "shared_codex_account", buckets, resetCreditsAvailable: credits };
};

const readQuota = async (connection: SupervisorConnection, signal: AbortSignal): Promise<SupervisorQuota | null> => {
  try {
    return quotaFromRateLimits(await connection.call("account/rateLimits/read", {}, signal));
  } catch {
    return null;
  }
};

type SupervisorTitle = { title: string | null; titleSource: "thread" | "metadata" | null };

const supervisorTitleOf = (live: ListedThread | null, metadata: ThreadMetadata | null): SupervisorTitle => {
  const threadTitle = live ? (live.name ?? live.preview) : null;
  if (threadTitle) return { title: threadTitle, titleSource: "thread" };
  const metadataTitle = metadata ? (metadata.title ?? metadata.preview) : null;
  return metadataTitle ? { title: metadataTitle, titleSource: "metadata" } : { title: null, titleSource: null };
};

type SupervisorLiveFields = {
  cwd: string | null;
  branch: string | null;
  model: string | null;
  effort: string | null;
  provider: string | null;
  sourceKind: string | null;
  parentThreadId: string | null;
  lastActivityAtMs: number | null;
  tokensUsed: number | null;
  usageSource: "state_db" | null;
};

const supervisorLiveFieldsOf = (live: ListedThread | null, metadata: ThreadMetadata | null): SupervisorLiveFields => {
  const tokensUsed = metadata ? metadata.tokensUsed : null;
  return {
    cwd: live?.cwd ?? (metadata ? metadata.cwd : null),
    branch: metadata ? metadata.gitBranch : null,
    model: live?.model ?? (metadata ? metadata.model : null),
    effort: live?.effort ?? (metadata ? metadata.effort : null),
    provider: live?.modelProvider ?? null,
    sourceKind: live?.sourceKind ?? (metadata ? metadata.threadSource : null),
    parentThreadId: live?.parentThreadId ?? (metadata ? metadata.parentThreadId : null),
    lastActivityAtMs: (live ? live.updatedAtMs : null) ?? (metadata ? metadata.updatedAtMs : null),
    tokensUsed,
    usageSource: tokensUsed !== null ? "state_db" : null,
  };
};

const supervisorWaitsOf = (activeFlags: readonly string[]): { waitingOnApproval: boolean; waitingOnUserInput: boolean } => ({
  waitingOnApproval: activeFlags.some((flag) => /approval/i.test(flag)),
  waitingOnUserInput: activeFlags.some((flag) => /user.?input|input.?request|needsinput/i.test(flag)),
});

/**
 * Projects one row from the verified live thread fields first. A local state
 * database only enriches what the live thread omitted (and always owns the
 * persisted branch and token usage); it never replaces live values.
 */
export const sessionFromParts = (input: {
  id: string;
  source: SupervisorSource;
  metadata: ThreadMetadata | null;
  listed: ListedThread | null;
  probe: ThreadProbe | null;
  loaded: boolean;
  sampledAtMs: number;
  childIds: string[] | null;
  unavailable: string[];
}): SupervisorSession => {
  const { metadata, listed, probe } = input;
  const live = listed ?? (probe ? probe.thread : null);
  const title = supervisorTitleOf(live, metadata);
  const fields = supervisorLiveFieldsOf(live, metadata);
  const activeFlags = probe ? probe.activeFlags : [];
  const waits = supervisorWaitsOf(activeFlags);
  return {
    id: input.id,
    sourceId: input.source.id,
    machine: input.source.name,
    ...title,
    ...fields,
    state: probe ? classifySupervisorState({ runtimeStatus: probe.runtimeStatus, turnStatus: probe.turnStatus, loaded: input.loaded }) : "unknown",
    activeFlags,
    loaded: input.loaded,
    ...waits,
    childIds: input.childIds,
    lastSampledAtMs: input.sampledAtMs,
    sampled: probe?.sampled === true,
    unavailable: input.unavailable,
  };
};

type SourceSample = { view: SupervisorSourceView; sessions: SupervisorSession[] };

const unavailableSourceView = (
  source: SupervisorSource,
  reason: string,
  sampledAtMs: number,
  durationMs: number,
  metadata: { available: boolean; database: string | null; reason: string | null },
  inventory: { listed: number; sampled: number; returned: number; truncated: boolean },
  lineage: "available" | "unavailable"
): SupervisorSourceView => ({
  id: source.id,
  name: source.name,
  kind: source.codexHome ? "local" : "remote",
  hostname: source.codexHome ? readHostname() : null,
  state: "unavailable",
  reason,
  metadata,
  inventory,
  lineage,
  quota: null,
  quotaReason: "source unavailable",
  durationMs,
  sampledAtMs,
});

const remoteUnavailableView = (source: SupervisorSource, reason: string, sampledAtMs: number, durationMs: number): SupervisorSourceView =>
  unavailableSourceView(
    source,
    reason,
    sampledAtMs,
    durationMs,
    { available: false, database: null, reason: "remote source has no local metadata database" },
    { listed: 0, sampled: 0, returned: 0, truncated: false },
    "unavailable"
  );

const loadedThreadIdsOf = (raw: unknown): string[] =>
  isRecord(raw) && isUnknownArray(raw.data) ? raw.data.filter((value): value is string => typeof value === "string" && THREAD_ID_PATTERN.test(value)) : [];

const metadataForSource = async (
  source: SupervisorSource,
  ids: readonly string[]
): Promise<{ database: string | null; rows: Map<string, ThreadMetadata>; reason: string | null }> => {
  if (!source.codexHome) return { database: null, rows: new Map<string, ThreadMetadata>(), reason: "remote source has no local metadata database" };
  return await readThreadMetadata(source.codexHome, ids);
};

const probeIdsFor = (listedIds: readonly string[], loadedIds: readonly string[]): string[] => {
  const listedIdSet = new Set(listedIds);
  return uniqueStrings([...listedIds.slice(0, LIVE_PROBE_LIMIT), ...loadedIds.filter((id) => !listedIdSet.has(id)).slice(0, EXTRA_LOADED_LIMIT)]);
};

const childrenByParent = (
  rowIds: readonly string[],
  liveThreadOf: (threadId: string) => ListedThread | null,
  metadataRows: ReadonlyMap<string, ThreadMetadata>
): Map<string, string[]> => {
  const childrenOf = new Map<string, string[]>();
  for (const threadId of rowIds) {
    const parent = liveThreadOf(threadId)?.parentThreadId ?? metadataRows.get(threadId)?.parentThreadId ?? null;
    if (!parent) continue;
    const existing = childrenOf.get(parent);
    if (existing) existing.push(threadId);
    else childrenOf.set(parent, [threadId]);
  }
  return childrenOf;
};

const sessionUnavailableNotes = (source: SupervisorSource, sampled: boolean, metadataReason: string | null): string[] => {
  const notes: string[] = [];
  if (!sampled) notes.push("live status not sampled in this pass");
  if (source.codexHome === null) notes.push("token usage is not reported by the source");
  else if (metadataReason !== null) notes.push(`metadata unavailable: ${metadataReason}`);
  return notes;
};

const sampleSource = async (source: SupervisorSource, signal: AbortSignal): Promise<SourceSample> => {
  const startedAt = nowMs();
  const sampledAtMs = startedAt;
  let connection: SupervisorConnection | null = null;
  try {
    const client = await openSupervisorConnection(source.socketPath, signal);
    connection = client;
    const loadedIds = loadedThreadIdsOf(await client.call("thread/loaded/list", {}, signal));
    const loadedSet = new Set(loadedIds);
    const listing = await listThreads(client, signal);
    const listedIds = listing.threads.map((thread) => thread.id);
    const metadata = await metadataForSource(source, uniqueStrings([...listedIds, ...loadedIds.slice(0, EXTRA_LOADED_LIMIT)]));
    const probeIds = probeIdsFor(listedIds, loadedIds);
    const probes = await mapWithConcurrency(probeIds, PROBE_CONCURRENCY, (threadId) => probeThread(client, threadId, signal));
    const probeById = new Map<string, ThreadProbe>();
    probeIds.forEach((threadId, index) => probeById.set(threadId, probes[index]));

    const listedById = new Map<string, ListedThread>(listing.threads.map((thread) => [thread.id, thread]));
    const rowIds = uniqueStrings([...listedIds, ...probeIds]);
    const liveThreadOf = (threadId: string): ListedThread | null => listedById.get(threadId) ?? probeById.get(threadId)?.thread ?? null;
    const childrenOf = childrenByParent(rowIds, liveThreadOf, metadata.rows);
    const lineageAvailable =
      listing.threads.length > 0 || probes.some((probe) => probe.thread !== null) || (source.codexHome !== null && metadata.reason === null);
    const sessions = rowIds.map((threadId) => {
      const probe = probeById.get(threadId) ?? null;
      return sessionFromParts({
        id: threadId,
        source,
        metadata: metadata.rows.get(threadId) ?? null,
        listed: listedById.get(threadId) ?? null,
        probe,
        loaded: loadedSet.has(threadId),
        sampledAtMs,
        childIds: lineageAvailable ? (childrenOf.get(threadId) ?? []) : null,
        unavailable: sessionUnavailableNotes(source, probe !== null, metadata.reason),
      });
    });
    const quota = await readQuota(connection, signal);
    const view: SupervisorSourceView = {
      id: source.id,
      name: source.name,
      kind: source.codexHome ? "local" : "remote",
      hostname: source.codexHome ? readHostname() : null,
      state: "ok",
      reason: null,
      metadata: { available: metadata.reason === null, database: metadata.database, reason: metadata.reason },
      inventory: {
        listed: listing.threads.length,
        sampled: probeIds.length,
        returned: sessions.length,
        truncated: listing.truncated || probeIds.length < rowIds.length,
      },
      lineage: lineageAvailable ? "available" : "unavailable",
      quota,
      quotaReason: quota ? null : "account quota read unavailable",
      durationMs: nowMs() - startedAt,
      sampledAtMs,
    };
    return { view, sessions };
  } catch (error) {
    const reason = error instanceof Error ? error.message : "app-server read failed";
    if (source.codexHome === null) {
      return { view: remoteUnavailableView(source, reason, sampledAtMs, nowMs() - startedAt), sessions: [] };
    }
    const fallback = await readThreadMetadata(source.codexHome, null);
    const sessions = [...fallback.rows.entries()].map(([threadId, row]) =>
      sessionFromParts({
        id: threadId,
        source,
        metadata: row,
        listed: null,
        probe: null,
        loaded: false,
        sampledAtMs,
        childIds: null,
        unavailable: [`live status unavailable: ${reason}`, "persisted metadata shown; it may be stale"],
      })
    );
    const view = unavailableSourceView(
      source,
      reason,
      sampledAtMs,
      nowMs() - startedAt,
      { available: fallback.reason === null, database: fallback.database, reason: fallback.reason },
      { listed: fallback.rows.size, sampled: 0, returned: sessions.length, truncated: fallback.rows.size >= RECENT_METADATA_LIMIT },
      fallback.reason === null ? "available" : "unavailable"
    );
    return { view, sessions };
  } finally {
    if (connection) await connection.close();
  }
};

const STATE_ORDER: Record<SupervisorState, number> = { active: 0, stale: 1, idle: 2, unknown: 3, system_error: 4 };

const collectSupervisorSnapshot = async (): Promise<SupervisorSnapshot> => {
  const config = await resolveSupervisorConfig();
  const timeout = AbortSignal.timeout(OVERALL_DEADLINE_MS);
  const samples = await mapWithConcurrency(config.sources, SOURCE_CONCURRENCY, (source) => sampleSource(source, timeout));
  const sessions = samples.flatMap((sample) => sample.sessions);
  sessions.sort((left, right) => {
    const byState = STATE_ORDER[left.state] - STATE_ORDER[right.state];
    if (byState !== 0) return byState;
    return (right.lastActivityAtMs ?? 0) - (left.lastActivityAtMs ?? 0);
  });
  const counts = {
    total: sessions.length,
    active: sessions.filter((session) => session.state === "active").length,
    waiting: sessions.filter((session) => session.waitingOnApproval || session.waitingOnUserInput).length,
    idle: sessions.filter((session) => session.state === "idle").length,
    stale: sessions.filter((session) => session.state === "stale").length,
    unknown: sessions.filter((session) => session.state === "unknown").length,
    systemError: sessions.filter((session) => session.state === "system_error").length,
  };
  const listed = samples.reduce((total, sample) => total + sample.view.inventory.listed, 0);
  const sampled = samples.reduce((total, sample) => total + sample.view.inventory.sampled, 0);
  const notes = [...config.notes];
  const unavailable = samples.filter((sample) => sample.view.state === "unavailable");
  if (unavailable.length > 0)
    notes.push(`${unavailable.length} of ${samples.length} sources unavailable: ${unavailable.map((sample) => sample.view.id).join(", ")}`);
  if (samples.some((sample) => sample.view.inventory.truncated))
    notes.push("inventory was truncated; the panel lists the most recently updated threads per source");
  return {
    sampledAtMs: nowMs(),
    view: "codex_app_server_read_only",
    sources: samples.map((sample) => sample.view),
    sessions,
    counts,
    coverage: { listed, sampled, truncated: samples.some((sample) => sample.view.inventory.truncated), notes },
  };
};

let snapshotCache: { at: number; value: SupervisorSnapshot } | null = null;
let snapshotInFlight: Promise<SupervisorSnapshot> | null = null;

/** Single-flight, short-lived cache so several UI polls share one sampling round. */
export const ensureSupervisorSnapshot = async (): Promise<SupervisorSnapshot> => {
  const now = nowMs();
  if (snapshotCache && now - snapshotCache.at < SNAPSHOT_TTL_MS) return snapshotCache.value;
  if (snapshotInFlight) return await snapshotInFlight;
  snapshotInFlight = collectSupervisorSnapshot()
    .then((value) => {
      snapshotCache = { at: nowMs(), value };
      return value;
    })
    .finally(() => {
      snapshotInFlight = null;
    });
  return await snapshotInFlight;
};

/* ------------------------------------------------------------ live output */

type SupervisorFollowEntry = {
  key: string;
  kind: "message" | "command";
  turnId: string;
  index: number;
  text: string;
  command: string | null;
  status: string | null;
  exitCode: number | null;
  atMs: number | null;
};

/**
 * Extracts only what a super admin may see: assistant messages and command
 * output. Reasoning, system, developer, user, and tool-internal items are never
 * converted into panel entries.
 */
export const extractFollowEntries = (turnId: string, items: unknown): SupervisorFollowEntry[] => {
  if (!isUnknownArray(items)) return [];
  const entries: SupervisorFollowEntry[] = [];
  for (let index = 0; index < items.length; index += 1) {
    const item = items[index];
    if (!isRecord(item)) continue;
    const type = asString(item.type);
    const id = textOrNull(item.id) ?? String(index);
    if (type === "agentMessage") {
      entries.push({
        key: `${turnId}:${id}`,
        kind: "message",
        turnId,
        index,
        text: asString(item.text) ?? "",
        command: null,
        status: textOrNull(item.phase),
        exitCode: null,
        atMs: null,
      });
      continue;
    }
    if (type === "commandExecution") {
      entries.push({
        key: `${turnId}:${id}`,
        kind: "command",
        turnId,
        index,
        text: asString(item.aggregatedOutput) ?? "",
        command: asString(item.command) ?? "",
        status: textOrNull(item.status),
        exitCode: asFiniteNumber(item.exitCode),
        atMs: null,
      });
    }
  }
  return entries;
};

export const parseFollowCursor = (cursor: string): { turnId: string; index: number } | null => {
  const separator = cursor.lastIndexOf(":");
  if (separator <= 0) return null;
  const index = Number(cursor.slice(separator + 1));
  if (!Number.isSafeInteger(index) || index < -1) return null;
  return { turnId: cursor.slice(0, separator), index };
};

const followEntryPayload = (entry: SupervisorFollowEntry): JsonRecord => ({
  key: entry.key,
  kind: entry.kind,
  turnId: entry.turnId,
  text: boundedText(entry.text, FOLLOW_MAX_TEXT_CHARS),
  command: entry.command === null ? null : boundedText(entry.command, FOLLOW_MAX_COMMAND_CHARS),
  status: entry.status,
  exitCode: entry.exitCode,
  atMs: entry.atMs,
});

type FollowUpdate = { entries: SupervisorFollowEntry[]; cursor: string; turnId: string | null; turnStatus: string | null; enriched: boolean };

const readFollowUpdate = async (source: SupervisorSource, threadId: string, cursor: string, signal: AbortSignal): Promise<FollowUpdate> => {
  const connection = await openSupervisorConnection(source.socketPath, signal);
  try {
    const summary = await connection.call("thread/turns/list", { threadId, limit: 1, sortDirection: "desc", itemsView: "summary" }, signal);
    const turn = newestTurnOf(summary);
    if (!turn?.id) return { entries: [], cursor, turnId: null, turnStatus: null, enriched: false };
    const previous = parseFollowCursor(cursor);
    const startIndex = previous?.turnId === turn.id ? previous.index : -1;
    let items = turn.items;
    let enriched = false;
    let entries = extractFollowEntries(turn.id, items).filter((entry) => entry.index > startIndex);
    const missingContent = entries.some((entry) => entry.text.length === 0);
    if (missingContent) {
      try {
        const full = await connection.call("thread/turns/list", { threadId, limit: 1, sortDirection: "desc", itemsView: "full" }, signal);
        const fullTurn = newestTurnOf(full);
        if (fullTurn?.id === turn.id) {
          items = fullTurn.items;
          enriched = true;
          entries = extractFollowEntries(turn.id, items).filter((entry) => entry.index > startIndex);
        }
      } catch {
        // Summary entries are still honest; the UI keeps them as recorded output.
      }
    }
    const emitted = entries.filter((entry) => entry.kind === "command" || entry.text.length > 0).slice(-FOLLOW_MAX_ENTRIES);
    const lastIndex = emitted.length > 0 ? emitted[emitted.length - 1].index : startIndex;
    return { entries: emitted, cursor: `${turn.id}:${lastIndex}`, turnId: turn.id, turnStatus: turn.status, enriched };
  } finally {
    await connection.close();
  }
};

/* ------------------------------------------------------------------ routes */

const supervisorError = (status: number, message: string): Response => openaiError(status, message, "invalid_request_error");

export const handleAdminCodexSupervisorSessions = async (): Promise<Response> => {
  const snapshot = await ensureSupervisorSnapshot();
  return json(200, snapshot, { "Cache-Control": "no-store" });
};

const ENCODER = new TextEncoder();

/**
 * SSE follow for one selected session. It polls the newest persisted turn every
 * two seconds and appends newly recorded assistant messages and command output.
 * It is explicitly not a token-level stream, and reports `unavailable` instead
 * of inventing output when the source cannot be read.
 */
export const handleAdminCodexSupervisorOutput = async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const sourceId = (url.searchParams.get("source") ?? "").trim();
  const threadId = (url.searchParams.get("id") ?? "").trim();
  const initialCursor = (url.searchParams.get("cursor") ?? "").trim().slice(0, 240);
  if (!SOURCE_ID_PATTERN.test(sourceId)) return supervisorError(400, "Invalid supervisor source id");
  if (!THREAD_ID_PATTERN.test(threadId)) return supervisorError(400, "Invalid thread id");
  const config = await resolveSupervisorConfig();
  const source = config.sources.find((entry) => entry.id === sourceId);
  if (!source) return supervisorError(400, "Unknown supervisor source");
  const snapshot = await ensureSupervisorSnapshot();
  const known = snapshot.sessions.some((session) => session.sourceId === sourceId && session.id === threadId);
  if (!known) return supervisorError(409, "Session is not in the sampled inventory for this source; refresh the panel and select a listed session");
  const sourceView = snapshot.sources.find((entry) => entry.id === sourceId);
  if (sourceView?.state === "unavailable") return supervisorError(409, `Source ${sourceId} is unavailable: ${sourceView.reason ?? "unknown reason"}`);

  let closed = false;
  let polls = 0;
  let lastUnavailable = "";
  let unavailablePolls = 0;
  let cursor = initialCursor;
  const startedAt = nowMs();
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      const send = (event: string, data: unknown): void => {
        if (closed) return;
        try {
          controller.enqueue(ENCODER.encode(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`));
        } catch {
          closed = true;
        }
      };
      const stop = (): void => {
        if (closed) return;
        closed = true;
        try {
          controller.close();
        } catch {
          // The client may already have gone away.
        }
      };
      req.signal.addEventListener("abort", stop, { once: true });
      void (async () => {
        send("ready", {
          sourceId,
          threadId,
          machine: source.name,
          mode: "recorded",
          note: "Updates as Codex records output (about every 2s), not a token-level stream. Only assistant messages and command output are shown.",
        });
        while (!closed && !req.signal.aborted && nowMs() - startedAt < FOLLOW_MAX_DURATION_MS) {
          try {
            const update = await readFollowUpdate(source, threadId, cursor, req.signal);
            cursor = update.cursor;
            polls += 1;
            if (update.entries.length > 0) {
              send("entries", {
                entries: update.entries.map(followEntryPayload),
                cursor: update.cursor,
                turnId: update.turnId,
                turnStatus: update.turnStatus,
                enriched: update.enriched,
                sampledAtMs: nowMs(),
              });
            } else if (polls % FOLLOW_HEARTBEAT_EVERY === 0) {
              send("heartbeat", { turnId: update.turnId, turnStatus: update.turnStatus, sampledAtMs: nowMs() });
            }
          } catch (error) {
            unavailablePolls += 1;
            const message = error instanceof Error ? boundedText(error.message, 200) : "output read failed";
            if (message !== lastUnavailable || unavailablePolls % FOLLOW_HEARTBEAT_EVERY === 0) {
              lastUnavailable = message;
              send("unavailable", { message, sampledAtMs: nowMs() });
            }
          }
          await waitForPoll(FOLLOW_POLL_MS, req.signal);
        }
        send("end", { reason: req.signal.aborted ? "client closed" : "follow duration limit reached" });
        stop();
      })();
    },
    cancel() {
      closed = true;
    },
  });
  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-store",
      "X-Accel-Buffering": "no",
    },
  });
};
