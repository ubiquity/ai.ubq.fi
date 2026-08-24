import { isRecord } from "../../src/utils.ts";

export type IsoTimestamp = string;

export type SentinelInterval = Readonly<{
  start: IsoTimestamp;
  end: IsoTimestamp;
  duration_ms: number;
}>;

export type TriageSeverity = "P0" | "P1" | "P2" | "P3";

export type TriageEvidence = Readonly<{
  source: "deno_log" | "replay_manifest" | "repository" | "github_issue";
  reference: string;
  detail: string;
}>;

export type TriageFinding = Readonly<{
  id: string;
  fingerprint: string;
  severity: TriageSeverity;
  title: string;
  affected_surface: string;
  evidence: readonly TriageEvidence[];
  proposed_correction: string;
  validation_requirements: readonly string[];
  actionable: boolean;
}>;

export type TriageReport = Readonly<{
  schema_version: 1;
  interval: SentinelInterval;
  findings: readonly TriageFinding[];
  no_findings_reason: string | null;
}>;

export type FindingDisposition = Readonly<{
  finding_id: string;
  status: "implemented" | "already_fixed" | "not_actionable" | "blocked";
  summary: string;
  changed_files: readonly string[];
  validation: readonly string[];
}>;

export type ImplementationReport = Readonly<{
  schema_version: 1;
  candidate_sha: string | null;
  dispositions: readonly FindingDisposition[];
  replay_acceptances: readonly {
    capture_fingerprint: string;
    disposition: "fixed" | "accepted_unavailable" | "accepted_still_failing" | "not_applicable";
    reasoning: string;
  }[];
  summary: string;
}>;

export type NativeReviewFinding = Readonly<{
  fingerprint: string;
  severity: TriageSeverity;
  title: string;
  body: string;
  location: string;
}>;

export type NativeReviewReport = Readonly<{
  schema_version: 1;
  round: number;
  parse_status: "findings" | "no_findings" | "unparseable";
  findings: readonly NativeReviewFinding[];
}>;

export type ReplayCase = Readonly<{
  fingerprint: string;
  case_group_digest: string;
  captured_at_ms: number;
  endpoint: string;
  method: string;
  content_type: string | null;
  compatibility_headers: Readonly<Record<string, string>>;
  body: Uint8Array<ArrayBuffer>;
  original: Readonly<{
    status: number;
    stream: boolean | null;
    framing_valid: boolean;
    completed: boolean;
    terminal_type: string | null;
    failure_kind: string | null;
    provider_route: string;
    failure_signature: string;
    internal_terminal_type?: string | null;
    internal_failure_kind?: string | null;
    synthetic_terminal_type?: string | null;
  }>;
}>;

export type ReplayResult = Readonly<{
  capture_fingerprint: string;
  attempted: boolean;
  unavailable_reason: string | null;
  http_status: number | null;
  sse_framing_valid: boolean | null;
  terminal_event: string | null;
  provider_route: string | null;
  observed_failure_signature: string | null;
  outcome: "improved" | "same_failure" | "regressed" | "unavailable";
  comparison: Readonly<{
    status_matches_original: boolean | null;
    terminal_matches_original: boolean | null;
    provider_matches_original: boolean | null;
    failure_signature_matches_original: boolean | null;
    framing_matches_original: boolean | null;
  }>;
}>;

export type DeploymentIdentity = Readonly<{
  app: string;
  git_sha: string;
  revision: string;
  health_url: string;
  observed_at: IsoTimestamp;
}>;

export type ProductionDecision = Readonly<{
  schema_version: 1;
  decision: "keep" | "rollback";
  evidence: readonly string[];
  traffic_sufficient: boolean;
  candidate: DeploymentIdentity;
  previous: DeploymentIdentity;
}>;

const isStringArray = (value: unknown): value is string[] =>
  Array.isArray(value) && value.every((item) => typeof item === "string");

const isSeverity = (value: unknown): value is TriageSeverity =>
  value === "P0" || value === "P1" || value === "P2" || value === "P3";

const isInterval = (value: unknown): value is SentinelInterval =>
  isRecord(value) && typeof value.start === "string" && typeof value.end === "string" &&
  typeof value.duration_ms === "number" && Number.isSafeInteger(value.duration_ms) && value.duration_ms > 0;

export const isTriageReport = (value: unknown): value is TriageReport => {
  if (!isRecord(value) || value.schema_version !== 1 || !isInterval(value.interval) || !Array.isArray(value.findings)) {
    return false;
  }
  if (!(value.no_findings_reason === null || typeof value.no_findings_reason === "string")) return false;
  if (
    (value.findings.length === 0 &&
      (typeof value.no_findings_reason !== "string" || value.no_findings_reason.trim().length === 0)) ||
    (value.findings.length > 0 && value.no_findings_reason !== null)
  ) return false;
  const ids = new Set<string>();
  return value.findings.every((finding) => {
    if (!isRecord(finding) || typeof finding.id !== "string" || finding.id.length === 0 || ids.has(finding.id)) {
      return false;
    }
    ids.add(finding.id);
    return typeof finding.fingerprint === "string" && finding.fingerprint.length >= 16 &&
      isSeverity(finding.severity) && typeof finding.title === "string" &&
      typeof finding.affected_surface === "string" && Array.isArray(finding.evidence) &&
      finding.evidence.length > 0 && finding.evidence.every((evidence) =>
        isRecord(evidence) &&
        (evidence.source === "deno_log" || evidence.source === "replay_manifest" || evidence.source === "repository" ||
          evidence.source === "github_issue") &&
        typeof evidence.reference === "string" && typeof evidence.detail === "string"
      ) && typeof finding.proposed_correction === "string" &&
      isStringArray(finding.validation_requirements) && typeof finding.actionable === "boolean";
  });
};

