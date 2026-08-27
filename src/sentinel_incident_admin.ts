import { openaiError } from "./http.ts";
import { getKv } from "./kv.ts";
import {
  acknowledgeSentinelIncident,
  claimSentinelIncidentWorkflowRun,
  deferSentinelIncident,
  isSentinelIncidentDeferralReason,
  isSentinelIncidentId,
  isSentinelProductionRuntime,
  SentinelIncidentAckConflict,
  SentinelIncidentClaimConflict,
  SentinelIncidentDeferConflict,
  type SentinelIncidentDeferralReason,
} from "./sentinel_incident_outbox.ts";
import { isRecord } from "./utils.ts";

const MAX_WORKFLOW_IDENTITY_BODY_BYTES = 4 * 1_024;
type WorkflowIdentity = Readonly<{
  incidentId: string;
  attempt: number;
  workflowRunId: number;
  ackNonce: string;
}>;

type WorkflowDeferral = WorkflowIdentity & Readonly<{ reason: SentinelIncidentDeferralReason }>;

const readWorkflowPayload = async (req: Request): Promise<unknown | Response> => {
  const declaredLength = Number(req.headers.get("content-length") ?? 0);
  if (Number.isFinite(declaredLength) && declaredLength > MAX_WORKFLOW_IDENTITY_BODY_BYTES) {
    return openaiError(413, "Sentinel incident workflow identity is too large", "invalid_request_error");
  }
  try {
    const bytes = new Uint8Array(await req.arrayBuffer());
    if (bytes.byteLength > MAX_WORKFLOW_IDENTITY_BODY_BYTES) {
      return openaiError(413, "Sentinel incident workflow identity is too large", "invalid_request_error");
    }
    const parsed: unknown = JSON.parse(new TextDecoder().decode(bytes));
    bytes.fill(0);
    return parsed;
  } catch {
    return openaiError(400, "Sentinel incident workflow identity is invalid", "invalid_request_error");
  }
};

const parseWorkflowIdentity = (parsed: Record<string, unknown>): WorkflowIdentity | Response => {
  const incidentId = parsed.incident_id;
  const attempt = parsed.attempt;
  const workflowRunId = parsed.workflow_run_id;
  const ackNonce = parsed.ack_nonce;
  if (
    !isSentinelIncidentId(incidentId) || typeof attempt !== "number" || !Number.isSafeInteger(attempt) ||
    attempt <= 0 ||
    typeof workflowRunId !== "number" || !Number.isSafeInteger(workflowRunId) || workflowRunId <= 0 ||
    typeof ackNonce !== "string" || !/^[A-Za-z0-9_-]{43}$/.test(ackNonce)
  ) return openaiError(400, "Sentinel incident workflow identity is invalid", "invalid_request_error");
  return { incidentId, attempt, workflowRunId, ackNonce };
};

const readWorkflowIdentity = async (req: Request): Promise<WorkflowIdentity | Response> => {
  const parsed = await readWorkflowPayload(req);
  if (parsed instanceof Response) return parsed;
  if (!isRecord(parsed) || Object.keys(parsed).sort().join(",") !== "ack_nonce,attempt,incident_id,workflow_run_id") {
    return openaiError(400, "Sentinel incident workflow identity is invalid", "invalid_request_error");
  }
  return parseWorkflowIdentity(parsed);
};

const readWorkflowDeferral = async (req: Request): Promise<WorkflowDeferral | Response> => {
  const parsed = await readWorkflowPayload(req);
  if (parsed instanceof Response) return parsed;
  if (
    !isRecord(parsed) ||
    Object.keys(parsed).sort().join(",") !== "ack_nonce,attempt,incident_id,reason,workflow_run_id" ||
    !isSentinelIncidentDeferralReason(parsed.reason)
  ) return openaiError(400, "Sentinel incident workflow deferral is invalid", "invalid_request_error");
  const identity = parseWorkflowIdentity(parsed);
  return identity instanceof Response ? identity : { ...identity, reason: parsed.reason };
};

export const handleAdminSentinelIncidentClaim = async (
  req: Request,
  dependencies: Readonly<{
    getKv?: typeof getKv;
    claim?: typeof claimSentinelIncidentWorkflowRun;
    isProduction?: typeof isSentinelProductionRuntime;
  }> = {},
): Promise<Response> => {
  if (!(dependencies.isProduction ?? isSentinelProductionRuntime)()) {
    return openaiError(404, "Not found", "not_found");
  }
  const identity = await readWorkflowIdentity(req);
  if (identity instanceof Response) return identity;
  try {
    const kv = await (dependencies.getKv ?? getKv)();
    if (!kv) return openaiError(503, "Sentinel incident storage is unavailable", "sentinel_incident_unavailable");
    await (dependencies.claim ?? claimSentinelIncidentWorkflowRun)(kv, identity);
    return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
  } catch (error) {
    if (error instanceof SentinelIncidentClaimConflict) {
      return openaiError(409, "Sentinel incident workflow claim is stale", "sentinel_incident_claim_conflict");
    }
    return openaiError(503, "Sentinel incident workflow claim failed", "sentinel_incident_claim_failed");
  }
};

export const handleAdminSentinelIncidentAck = async (
  req: Request,
  dependencies: Readonly<{
    getKv?: typeof getKv;
    acknowledge?: typeof acknowledgeSentinelIncident;
    isProduction?: typeof isSentinelProductionRuntime;
  }> = {},
): Promise<Response> => {
  if (!(dependencies.isProduction ?? isSentinelProductionRuntime)()) {
    return openaiError(404, "Not found", "not_found");
  }
  const identity = await readWorkflowIdentity(req);
  if (identity instanceof Response) return identity;
  try {
    const kv = await (dependencies.getKv ?? getKv)();
    if (!kv) return openaiError(503, "Sentinel incident storage is unavailable", "sentinel_incident_unavailable");
    await (dependencies.acknowledge ?? acknowledgeSentinelIncident)(kv, identity);
    return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
  } catch (error) {
    if (error instanceof SentinelIncidentAckConflict) {
      return openaiError(409, "Sentinel incident acknowledgement is stale", "sentinel_incident_ack_conflict");
    }
    return openaiError(503, "Sentinel incident acknowledgement failed", "sentinel_incident_ack_failed");
  }
};

export const handleAdminSentinelIncidentDefer = async (
  req: Request,
  dependencies: Readonly<{
    getKv?: typeof getKv;
    defer?: typeof deferSentinelIncident;
    isProduction?: typeof isSentinelProductionRuntime;
  }> = {},
): Promise<Response> => {
  if (!(dependencies.isProduction ?? isSentinelProductionRuntime)()) {
    return openaiError(404, "Not found", "not_found");
  }
  const deferral = await readWorkflowDeferral(req);
  if (deferral instanceof Response) return deferral;
  try {
    const kv = await (dependencies.getKv ?? getKv)();
    if (!kv) return openaiError(503, "Sentinel incident storage is unavailable", "sentinel_incident_unavailable");
    const disposition = await (dependencies.defer ?? deferSentinelIncident)(kv, deferral);
    return new Response(null, {
      status: 204,
      headers: {
        "Cache-Control": "no-store",
        "X-Sentinel-Incident-Disposition": disposition,
      },
    });
  } catch (error) {
    if (error instanceof SentinelIncidentDeferConflict) {
      return openaiError(409, "Sentinel incident workflow deferral is stale", "sentinel_incident_defer_conflict");
    }
    return openaiError(503, "Sentinel incident workflow deferral failed", "sentinel_incident_defer_failed");
  }
};
