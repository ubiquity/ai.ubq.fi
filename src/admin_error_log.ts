import { getKv } from "./kv.ts";
import { json, openaiError } from "./http.ts";

export const ADMIN_ERROR_LOG_PREFIX = ["uos_ai", "admin_error_log", "v1"] as const;
export const ADMIN_ERROR_LOG_TTL_MS = 7 * 24 * 60 * 60_000;
const DEFAULT_LIMIT = 100;
const MAX_LIMIT = 500;

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

export const listAdminErrors = async (
  limit = DEFAULT_LIMIT,
  kvOverride?: Deno.Kv | null,
): Promise<AdminErrorLogRecord[]> => {
  const kv = kvOverride === undefined ? await getKv() : kvOverride;
  if (!kv) return [];
  const boundedLimit = Math.max(1, Math.min(MAX_LIMIT, Math.trunc(limit)));
  const records: AdminErrorLogRecord[] = [];
  for await (
    const entry of kv.list<AdminErrorLogRecord>({ prefix: ADMIN_ERROR_LOG_PREFIX }, {
      reverse: true,
      limit: boundedLimit,
    })
  ) {
    if (isAdminErrorLogRecord(entry.value)) records.push(entry.value);
  }
  return records;
};

export const handleAdminErrors = async (req: Request): Promise<Response> => {
  const rawLimit = new URL(req.url).searchParams.get("limit");
  const limit = rawLimit === null ? DEFAULT_LIMIT : Number(rawLimit);
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > MAX_LIMIT) {
    return openaiError(400, `limit must be an integer from 1 to ${MAX_LIMIT}`, "invalid_request_error");
  }
  const kv = await getKv();
  if (!kv) return openaiError(503, "Error history storage is unavailable", "server_error");
  return json(200, { object: "list", data: await listAdminErrors(limit, kv) });
};
