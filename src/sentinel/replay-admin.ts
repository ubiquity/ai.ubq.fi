import { json, openaiError } from "../http.ts";
import { getKv } from "../kv.ts";
import { isSentinelIncidentId } from "./incident-outbox.ts";
import {
  isSentinelReplayRequestId,
  listEncryptedSentinelIncidentReplays,
  listEncryptedSentinelReplays,
  listEncryptedSentinelReplaysByRequestId,
  SENTINEL_REPLAY_EXPORT_PAGE_LIMIT,
} from "./replay-capture.ts";

const nonNegativeInteger = (value: string | null, fallback: number): number | null => {
  if (value === null || value === "") return fallback;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : null;
};

const validCursor = (value: string | null): boolean => value === null || value === "" || (value.length <= 2_048 && /^[A-Za-z0-9_-]+={0,2}$/.test(value));

type SentinelReplayAdminDependencies = Readonly<{
  getKv?: typeof getKv;
  listEncryptedSentinelReplays?: typeof listEncryptedSentinelReplays;
  listEncryptedSentinelIncidentReplays?: typeof listEncryptedSentinelIncidentReplays;
  listEncryptedSentinelReplaysByRequestId?: typeof listEncryptedSentinelReplaysByRequestId;
}>;

/** One shared invalid-input response for every replay-capture lookup mode. */
const invalidReplayExportRequest = (): Response =>
  openaiError(
    400,
    "after_ms and before_ms must define a valid interval, limit must be one, cursor must be valid, and at most one of incident_id or request_id may be selected",
    "invalid_request_error"
  );

export const handleAdminSentinelReplayCaptures = async (req: Request, dependencies: SentinelReplayAdminDependencies = {}): Promise<Response> => {
  const url = new URL(req.url);
  // A missing parameter and one that trims to "" both mean "absent" here: `??`
  // would forward the empty value, so the emptiness is spelled out.
  const trimmedIncidentId = url.searchParams.get("incident_id")?.trim() ?? "";
  const incidentId = trimmedIncidentId === "" ? null : trimmedIncidentId;
  const trimmedRequestId = url.searchParams.get("request_id")?.trim() ?? "";
  const requestId = trimmedRequestId === "" ? null : trimmedRequestId;
  let afterMs: number;
  let beforeMs: number;
  let limit: number;
  let cursor: string | null;
  if (requestId !== null) {
    // A request-id lookup is a point lookup: it neither needs nor consumes a
    // time interval, so its unused listing parameters are fixed to safe values.
    // The request-id shape and the mutually exclusive mode selection are still
    // validated, and every listing mode parses its parameters exactly as before.
    if (!isSentinelReplayRequestId(requestId) || incidentId !== null) return invalidReplayExportRequest();
    afterMs = 0;
    beforeMs = 0;
    limit = SENTINEL_REPLAY_EXPORT_PAGE_LIMIT;
    cursor = null;
  } else {
    const parsedAfterMs = nonNegativeInteger(url.searchParams.get("after_ms"), 0);
    const parsedBeforeMs = nonNegativeInteger(url.searchParams.get("before_ms"), -1);
    const parsedLimit = nonNegativeInteger(url.searchParams.get("limit"), SENTINEL_REPLAY_EXPORT_PAGE_LIMIT);
    const parsedCursor = url.searchParams.get("cursor");
    if (
      parsedAfterMs === null ||
      parsedBeforeMs === null ||
      parsedBeforeMs < parsedAfterMs ||
      parsedLimit !== SENTINEL_REPLAY_EXPORT_PAGE_LIMIT ||
      !validCursor(parsedCursor) ||
      (incidentId !== null && !isSentinelIncidentId(incidentId))
    ) {
      return invalidReplayExportRequest();
    }
    afterMs = parsedAfterMs;
    beforeMs = parsedBeforeMs;
    limit = parsedLimit;
    cursor = parsedCursor;
  }
  // An empty cursor is absent as well, so it must never be forwarded upstream.
  const requestCursor = cursor === null || cursor === "" ? undefined : cursor;
  try {
    const kv = await (dependencies.getKv ?? getKv)();
    if (!kv) {
      return openaiError(503, "Sentinel replay storage is unavailable", "sentinel_replay_storage_unavailable");
    }
    // A request-id lookup always answers with the capture status for that
    // request, so a disabled, failed or expired capture is distinguishable
    // from a request that never produced one.
    if (requestId) {
      const result = await (dependencies.listEncryptedSentinelReplaysByRequestId ?? listEncryptedSentinelReplaysByRequestId)(kv, requestId);
      return json(
        200,
        {
          data: result.captures,
          cursor: null,
          capture_status: result.status,
        },
        { "Cache-Control": "no-store" }
      );
    }
    const page = incidentId
      ? await (dependencies.listEncryptedSentinelIncidentReplays ?? listEncryptedSentinelIncidentReplays)(kv, {
          incidentId,
          limit,
          cursor: requestCursor,
        })
      : await (dependencies.listEncryptedSentinelReplays ?? listEncryptedSentinelReplays)(kv, {
          afterMs,
          beforeMs,
          limit,
          cursor: requestCursor,
        });
    return json(
      200,
      {
        data: page.captures,
        cursor: page.cursor || null,
      },
      { "Cache-Control": "no-store" }
    );
  } catch {
    return openaiError(503, "Sentinel replay export failed", "sentinel_replay_export_failed");
  }
};
