import { isSentinelProtectedImplementationPath, SENTINEL_POLICY } from "./policy.ts";
import type {
  NativeReviewFinding,
  NativeReviewReport,
  SentinelInterval,
  TriageReport,
  TriageSeverity,
} from "./types.ts";

const PRIORITY_PATTERN = /(?:^|\s)\[(P[0-3])\]\s+(.+)$/;
const LOCATION_PATTERN = /(?:`)?([A-Za-z0-9_.@/+\-]+:\d+(?:-\d+)?(?::\d+)?)(?:`)?/;
const NO_FINDINGS_PATTERNS = [
  /^no findings\.?$/i,
  /^(?:i|we) did not find any (?:actionable )?(?:issues|findings|defects?)\.?$/i,
  /^no (?:actionable )?(?:issues|findings|defects?)(?: (?:was|were))? (?:found|identified|detected)(?: in (?:the )?(?:changes|patch|diff|candidate|implementation|code under review))?\.(?: (?:focused )?(?:sentinel )?tests? (?:pass|passed)\.)?$/i,
];
const CHECKOUT_PATH_MARKERS = ["/candidate-worktree/", "/checkout/"];

const normalizeLocation = (value: string): string => {
  for (const marker of CHECKOUT_PATH_MARKERS) {
    const index = value.lastIndexOf(marker);
    if (index >= 0) return value.slice(index + marker.length);
  }
  return value;
};

const normalizeReviewLocations = (value: string): string =>
  value.replace(/\/[A-Za-z0-9_.@/+\-]+:\d+(?:-\d+)?(?::\d+)?/gu, (location) => normalizeLocation(location));

/** Codex writes the final native review to stdout; stderr is only a fallback for older clients. */
export const nativeReviewParseInput = (stdout: string, stderr: string): string =>
  stdout.trim().length > 0 ? stdout : stderr;

