import { SENTINEL_POLICY } from "./policy.ts";
import type { NativeReviewFinding, NativeReviewReport, TriageSeverity } from "./types.ts";

const PRIORITY_PATTERN = /(?:^|\s)\[(P[0-3])\]\s+(.+)$/;
const LOCATION_PATTERN = /(?:`)?([A-Za-z0-9_.@/+\-]+:\d+(?::\d+)?)(?:`)?/;
const NO_FINDINGS_PATTERNS = [
  /\bno findings\b/i,
  /\bdid not find any (?:actionable )?(?:issues|findings)\b/i,
  /\bno (?:actionable )?issues (?:found|identified)\b/i,
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
  value.replace(/\/[A-Za-z0-9_.@/+\-]+:\d+(?::\d+)?/gu, (location) => normalizeLocation(location));

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
    return {
      schema_version: 1,
      round,
      parse_status: NO_FINDINGS_PATTERNS.some((pattern) => pattern.test(raw)) ? "no_findings" : "unparseable",
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

export const blockingReviewFindings = (report: NativeReviewReport): NativeReviewFinding[] => {
  if (report.parse_status === "unparseable") throw new Error("Native Codex review output was not parseable");
  return report.findings.filter((finding) => finding.severity === "P0" || finding.severity === "P1");
};

export const canStartReviewRound = (completedRounds: number): boolean =>
  Number.isSafeInteger(completedRounds) && completedRounds >= 0 &&
  completedRounds < SENTINEL_POLICY.maximumReviewRounds;

type BacklogEntry = {
  fingerprint: string;
  severity: "P2" | "P3";
  first: string;
  latest: string;
  sha: string;
  location: string;
  finding: string;
  disposition: string;
};

const cleanCell = (value: string, maximum = 800): string => {
  const normalized = value.trim().replaceAll("\r", " ").replaceAll("\n", " ").replace(/\s+/g, " ")
    .slice(0, maximum) || "unknown";
  return normalized.replace(/[|<>`]|[^\x20-\x7e]/gu, (character) => {
    if (character === "|") return "&#124;";
    if (character === "<") return "&lt;";
    if (character === ">") return "&gt;";
    if (character === "`") return "&#96;";
    return `&#x${character.codePointAt(0)!.toString(16)};`;
  });
};

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

const backlogTable = (entries: readonly BacklogEntry[]): string[] => {
  const rows = entries.map((entry) => [
    `\`${entry.fingerprint}\``,
    entry.severity,
    entry.first,
    entry.latest,
    `\`${entry.sha}\``,
    `\`${entry.location}\``,
    entry.finding,
    entry.disposition,
  ]);
  const widths = BACKLOG_HEADERS.map((header, index) =>
    Math.max(header.length, ...rows.map((row) => row[index]!.length))
  );
  const renderRow = (cells: readonly string[]): string =>
    `| ${cells.map((cell, index) => cell.padEnd(widths[index]!)).join(" | ")} |`;
  return [
    renderRow(BACKLOG_HEADERS),
    renderRow(widths.map((width) => "-".repeat(width))),
    ...rows.map(renderRow),
  ];
};

const parseBacklogRows = (markdown: string): Map<string, BacklogEntry> => {
  const entries = new Map<string, BacklogEntry>();
  for (const line of markdown.split("\n")) {
    const row = line.trim();
    if (!row.startsWith("| `") || !row.endsWith("|")) continue;
    const cells = row.slice(1, -1).split("|").map((cell) => cell.trim());
    if (cells.length !== 8) continue;
    const fingerprint = cells[0].replaceAll("`", "");
    const severity = cells[1];
    const sha = cells[4].replaceAll("`", "");
    if (
      !/^[0-9a-f]{16,64}$/.test(fingerprint) || (severity !== "P2" && severity !== "P3") ||
      !/^[0-9a-f]{40}$/.test(sha)
    ) continue;
    entries.set(fingerprint, {
      fingerprint,
      severity,
      first: cleanCell(cells[2], 64),
      latest: cleanCell(cells[3], 64),
      sha,
      location: cleanCell(cells[5].replaceAll("`", ""), 240),
      finding: cleanCell(cells[6]),
      disposition: cleanCell(cells[7], 120),
    });
  }
  return entries;
};

export const mergeReviewBacklog = (
  currentMarkdown: string,
  findings: readonly NativeReviewFinding[],
  affectedSha: string,
  observedAt: Date,
): string => {
  if (!/^[0-9a-f]{40}$/.test(affectedSha)) throw new Error("Backlog affected SHA must be a full Git SHA");
  const observed = observedAt.toISOString();
  const entries = parseBacklogRows(currentMarkdown);
  for (const finding of findings) {
    if (finding.severity !== "P2" && finding.severity !== "P3") continue;
    const existing = entries.get(finding.fingerprint);
    entries.set(finding.fingerprint, {
      fingerprint: finding.fingerprint,
      severity: finding.severity,
      first: existing?.first ?? observed,
      latest: observed,
      sha: affectedSha,
      location: cleanCell(finding.location, 240),
      finding: cleanCell(`${finding.title}${finding.body ? ` — ${finding.body}` : ""}`),
      disposition: existing?.disposition ?? "open",
    });
  }
  const sortedEntries = [...entries.values()].sort((left, right) =>
    left.severity.localeCompare(right.severity) || left.first.localeCompare(right.first) ||
    left.fingerprint.localeCompare(right.fingerprint)
  );
  return [
    "# Sentinel Review Backlog",
    "",
    "Unresolved native Codex review findings at P2 or P3 are tracked here. P0 and P1 findings block the cycle and never enter",
    "this backlog.",
    "",
    ...backlogTable(sortedEntries),
    "",
  ].join("\n");
};
