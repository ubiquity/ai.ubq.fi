const PROTECTED_IMPLEMENTATION_PATHS = Object.freeze([
  ".github/workflows/provider-sentinel.yml",
  ".github/workflows/deno-deploy.yml",
  "AGENTS.md",
  "deno.json",
  "docs/sentinel-review-backlog.md",
  "scripts/sentinel/codex.ts",
  "scripts/sentinel/deploy.ts",
  "scripts/sentinel/github.ts",
  "scripts/sentinel/main.ts",
  "scripts/sentinel/policy.ts",
  "scripts/sentinel/quota.ts",
  "scripts/sentinel/replay.ts",
  "scripts/sentinel/review.ts",
  "scripts/sentinel/types.ts",
  "scripts/sentinel/validation.ts",
  "scripts/sentinel/windows.ts",
  "src/sentinel_replay_admin.ts",
  "src/sentinel_replay_capture.ts",
  "tests/sentinel-deploy.test.ts",
  "tests/sentinel-orchestrator.test.ts",
  "tests/sentinel-quota-codex.test.ts",
  "tests/sentinel-replay-capture.test.ts",
]);

const AGENT_INSTRUCTION_BASENAMES = new Set(["AGENTS.md", "AGENTS.override.md", "SKILL.md"]);

/**
 * Returns true for Sentinel controls and repository content that can change a
 * Codex agent's instructions, project configuration, command rules, or skills.
 */
export const isSentinelProtectedImplementationPath = (path: string): boolean => {
  const segments = path.split("/");
  if (
    path.length === 0 || path.startsWith("/") || path.includes("\\") ||
    segments.some((segment) => segment === "" || segment === "." || segment === "..")
  ) return true;
  const basename = segments.at(-1)!;
  if (AGENT_INSTRUCTION_BASENAMES.has(basename)) return true;
  if (segments.includes(".codex")) return true;
  if (segments.some((segment, index) => segment === ".agents" && segments[index + 1] === "skills")) return true;
  return PROTECTED_IMPLEMENTATION_PATHS.includes(path) || path.startsWith(".github/workflows/") ||
    path === "deno.jsonc" || path.startsWith("scripts/sentinel/") || path.startsWith("src/sentinel_replay_") ||
    /^tests\/sentinel-.*\.test\.ts$/u.test(path);
};

export const SENTINEL_POLICY = Object.freeze({
  version: 1,
  developmentBranch: "development",
  developmentRef: "refs/heads/development",
  temporaryBranchPrefix: "sentinel/candidate-",
  maximumReviewRounds: 3,
  productionLatestStartMs: 90 * 60 * 1_000,
  monitorDurationMs: 30 * 60 * 1_000,
  monitorPollMs: 30 * 1_000,
  triage: Object.freeze({ model: "gpt-5.6-sol", reasoning: "medium" as const }),
  implementation: Object.freeze({ model: "gpt-5.6-luna", reasoning: "max" as const }),
  monitoring: Object.freeze({ model: "gpt-5.6-sol", reasoning: "medium" as const }),
  deno: Object.freeze({
    organization: "ubiquity-dao",
    productionApp: "ai-ubq-fi",
    previewApp: "p-ai-ubq-fi",
    productionHealthUrls: Object.freeze([
      "https://ai-ubq-fi.ubiquity-dao.deno.net/health",
      "https://ai.ubq.fi/health",
    ]),
    previewHealthUrl: "https://p-ai-ubq-fi.ubiquity-dao.deno.net/health",
  }),
  github: Object.freeze({ deploymentWorkflow: "deno-deploy.yml" }),
  paths: Object.freeze({
    root: ".sentinel",
    rawLogs: ".sentinel/raw-logs",
    encryptedReplayCases: ".sentinel/replay-cases",
    reports: ".sentinel/reports",
    checkout: ".sentinel/candidate-worktree",
    reviewBacklog: "docs/sentinel-review-backlog.md",
  }),
  protectedImplementationPaths: PROTECTED_IMPLEMENTATION_PATHS,
});

export type SentinelMode = "daily" | "incident" | "preview";

export const isAutonomousMode = (mode: SentinelMode): boolean => mode !== "preview";