const sha256 = async (value: string): Promise<string> => {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const normalizedFindingText = (value: string): string => value.trim().replace(/\s+/g, " ").toLowerCase();

const findingFingerprint = async (
  title: string,
  body: string,
  location: string,
): Promise<string> =>
  await sha256(
    `${normalizedFindingText(title)}\n${normalizedFindingText(body)}\n${location.toLowerCase()}`,
  );

export const parseNativeReview = async (raw: string, round: number): Promise<NativeReviewReport> => {
  if (!Number.isSafeInteger(round) || round < 1 || round > SENTINEL_POLICY.maximumReviewRounds) {
    throw new Error("Native review round is outside the configured limit");
  }
  const lines = raw.replaceAll("\r\n", "\n").split("\n");
  const pending: Array<{ severity: TriageSeverity; title: string; body: string[]; location: string }> = [];
  let current: (typeof pending)[number] | null = null;
  for (const line of lines) {
    const match = line.match(PRIORITY_PATTERN);
    if (match) {
      if (current) pending.push(current);
      const titleAndLocation = normalizeReviewLocations(match[2].trim().replace(/^[-*]\s+/, ""));
      const location = normalizeLocation(titleAndLocation.match(LOCATION_PATTERN)?.[1] ?? "unknown");
      current = { severity: match[1] as TriageSeverity, title: titleAndLocation, body: [], location };
      continue;
    }
    if (current) {
      if (/^\s*(?:[-*]|\d+\.)\s+\[P[0-3]\]/.test(line)) {
        pending.push(current);
        current = null;
      } else if (line.trim()) {
        const bodyLine = normalizeReviewLocations(line.trim());
        current.body.push(bodyLine);
        if (current.location === "unknown") {
          current.location = normalizeLocation(bodyLine.match(LOCATION_PATTERN)?.[1] ?? "unknown");
        }
      }
    }
  }
  if (current) pending.push(current);

  if (pending.length === 0) {
    const normalized = raw.trim().replace(/\s+/g, " ");
    return {
      schema_version: 1,
      round,
      parse_status: NO_FINDINGS_PATTERNS.some((pattern) => pattern.test(normalized)) ? "no_findings" : "unparseable",
      findings: [],
    };
  }
  const findings: NativeReviewFinding[] = [];
  for (const item of pending) {
    const body = item.body.join("\n");
    findings.push({
      fingerprint: await findingFingerprint(item.title, body, item.location),
      severity: item.severity,
      title: item.title,
      body,
      location: item.location,
    });
  }
  return { schema_version: 1, round, parse_status: "findings", findings };
};

export const blockingReviewFindings = (
  report: NativeReviewReport,
  requiredFingerprint?: string,
): NativeReviewFinding[] => {
  if (report.parse_status === "unparseable") throw new Error("Native Codex review output was not parseable");
  if (requiredFingerprint !== undefined && !REVIEW_BACKLOG_FINGERPRINT.test(requiredFingerprint)) {
    throw new Error("Required review fingerprint is invalid");
  }
  return report.findings.filter((finding) =>
    finding.severity === "P0" || finding.severity === "P1" || finding.fingerprint === requiredFingerprint
  );
};

export const canStartReviewRound = (completedRounds: number): boolean =>
  Number.isSafeInteger(completedRounds) && completedRounds >= 0 &&
  completedRounds < SENTINEL_POLICY.maximumReviewRounds;

export type ReviewBacklogDisposition = "open" | "resolved" | "accepted_risk" | "manual_required";

export type ReviewBacklogEntry = Readonly<{
  fingerprint: string;
  severity: "P2" | "P3";
  first: string;
  latest: string;
  sha: string;
  location: string;
  finding: string;
  disposition: ReviewBacklogDisposition;
}>;

const MAX_REVIEW_BACKLOG_BYTES = 256 * 1_024;
const MAX_REVIEW_BACKLOG_ENTRIES = 256;
const MAX_REVIEW_BACKLOG_LINE_LENGTH = 4_096;
const REVIEW_BACKLOG_FINGERPRINT = /^[0-9a-f]{64}$/u;
const REVIEW_BACKLOG_SHA = /^[0-9a-f]{40}$/u;
const REVIEW_BACKLOG_TIMESTAMP = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?Z$/u;
const REVIEW_BACKLOG_LOCATION = /^([A-Za-z0-9_.@/+\-]+):(\d+)(?:-(\d+))?(?::(\d+))?$/u;
const REVIEW_BACKLOG_DISPOSITIONS = new Set<ReviewBacklogDisposition>([
  "open",
  "resolved",
  "accepted_risk",
  "manual_required",
]);

const normalizeCellText = (value: string, maximum = 800): string =>
  value.trim().replaceAll("\r", " ").replaceAll("\n", " ").replace(/\s+/g, " ").slice(0, maximum) || "unknown";

const cleanCell = (value: string, maximum = 800): string => {
  const normalized = normalizeCellText(value, maximum);
  return normalized.replace(/[|<>`]|[^\x20-\x7e]/gu, (character) => {
    if (character === "|") return "&#124;";
    if (character === "<") return "&lt;";
    if (character === ">") return "&gt;";
    if (character === "`") return "&#96;";
    return `&#x${character.codePointAt(0)!.toString(16)};`;
  });
};

const decodeCell = (value: string): string =>
  value
    .replace(/&#x([0-9a-f]+);/giu, (_match, encoded: string) => String.fromCodePoint(Number.parseInt(encoded, 16)))
    .replace(/&#(\d+);/gu, (_match, encoded: string) => String.fromCodePoint(Number.parseInt(encoded, 10)))
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">");

const BACKLOG_HEADERS = Object.freeze([
  "Fingerprint",
  "Severity",
  "First observed",
  "Latest observed",
  "Affected SHA",
  "Location",
  "Finding",
  "Disposition",
]);

const backlogTable = (entries: readonly ReviewBacklogEntry[]): string[] => {
  const rows = entries.map((entry) => [
    `\`${entry.fingerprint}\``,
    entry.severity,
    cleanCell(entry.first, 64),
    cleanCell(entry.latest, 64),
    `\`${entry.sha}\``,
    `\`${cleanCell(entry.location, 240)}\``,
    cleanCell(entry.finding),
    entry.disposition,
  ]);
  const widths = BACKLOG_HEADERS.map((header, index) =>
    Math.max(header.length, ...rows.map((row) => row[index]!.length))
  );
  const renderRow = (cells: readonly string[]): string =>
    `| ${cells.map((cell, index) => cell.padEnd(widths[index]!)).join(" | ")} |`;
  const rendered = [
    renderRow(BACKLOG_HEADERS),
    renderRow(widths.map((width) => "-".repeat(width))),
    ...rows.map(renderRow),
  ];
  if (rendered.some((row) => row.length > MAX_REVIEW_BACKLOG_LINE_LENGTH)) {
    throw new Error("Sentinel review backlog row exceeds its length limit");
  }
  return rendered;
};

const validBacklogTimestamp = (value: string): boolean =>
  REVIEW_BACKLOG_TIMESTAMP.test(value) && Number.isFinite(Date.parse(value));

export const reviewBacklogLocationPath = (location: string): string | null => {
  const match = location.match(REVIEW_BACKLOG_LOCATION);
  if (!match) return null;
  const line = Number(match[2]);
  const endLine = match[3] === undefined ? line : Number(match[3]);
  const column = match[4] === undefined ? 1 : Number(match[4]);
  if (
    !Number.isSafeInteger(line) || line <= 0 || !Number.isSafeInteger(endLine) || endLine < line ||
    !Number.isSafeInteger(column) || column <= 0
  ) return null;
  return match[1];
};

const sortedBacklogEntries = (entries: readonly ReviewBacklogEntry[]): ReviewBacklogEntry[] =>
  [...entries].sort((left, right) =>
    (left.severity === right.severity ? 0 : left.severity === "P2" ? -1 : 1) ||
    Date.parse(left.first) - Date.parse(right.first) || left.fingerprint.localeCompare(right.fingerprint)
  );

export const parseReviewBacklog = (markdown: string): ReviewBacklogEntry[] => {
  if (new TextEncoder().encode(markdown).byteLength > MAX_REVIEW_BACKLOG_BYTES) {
    throw new Error("Sentinel review backlog exceeds its byte limit");
  }
  const entries = new Map<string, ReviewBacklogEntry>();
  for (const line of markdown.replaceAll("\r\n", "\n").split("\n")) {
    const row = line.trim();
    if (!row.startsWith("|")) continue;
    if (row.length > MAX_REVIEW_BACKLOG_LINE_LENGTH) {
      throw new Error("Sentinel review backlog row exceeds its length limit");
    }
    if (!row.endsWith("|")) throw new Error("Sentinel review backlog contains an unterminated row");
    const cells = row.slice(1, -1).split("|").map((cell) => cell.trim());
    if (cells.length === BACKLOG_HEADERS.length && cells.every((cell, index) => cell === BACKLOG_HEADERS[index])) {
      continue;
    }
    if (cells.length === BACKLOG_HEADERS.length && cells.every((cell) => /^-+$/u.test(cell))) continue;
    if (cells.length !== 8) throw new Error("Sentinel review backlog row has the wrong number of columns");
    if (!/^`[0-9a-f]{64}`$/u.test(cells[0]) || !/^`[0-9a-f]{40}`$/u.test(cells[4])) {
      throw new Error("Sentinel review backlog row has an invalid fingerprint or SHA");
    }
    const fingerprint = cells[0].slice(1, -1);
    const severity = cells[1];
    const first = decodeCell(cells[2]);
    const latest = decodeCell(cells[3]);
    const sha = cells[4].slice(1, -1);
    const locationCell = cells[5];
    const finding = normalizeCellText(decodeCell(cells[6]));
    const disposition = cells[7] as ReviewBacklogDisposition;
    if (
      !REVIEW_BACKLOG_FINGERPRINT.test(fingerprint) || (severity !== "P2" && severity !== "P3") ||
      !validBacklogTimestamp(first) || !validBacklogTimestamp(latest) || Date.parse(latest) < Date.parse(first) ||
      !REVIEW_BACKLOG_SHA.test(sha) || !/^`[^`]+`$/u.test(locationCell) ||
      (decodeCell(locationCell.slice(1, -1)) !== "unknown" &&
        !reviewBacklogLocationPath(decodeCell(locationCell.slice(1, -1)))) ||
      finding === "unknown" ||
      !REVIEW_BACKLOG_DISPOSITIONS.has(disposition)
    ) throw new Error("Sentinel review backlog row is invalid");
    if (entries.has(fingerprint)) throw new Error("Sentinel review backlog contains a duplicate fingerprint");
    if (entries.size >= MAX_REVIEW_BACKLOG_ENTRIES) {
      throw new Error("Sentinel review backlog exceeds its entry limit");
    }
    entries.set(fingerprint, {
      fingerprint,
      severity,
      first,
      latest,
      sha,
      location: normalizeCellText(decodeCell(locationCell.slice(1, -1)), 240),
      finding,
      disposition,
    });
  }
  const parsed = sortedBacklogEntries([...entries.values()]);
  if (markdown !== renderReviewBacklog(parsed)) {
    throw new Error("Sentinel review backlog is not in its canonical complete form");
  }
  return parsed;
};

export const renderReviewBacklog = (entries: readonly ReviewBacklogEntry[]): string => {
  if (entries.length > MAX_REVIEW_BACKLOG_ENTRIES) throw new Error("Sentinel review backlog exceeds its entry limit");
  const markdown = [
    "# Sentinel Review Backlog",
    "",
    "Native Codex review findings at P2 or P3 are tracked here. P0 and P1 findings block the cycle and never enter this",
    "backlog.",
    "",
    ...backlogTable(sortedBacklogEntries(entries)),
    "",
  ].join("\n");
  if (new TextEncoder().encode(markdown).byteLength > MAX_REVIEW_BACKLOG_BYTES) {
    throw new Error("Sentinel review backlog exceeds its byte limit");
  }
  return markdown;
};

export const selectNextReviewBacklogEntry = (markdown: string): ReviewBacklogEntry | null => {
  for (const entry of parseReviewBacklog(markdown)) {
    if (entry.disposition !== "open") continue;
    const path = reviewBacklogLocationPath(entry.location);
    if (!path || isSentinelProtectedImplementationPath(path)) continue;
    return entry;
  }
  return null;
};

export const reviewBacklogTriageReport = (
  entry: ReviewBacklogEntry,
  interval: SentinelInterval,
): TriageReport => ({
  schema_version: 1,
  interval,
  findings: [{
    id: `review-backlog:${entry.fingerprint}`,
    fingerprint: entry.fingerprint,
    severity: entry.severity,
    title: `Resolve native review backlog finding ${entry.fingerprint.slice(0, 12)}`,
    affected_surface: entry.location,
    evidence: [{
      source: "repository",
      reference: `${SENTINEL_POLICY.paths.reviewBacklog}#${entry.fingerprint}`,
      detail: entry.finding,
    }],
    proposed_correction: entry.finding,
    validation_requirements: [
      `Run focused tests for ${entry.location}`,
      "Run repository formatting, lint, build, and affected tests",
    ],
    actionable: true,
  }],
  no_findings_reason: null,
});