export const isImplementationReport = (value: unknown): value is ImplementationReport => {
  if (
    !isRecord(value) || value.schema_version !== 1 || !Array.isArray(value.dispositions) ||
    !Array.isArray(value.replay_acceptances) || typeof value.summary !== "string"
  ) return false;
  if (!(value.candidate_sha === null || typeof value.candidate_sha === "string")) return false;
  return value.dispositions.every((item) =>
    isRecord(item) && typeof item.finding_id === "string" &&
    (item.status === "implemented" || item.status === "already_fixed" || item.status === "not_actionable" ||
      item.status === "blocked") &&
    typeof item.summary === "string" && isStringArray(item.changed_files) &&
    isStringArray(item.validation)
  ) && value.replay_acceptances.every((item) =>
    isRecord(item) && typeof item.capture_fingerprint === "string" &&
    (item.disposition === "fixed" || item.disposition === "accepted_unavailable" ||
      item.disposition === "accepted_still_failing" || item.disposition === "not_applicable") &&
    typeof item.reasoning === "string" &&
    (item.disposition === "fixed" || item.disposition === "not_applicable" || item.reasoning.trim().length > 0)
  );
};

export const assertCompleteFindingDispositions = (
  triage: TriageReport,
  implementation: ImplementationReport,
): void => {
  const expected = new Set(triage.findings.map((finding) => finding.id));
  const actual = new Set(implementation.dispositions.map((item) => item.finding_id));
  if (
    implementation.dispositions.length !== expected.size || actual.size !== implementation.dispositions.length ||
    expected.size !== actual.size || [...expected].some((id) => !actual.has(id))
  ) {
    throw new Error("Implementation report must record one disposition for every triage finding");
  }
};

export const assertActionableFindingsResolved = (
  triage: TriageReport,
  implementation: ImplementationReport,
): void => {
  const dispositions = new Map(implementation.dispositions.map((item) => [item.finding_id, item]));
  const unresolved = triage.findings.filter((finding) => {
    if (!finding.actionable) return false;
    const disposition = dispositions.get(finding.id);
    return disposition?.status !== "implemented" && disposition?.status !== "already_fixed";
  });
  if (unresolved.length > 0) {
    throw new Error(
      `Actionable triage findings remain unresolved: ${unresolved.map((finding) => finding.id).join(", ")}`,
    );
  }
};

export const TRIAGE_OUTPUT_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["schema_version", "interval", "findings", "no_findings_reason"],
  properties: {
    schema_version: { type: "integer", const: 1 },
    interval: {
      type: "object",
      additionalProperties: false,
      required: ["start", "end", "duration_ms"],
      properties: {
        start: { type: "string" },
        end: { type: "string" },
        duration_ms: { type: "integer", minimum: 1 },
      },
    },
    findings: {
      type: "array",
      items: {
        type: "object",
        additionalProperties: false,
        required: [
          "id",
          "fingerprint",
          "severity",
          "title",
          "affected_surface",
          "evidence",
          "proposed_correction",
          "validation_requirements",
          "actionable",
        ],
        properties: {
          id: { type: "string", minLength: 1 },
          fingerprint: { type: "string", minLength: 16 },
          severity: { type: "string", enum: ["P0", "P1", "P2", "P3"] },
          title: { type: "string", minLength: 1 },
          affected_surface: { type: "string", minLength: 1 },
          evidence: {
            type: "array",
            minItems: 1,
            items: {
              type: "object",
              additionalProperties: false,
              required: ["source", "reference", "detail"],
              properties: {
                source: { type: "string", enum: ["deno_log", "replay_manifest", "repository", "github_issue"] },
                reference: { type: "string" },
                detail: { type: "string" },
              },
            },
          },
          proposed_correction: { type: "string", minLength: 1 },
          validation_requirements: { type: "array", items: { type: "string" } },
          actionable: { type: "boolean" },
        },
      },
    },
    no_findings_reason: { type: ["string", "null"] },
  },
} as const;

export const IMPLEMENTATION_OUTPUT_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["schema_version", "candidate_sha", "dispositions", "replay_acceptances", "summary"],
  properties: {
    schema_version: { type: "integer", const: 1 },
    candidate_sha: { type: ["string", "null"] },
    dispositions: {
      type: "array",
      items: {
        type: "object",
        additionalProperties: false,
        required: ["finding_id", "status", "summary", "changed_files", "validation"],
        properties: {
          finding_id: { type: "string" },
          status: { type: "string", enum: ["implemented", "already_fixed", "not_actionable", "blocked"] },
          summary: { type: "string" },
          changed_files: { type: "array", items: { type: "string" } },
          validation: { type: "array", items: { type: "string" } },
        },
      },
    },
    replay_acceptances: {
      type: "array",
      items: {
        type: "object",
        additionalProperties: false,
        required: ["capture_fingerprint", "disposition", "reasoning"],
        properties: {
          capture_fingerprint: { type: "string" },
          disposition: {
            type: "string",
            enum: ["fixed", "accepted_unavailable", "accepted_still_failing", "not_applicable"],
          },
          reasoning: { type: "string" },
        },
      },
    },
    summary: { type: "string" },
  },
} as const;

export const MONITOR_OUTPUT_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["schema_version", "decision", "evidence", "traffic_sufficient", "observed_regression"],
  properties: {
    schema_version: { type: "integer", const: 1 },
    decision: { type: "string", enum: ["keep", "rollback"] },
    evidence: { type: "array", minItems: 1, items: { type: "string" } },
    traffic_sufficient: { type: "boolean" },
    observed_regression: { type: "boolean" },
  },
} as const;
