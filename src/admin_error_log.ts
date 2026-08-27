import { getKv } from "./kv.ts";
import { json, openaiError } from "./http.ts";

export const ADMIN_ERROR_LOG_PREFIX = ["uos_ai", "admin_error_log", "v1"] as const;
export const ADMIN_ERROR_LOG_TTL_MS = 7 * 24 * 60 * 60_000;
const DEFAULT_LIMIT = 100;
const MAX_LIMIT = 500;
export const ADMIN_ERROR_BUCKET_MS = 15 * 60_000;

export type AdminErrorLogRecord = Readonly<{
  version: 1;
  request_id: string;
  route: string;
  status: number;
  provider: string;
  model: string | null;
  reasoning: string | null;
  stream: boolean | null;
  terminal_type: string;
  failure_kind: string;
  delivery_outcome: "delivered" | "interrupted" | "unobserved";
  created_at_ms: number;
  latency_ms: number;
  git_sha: string | null;
  deno_revision: string | null;
}>;

const bounded = (value: string | null | undefined, fallback: string, maximum = 160): string => {
  const normalized = value?.trim();
  return normalized ? normalized.slice(0, maximum) : fallback;
};

export const recordAdminError = async (
  input:
    & Omit<AdminErrorLogRecord, "version" | "terminal_type" | "failure_kind">
    & Readonly<{ terminal_type: string | null; failure_kind: string | null }>,
  kvOverride?: Deno.Kv | null,
): Promise<void> => {
  // Authentication failures are not inference failures and must retain the
  // request path's zero-KV budget.
  if (input.status === 401 && input.provider === "gateway") return;
  const failed = input.status >= 400 || input.terminal_type === "error" || input.terminal_type === "eof" ||
    input.terminal_type === "deadline" || input.terminal_type === "response.failed" ||
    input.terminal_type === "response.incomplete" || input.failure_kind !== null;
  if (!failed) return;

  const kv = kvOverride === undefined ? await getKv() : kvOverride;
  if (!kv) return;
  const terminalType = bounded(input.terminal_type, input.status >= 400 ? "http.error" : "error");
  const failureKind = bounded(input.failure_kind, input.status >= 400 ? `http_${input.status}` : terminalType);
  const record: AdminErrorLogRecord = {
    ...input,
    version: 1,
    terminal_type: terminalType,
    failure_kind: failureKind,
  };
  await kv.set([...ADMIN_ERROR_LOG_PREFIX, input.created_at_ms, input.request_id], record, {
    expireIn: ADMIN_ERROR_LOG_TTL_MS,
  });
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

export const isAdminErrorLogRecord = (value: unknown): value is AdminErrorLogRecord => {
  if (!isRecord(value)) return false;
  return value.version === 1 && typeof value.request_id === "string" && typeof value.route === "string" &&
    typeof value.status === "number" && typeof value.provider === "string" &&
    (value.model === null || typeof value.model === "string") &&
    (value.reasoning === null || typeof value.reasoning === "string") &&
    (value.stream === null || typeof value.stream === "boolean") && typeof value.terminal_type === "string" &&
    typeof value.failure_kind === "string" &&
    (value.delivery_outcome === "delivered" || value.delivery_outcome === "interrupted" ||
      value.delivery_outcome === "unobserved") &&
    typeof value.created_at_ms === "number" &&
    typeof value.latency_ms === "number" && (value.git_sha === null || typeof value.git_sha === "string") &&
    (value.deno_revision === null || typeof value.deno_revision === "string");
};

// These deployments let partial-body framing override an authoritative
// downstream cancellation. The retained rows cannot otherwise be separated
// from genuine premature EOFs, so limit suppression to the affected code.
const LEGACY_INTERRUPTED_MISSING_SSE_TERMINAL_GIT_SHAS = new Set([
  "4d2928f47f3ba3ad66b8c15e6323bcccd0a39390",
  "0d795e28e42be63bbd7f0d4ce44d8ea0f6ab9d4a",
  "149ef399a52373ba09d75cfd00d5e5139564dcdf",
  "8532ec7ee1fc5a6a9bb44fe2a3703527b8d78952",
  "35fb0782c01309a85ea08f5a6a48f8de62a1f29f",
  "c18a4e091c09c7a9e04588a8fe745c696deaf6d2",
  "214957fe322c8cff6ad20ddea55dcf9576273107",
  "50c06cd8e93fe3b34a9699d31f1f8998f632fbe6",
  "64f4ba5a21e476386f95c8474bd76b037e6afc4f",
  "7fb508a076e9edf04c6e4087a19ad6e25452dc6b",
  "c9bcf7d5937cf137bc34d8656187d3f20e9f8b4d",
  "9d3b7d0eb8a1c57079d298b17d50d25f74dd39d9",
  "542df9ef4560ca9c090925a5a1b24a141a584867",
  "ce37210d58746a2ed3aec34c38b370f7060639e1",
  "f9dec6c3b6270813be7b4a957221f7130c37a52b",
]);

const isLegacyInterruptedMissingSseTerminal = (record: AdminErrorLogRecord): boolean =>
  record.status === 200 && record.stream === true && record.terminal_type === "error" &&
  record.failure_kind === "missing_sse_terminal" && record.delivery_outcome === "interrupted" &&
  record.git_sha !== null && LEGACY_INTERRUPTED_MISSING_SSE_TERMINAL_GIT_SHAS.has(record.git_sha);

export type AdminErrorHistory = Readonly<{
  data: AdminErrorLogRecord[];
  five_xx_buckets: Array<{ bucket_start_at_ms: number; count: number }>;
}>;

export const listAdminErrorHistory = async (
  limit = DEFAULT_LIMIT,
  kvOverride?: Deno.Kv | null,
): Promise<AdminErrorHistory> => {
  const kv = kvOverride === undefined ? await getKv() : kvOverride;
  if (!kv) return { data: [], five_xx_buckets: [] };
  const boundedLimit = Math.max(1, Math.min(MAX_LIMIT, Math.trunc(limit)));
  const data: AdminErrorLogRecord[] = [];
  const fiveXxCounts = new Map<number, number>();
  for await (
    const entry of kv.list<AdminErrorLogRecord>({ prefix: ADMIN_ERROR_LOG_PREFIX }, {
      reverse: true,
      batchSize: MAX_LIMIT,
    })
  ) {
    if (!isAdminErrorLogRecord(entry.value)) continue;
    const record = entry.value;
    if (record.status >= 500 && record.status <= 599) {
      const bucketStartAtMs = Math.floor(record.created_at_ms / ADMIN_ERROR_BUCKET_MS) * ADMIN_ERROR_BUCKET_MS;
      fiveXxCounts.set(bucketStartAtMs, (fiveXxCounts.get(bucketStartAtMs) ?? 0) + 1);
    }
    if (data.length < boundedLimit && !isLegacyInterruptedMissingSseTerminal(record)) data.push(record);
  }
  return {
    data,
    five_xx_buckets: [...fiveXxCounts.entries()]
      .sort(([left], [right]) => left - right)
      .map(([bucket_start_at_ms, count]) => ({ bucket_start_at_ms, count })),
  };
};

export const handleAdminErrors = async (req: Request): Promise<Response> => {
  const rawLimit = new URL(req.url).searchParams.get("limit");
  const limit = rawLimit === null ? DEFAULT_LIMIT : Number(rawLimit);
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > MAX_LIMIT) {
    return openaiError(400, `limit must be an integer from 1 to ${MAX_LIMIT}`, "invalid_request_error");
  }
  const kv = await getKv();
  if (!kv) return openaiError(503, "Error history storage is unavailable", "server_error");
  return json(200, { object: "list", ...await listAdminErrorHistory(limit, kv) });
};