export const applyReviewBacklogImplementationDisposition = (
  markdown: string,
  fingerprint: string,
  disposition: "resolved" | "manual_required",
): Readonly<{ markdown: string; disposition: "resolved" | "manual_required" }> => {
  if (!REVIEW_BACKLOG_FINGERPRINT.test(fingerprint)) {
    throw new Error("Sentinel review backlog completion input is invalid");
  }
  const entries = parseReviewBacklog(markdown);
  const index = entries.findIndex((entry) => entry.fingerprint === fingerprint);
  if (index < 0 || entries[index]!.disposition !== "open") {
    throw new Error("Only the selected open Sentinel review backlog entry can be completed");
  }
  entries[index] = { ...entries[index]!, disposition };
  return { markdown: renderReviewBacklog(entries), disposition };
};

export const mergeReviewBacklog = (
  currentMarkdown: string,
  findings: readonly NativeReviewFinding[],
  affectedSha: string,
  observedAt: Date,
): string => {
  if (!/^[0-9a-f]{40}$/.test(affectedSha)) throw new Error("Backlog affected SHA must be a full Git SHA");
  const observed = observedAt.toISOString();
  const entries = new Map(parseReviewBacklog(currentMarkdown).map((entry) => [entry.fingerprint, entry]));
  for (const finding of findings) {
    if (finding.severity !== "P2" && finding.severity !== "P3") continue;
    const existing = entries.get(finding.fingerprint);
    entries.set(finding.fingerprint, {
      fingerprint: finding.fingerprint,
      severity: finding.severity,
      first: existing?.first ?? observed,
      latest: observed,
      sha: affectedSha,
      location: normalizeCellText(finding.location, 240),
      finding: normalizeCellText(`${finding.title}${finding.body ? ` — ${finding.body}` : ""}`),
      disposition: existing?.disposition === "accepted_risk" || existing?.disposition === "manual_required"
        ? existing.disposition
        : "open",
    });
  }
  return renderReviewBacklog([...entries.values()]);
};
