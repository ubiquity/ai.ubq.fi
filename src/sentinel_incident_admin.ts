import { json, openaiError } from "./http.ts";
import { getKv } from "./kv.ts";
import {
  isSentinelIncidentId,
  listSentinelIncidentIndexRows,
  SENTINEL_INCIDENT_INDEX_DEFAULT_PAGE_LIMIT,
  SENTINEL_INCIDENT_INDEX_MAX_PAGE_LIMIT,
  type SentinelIncidentIndexRow,
} from "./sentinel_incident_outbox.ts";

const INDEX_CURSOR = /^[A-Za-z0-9_-]+={0,2}$/;

const validPageLimit = (value: string | null): number | null => {
  if (value === null) return SENTINEL_INCIDENT_INDEX_DEFAULT_PAGE_LIMIT;
  if (!/^[0-9]+$/.test(value)) return null;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= 1 && parsed <= SENTINEL_INCIDENT_INDEX_MAX_PAGE_LIMIT
    ? parsed
    : null;
};

const validCursor = (value: string | null): boolean =>
  value === null || (value.length > 0 && value.length <= 2_048 && INDEX_CURSOR.test(value));

type SentinelIncidentAdminDependencies = Readonly<{
  getKv?: typeof getKv;
  listSentinelIncidentIndexRows?: typeof listSentinelIncidentIndexRows;
}>;

/** Exact snake_case wire row; internal keys/metadata are never exposed. */
const rowToWire = (row: SentinelIncidentIndexRow): Record<string, unknown> => ({
  incident_id: row.incident_id,
  fingerprint: row.fingerprint,
  severity: row.severity,
  first_seen_at_ms: row.first_seen_at_ms,
  last_seen_at_ms: row.last_seen_at_ms,
  count: row.count,
  failing_revision: row.failing_revision,
  error_type: row.error_type,
  context: {
    message: row.context.message,
    location: row.context.location,
    sample: [...row.context.sample],
  },
  provenance: {
    endpoint: row.provenance.endpoint,
    captured_at_ms: row.provenance.captured_at_ms,
    captured_by: row.provenance.captured_by,
  },
  evidence_ref: row.evidence_ref === null ? null : { ref: row.evidence_ref.ref, digest: row.evidence_ref.digest },
  evidence_expires_at_ms: row.evidence_expires_at_ms,
});

export const handleAdminSentinelIncidents = async (
  req: Request,
  dependencies: SentinelIncidentAdminDependencies = {},
): Promise<Response> => {
  const url = new URL(req.url);
  const limit = validPageLimit(url.searchParams.get("limit"));
  const cursor = url.searchParams.get("cursor");
  const rawIncidentId = url.searchParams.get("incident_id");
  const incidentId = rawIncidentId;
  // Frozen single-value semantics: a repeated query key is never resolved by
  // arbitrary first-value coercion; explicit empty values are malformed, not
  // silently defaulted or trimmed (optional means the key is absent).
  const unknownKeys = [...url.searchParams.keys()].filter(
    (key) => key !== "limit" && key !== "cursor" && key !== "incident_id",
  );
  const duplicatedKeys = [...url.searchParams.keys()].some(
    (key) => url.searchParams.getAll(key).length !== 1,
  );
  if (
    limit === null || !validCursor(cursor) || unknownKeys.length > 0 || duplicatedKeys ||
    (incidentId !== null && !isSentinelIncidentId(incidentId))
  ) {
    return openaiError(
      400,
      "limit must be an integer from 1 to 100, cursor, and incident_id must be valid, and unknown query keys are rejected",
      "invalid_request_error",
    );
  }
  try {
    const kv = await (dependencies.getKv ?? getKv)();
    if (!kv) {
      return openaiError(503, "Sentinel incident index is unavailable", "sentinel_incidents_unavailable");
    }
    const page = await (dependencies.listSentinelIncidentIndexRows ?? listSentinelIncidentIndexRows)(kv, {
      incidentId: incidentId ?? undefined,
      limit,
      cursor: cursor || undefined,
    });
    return json(200, {
      data: page.rows.map(rowToWire),
      cursor: page.cursor || null,
      // Every successful page read is complete; incomplete is reserved for a
      // genuine source gap and never for ordinary pagination continuation.
      coverage: { status: "complete" },
    }, { "Cache-Control": "no-store" });
  } catch {
    return openaiError(503, "Sentinel incident index is unavailable", "sentinel_incidents_unavailable");
  }
};
