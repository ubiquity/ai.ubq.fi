import { json, openaiError } from "./http.ts";
import { getKv } from "./kv.ts";
import { listEncryptedSentinelReplays, SENTINEL_REPLAY_EXPORT_PAGE_LIMIT } from "./sentinel_replay_capture.ts";

const nonNegativeInteger = (value: string | null, fallback: number): number | null => {
  if (value === null || value === "") return fallback;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : null;
};

const validCursor = (value: string | null): boolean =>
  value === null || value === "" || (value.length <= 2_048 && /^[A-Za-z0-9_-]+$/.test(value));

type SentinelReplayAdminDependencies = Readonly<{
  getKv?: typeof getKv;
  listEncryptedSentinelReplays?: typeof listEncryptedSentinelReplays;
}>;

export const handleAdminSentinelReplayCaptures = async (
  req: Request,
  dependencies: SentinelReplayAdminDependencies = {},
): Promise<Response> => {
  const url = new URL(req.url);
  const afterMs = nonNegativeInteger(url.searchParams.get("after_ms"), 0);
  const beforeMs = nonNegativeInteger(url.searchParams.get("before_ms"), -1);
  const limit = nonNegativeInteger(url.searchParams.get("limit"), SENTINEL_REPLAY_EXPORT_PAGE_LIMIT);
  const cursor = url.searchParams.get("cursor");
  if (
    afterMs === null || beforeMs === null || beforeMs < afterMs ||
    limit !== SENTINEL_REPLAY_EXPORT_PAGE_LIMIT || !validCursor(cursor)
  ) {
    return openaiError(
      400,
      "after_ms and before_ms must define a valid interval, limit must be one, and cursor must be valid",
      "invalid_request_error",
    );
  }
  try {
    const kv = await (dependencies.getKv ?? getKv)();
    if (!kv) {
      return openaiError(503, "Sentinel replay storage is unavailable", "sentinel_replay_storage_unavailable");
    }
    const page = await (dependencies.listEncryptedSentinelReplays ?? listEncryptedSentinelReplays)(kv, {
      afterMs,
      beforeMs,
      limit,
      cursor: cursor || undefined,
    });
    return json(200, {
      data: page.captures,
      cursor: page.cursor || null,
    }, { "Cache-Control": "no-store" });
  } catch {
    return openaiError(503, "Sentinel replay export failed", "sentinel_replay_export_failed");
  }
};
