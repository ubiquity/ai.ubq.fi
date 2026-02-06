import { extractCodexModelsFromText, resolveCodexBinaryPath } from "./codex-models.ts";

export type FlagValue = string | boolean | string[];

export type ParsedArgs = Readonly<{
  _: string[];
  flags: Record<string, FlagValue>;
}>;

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null;

const getString = (value: unknown): string | null => (typeof value === "string" ? value : null);

const TEXT_ENCODER = new TextEncoder();

const encodeHex = (bytes: Uint8Array): string => {
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
};

const sha256Hex = async (value: string): Promise<string> => {
  const digest = await crypto.subtle.digest("SHA-256", TEXT_ENCODER.encode(value));
  return encodeHex(new Uint8Array(digest));
};

const describeSecret = async (value: string | undefined): Promise<string> => {
  const trimmed = (value ?? "").trim();
  if (!trimmed) return "(unset)";
  try {
    const fp = (await sha256Hex(trimmed)).slice(0, 12);
    return `(set len=${trimmed.length} sha256=${fp})`;
  } catch {
    return `(set len=${trimmed.length})`;
  }
};

const classifyToken = (token: string): string => {
  const trimmed = token.trim();
  if (!trimmed) return "unset";
  if (trimmed.startsWith("ddw_")) return "deno_deploy_like(ddw_)";
  if (trimmed.startsWith("ubq_ai_")) return "ubq_ai_prefix";
  if (/^[0-9a-fA-F]{64}$/.test(trimmed)) return "hex64";
  if (trimmed.includes("_")) return "has_underscore";
  return "other";
};

const expandTilde = (path: string, homeDir: string | undefined): string => {
  if (path === "~") return homeDir ?? path;
  if (path.startsWith("~/") && homeDir) return `${homeDir}${path.slice(1)}`;
  return path;
};

const normalizeVersion = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed ? trimmed : null;
};

const readCodexVersionFile = async (runtime: UbqAiRuntime, homeDir: string | undefined): Promise<string | null> => {
  if (!homeDir) return null;
  try {
    const text = await runtime.readTextFile(`${homeDir}/.codex/version.json`);
    const parsed = JSON.parse(text) as Record<string, unknown>;
    return normalizeVersion(parsed.latest_version ?? parsed.version ?? parsed.current_version);
  } catch {
    return null;
  }
};

const readCodexPackageVersion = async (runtime: UbqAiRuntime, codexPath: string | null): Promise<string | null> => {
  if (!codexPath) return null;
  let realPath = codexPath;
  try {
    realPath = await Deno.realPath(codexPath);
  } catch {
    realPath = codexPath;
  }
  const normalized = realPath.replace(/\\/g, "/");
  const marker = "/node_modules/@openai/codex/";
  const index = normalized.lastIndexOf(marker);
  if (index === -1) return null;
  const pkgPath = `${normalized.slice(0, index + marker.length)}package.json`;
  try {
    const text = await runtime.readTextFile(pkgPath);
    const parsed = JSON.parse(text) as Record<string, unknown>;
    return normalizeVersion(parsed.version);
  } catch {
    return null;
  }
};

const resolveCodexClientVersion = async (
  runtime: UbqAiRuntime,
  codexPaths: (string | null | undefined)[],
  homeDir: string | undefined,
): Promise<string | null> => {
  for (const codexPath of codexPaths) {
    if (!codexPath) continue;
    const pkgVersion = await readCodexPackageVersion(runtime, codexPath);
    if (pkgVersion) return pkgVersion;
  }
  return await readCodexVersionFile(runtime, homeDir);
};

const loadCodexBinaryText = async (
  runtime: UbqAiRuntime,
  codexBinFlag: string | null,
  homeDir: string | undefined,
): Promise<
  { path: string; sourcePath: string; text: string; models: NonNullable<ReturnType<typeof extractCodexModelsFromText>> } | null
> => {
  const candidates: string[] = [];
  if (codexBinFlag) candidates.push(expandTilde(codexBinFlag, homeDir));
  const pathValue = runtime.envGet("PATH") ?? "";
  const separator = Deno.build.os === "windows" ? ";" : ":";
  for (const segment of pathValue.split(separator).filter(Boolean)) {
    candidates.push(`${segment.replace(/\/$/, "")}/codex`);
  }

  const seen = new Set<string>();

  for (const candidate of candidates) {
    if (seen.has(candidate)) continue;
    seen.add(candidate);
    try {
      const resolved = await resolveCodexBinaryPath(
        candidate,
        runtime.readTextFile,
        Deno.build.os,
        Deno.build.arch,
        Deno.realPath,
      );
      let text: string;
      try {
        text = await runtime.readTextFile(resolved);
      } catch (error) {
        try {
          const bytes = await Deno.readFile(resolved);
          text = new TextDecoder().decode(bytes);
        } catch {
          throw error;
        }
      }
      const models = extractCodexModelsFromText(text);
      if (!models) continue;
      return { path: resolved, sourcePath: candidate, text, models };
    } catch {
      // ignore
    }
  }
  return null;
};

const pushFlag = (flags: Record<string, FlagValue>, key: string, value: string | boolean): void => {
  const existing = flags[key];
  if (existing === undefined) {
    flags[key] = value;
    return;
  }
  if (Array.isArray(existing)) {
    if (typeof value === "string") existing.push(value);
    return;
  }
  if (typeof existing === "string" && typeof value === "string") {
    flags[key] = [existing, value];
  }
};

const BOOLEAN_FLAGS = new Set([
  "help",
  "json",
  "raw",
  "reset-usage",
  "stream",
  "token-only",
  "verbose",
]);

const REASONING_EFFORTS = new Set(["none", "minimal", "low", "medium", "high", "xhigh"]);

export const parseArgs = (args: string[]): ParsedArgs => {
  const flags: Record<string, FlagValue> = {};
  const positional: string[] = [];

  for (let i = 0; i < args.length; i++) {
    const arg = args[i] ?? "";
    if (arg === "--") continue;
    if (arg === "-h") {
      flags.help = true;
      continue;
    }
    if (arg === "-v") {
      flags.verbose = true;
      continue;
    }
    if (!arg.startsWith("--")) {
      positional.push(arg);
      continue;
    }

    const eq = arg.indexOf("=");
    if (eq !== -1) {
      const key = arg.slice(2, eq);
      const value = arg.slice(eq + 1);
      if (key) {
        if (BOOLEAN_FLAGS.has(key)) {
          const normalized = value.trim().toLowerCase();
          pushFlag(flags, key, !(normalized === "0" || normalized === "false" || normalized === "no"));
        } else {
          pushFlag(flags, key, value);
        }
      }
      continue;
    }

    const key = arg.slice(2);
    if (BOOLEAN_FLAGS.has(key)) {
      pushFlag(flags, key, true);
      continue;
    }
    const next = args[i + 1];

    // Smarter flag detection: "next" is only a new flag if it starts with "--"
    // OR it's a known short flag (like -h, -v).
    // This avoids misinterpreting PEM content (starts with "-----") as a flag.
    const isNextFlag = !!next && (next.startsWith("--") || next === "-h" || next === "-v");

    if (!next || isNextFlag) {
      if (key) pushFlag(flags, key, true);
      continue;
    }
    if (key) pushFlag(flags, key, next);
    i++;
  }

  return { _: positional, flags };
};

const SHORT_GIT_REVISION_LEN = 7;

const toShortGitRevision = (value: string | undefined): string | null => {
  const trimmed = (value ?? "").trim();
  if (!trimmed) return null;
  if (!/^[0-9a-fA-F]{7,40}$/.test(trimmed)) return null;
  return trimmed.slice(0, SHORT_GIT_REVISION_LEN);
};

const tryReadTextFile = async (runtime: UbqAiRuntime, path: string): Promise<string | null> => {
  try {
    return await runtime.readTextFile(path);
  } catch {
    return null;
  }
};

const parseGitDirFromDotGitFile = (content: string): string | null => {
  const firstLine = (content.split(/\r?\n/, 1)[0] ?? "").trim();
  const match = firstLine.match(/^gitdir:\s*(.+)\s*$/i);
  return match?.[1]?.trim() || null;
};

const isAbsolutePath = (path: string): boolean =>
  path.startsWith("/") || path.startsWith("\\") || /^[a-zA-Z]:[\\/]/.test(path);

const readGitHeadShortRevision = async (runtime: UbqAiRuntime, gitDir: string): Promise<string | null> => {
  const head = await tryReadTextFile(runtime, `${gitDir}/HEAD`);
  if (!head) return null;
  const trimmedHead = head.trim();

  const refMatch = trimmedHead.match(/^ref:\s*(.+)\s*$/);
  if (!refMatch) return toShortGitRevision(trimmedHead);

  const refPath = refMatch[1]?.trim();
  if (!refPath) return null;

  const ref = await tryReadTextFile(runtime, `${gitDir}/${refPath}`);
  if (ref) return toShortGitRevision(ref.trim());

  const packedRefs = await tryReadTextFile(runtime, `${gitDir}/packed-refs`);
  if (!packedRefs) return null;

  for (const line of packedRefs.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#") || trimmed.startsWith("^")) continue;
    const space = trimmed.indexOf(" ");
    if (space === -1) continue;
    const hash = trimmed.slice(0, space).trim();
    const refName = trimmed.slice(space + 1).trim();
    if (refName === refPath) return toShortGitRevision(hash);
  }

  return null;
};

const gitShortRevision = async (runtime: UbqAiRuntime): Promise<string | null> => {
  const env = toShortGitRevision(runtime.envGet("GIT_REVISION") ?? runtime.envGet("GITHUB_SHA") ?? undefined);
  if (env) return env;

  const gitRoots = [
    ".",
    "..",
    "../..",
    "../../..",
    "../../../..",
    "../../../../..",
    "../../../../../..",
    "../../../../../../..",
  ];
  for (const root of gitRoots) {
    const dotGitHead = await tryReadTextFile(runtime, `${root}/.git/HEAD`);
    if (dotGitHead) return await readGitHeadShortRevision(runtime, `${root}/.git`);

    const dotGitFile = await tryReadTextFile(runtime, `${root}/.git`);
    if (!dotGitFile) continue;
    const gitDir = parseGitDirFromDotGitFile(dotGitFile);
    if (!gitDir) continue;
    const resolvedGitDir = isAbsolutePath(gitDir) ? gitDir : `${root}/${gitDir}`;
    const rev = await readGitHeadShortRevision(runtime, resolvedGitDir);
    if (rev) return rev;
  }

  return null;
};

const renderUbqLogo = (revision: string | null): string => {
  const revLabel = revision ? `${revision}` : "";
  return `⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣤⣾⣷⣤⣀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⣀⣴⣾⡿⠛⠉⠉⠛⢿⣷⣦⣀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⣠⣴⣿⠿⠛⠁⠀⠀⠀⠀⠀⠀⠈⠛⠿⣿⣦⣄⠀⠀⠀
⠀⢰⣿⣿⣿⡁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢈⣿⣿⣿⡆⠀
⠀⢸⣿⡟⠻⢿⣷⡀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣾⡿⠟⢻⣿⡇⠀
⠀⢸⣿⡇⠀⠀⢻⣷⠀⠀⠀⠀⠀⠀⠀⠀⣾⡟⠀⠀⢸⣿⡇⠀ai.ubq.fi
⠀⢸⣿⡇⠀⠀⠀⣿⣇⠀⠀⠀⠀⠀⠀⣸⣿⠀⠀⠀⢸⣿⡇⠀
⠀⢸⣿⡇⠀⠀⠀⢸⣿⡀⠀⠀⠀⠀⢀⣿⡇⠀⠀⠀⢸⣿⡇⠀${revLabel}
⠀⢸⣿⡇⠀⠀⠀⠀⢻⣷⣄⣀⣀⣠⣾⡟⠀⠀⠀⠀⢸⣿⡇⠀
⠀⢸⣿⣧⣀⠀⠀⠀⠀⠙⠛⠿⠿⠛⠋⠀⠀⠀⠀⣀⣼⣿⡇⠀
⠀⠀⠈⠛⠿⣷⣦⣀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣴⣾⠿⠛⠁⠀⠀
⠀⠀⠀⠀⠀⠈⠙⠿⣷⣦⣄⠀⠀⣠⣴⣾⠿⠋⠁⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠙⠻⣿⣿⠟⠋⠀⠀⠀⠀⠀⠀⠀⠀⠀`;
};

const usageText = async (runtime: UbqAiRuntime): Promise<string> =>
  `${renderUbqLogo(await gitShortRevision(runtime))}

Usage:
  deno run --allow-env --allow-net --allow-read scripts/ubq-ai.ts [--url <base>] <command> [options]

Global options:
  --url <url>                 Base URL (default: https://ai.ubq.fi)
  --token <token>             Client token (or set UOS_AI_TOKEN; falls back to admin token if unset)
  --admin-token <token>       Admin token (or set DENO_DEPLOY_TOKEN)
  --json                      Print full JSON (default prints text when possible)
  --stream                    Stream output (when supported)
  --raw                       For streams: print raw SSE bytes (no parsing)
  -v, --verbose               Print debug info (no secrets)
  -h, --help                  Show help

Commands:
  health
  info
  whoami
  models
  chat [<prompt>] [--model <id>] [--reasoning-effort <level>] [--system <text>] [--developer <text>] [--messages-json <json>] [--messages-file <path>]
  responses [<input>] [--model <id>] [--reasoning-effort <level>] [--instructions <text>] [--input-json <json>] [--input-file <path>]
  admin upload-auth [--auth-json <path>] [--codex-bin <path>]
  admin keys create "<name>" [--token <token>] [--expires <preset>|--expires-at-ms <ms>] [--usage-limit <requests>]
  admin keys list
  admin keys revoke --id <id>
  admin kernel-pubkeys list
  admin kernel-pubkeys add --app-id <id> --pem "<pem>" [--owner <name>]
  admin kernel-pubkeys remove --app-id <id>
  admin kernel-usage get --owner <name> [--repo <name>] [--scope repo|org]
  admin kernel-usage set --owner <name> [--repo <name>] --usage-limit <requests> [--window-ms <ms>] [--scope repo|org] [--reset-usage]

Admin notes:
  upload-auth extracts Codex models from the local codex binary.

Admin key expiration:
  --expires <preset>           day|week|month|quarter|year|forever (sets expires_at_ms)
  --expires-at-ms <ms>         Unix epoch ms timestamp; -1 means does not expire

Examples:
  UOS_AI_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts chat --system \"You are a helpful assistant.\" \"Tell me a short joke.\"
  UOS_AI_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts chat --system \"You are a helpful assistant.\" --stream \"Say hello in 5 different ways.\"
  DENO_DEPLOY_TOKEN=... deno run --allow-env --allow-net --allow-read scripts/ubq-ai.ts admin upload-auth
  DENO_DEPLOY_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts admin keys create \"example key\"
  DENO_DEPLOY_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts admin keys create \"tmp key\" --expires week
  UOS_AI_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts whoami | jq

`;

export type UbqAiRuntime = Readonly<{
  fetch: (req: Request) => Promise<Response>;
  envGet: (key: string) => string | undefined;
  readTextFile: (path: string) => Promise<string>;
  stdinIsTerminal: boolean;
  readStdin: () => Promise<string>;
  out: (chunk: Uint8Array) => Promise<void>;
  err: (chunk: Uint8Array) => Promise<void>;
}>;

const writeOutText = async (runtime: UbqAiRuntime, text: string): Promise<void> => {
  await runtime.out(TEXT_ENCODER.encode(text));
};

const writeErrText = async (runtime: UbqAiRuntime, text: string): Promise<void> => {
  await runtime.err(TEXT_ENCODER.encode(text));
};

const readStdin = async (): Promise<string> => {
  const chunks: Uint8Array[] = [];
  const reader = Deno.stdin.readable.getReader();
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) chunks.push(value);
    }
  } finally {
    try {
      reader.releaseLock();
    } catch {
      // ignore
    }
  }
  const total = chunks.reduce((sum, c) => sum + c.byteLength, 0);
  const merged = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    merged.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return new TextDecoder().decode(merged).trim();
};

const doFetch = async (
  runtime: UbqAiRuntime,
  req: Request,
): Promise<
  { ok: true; status: number; contentType: string; json: unknown; headers: Headers } | {
    ok: false;
    status: number;
    contentType: string;
    body: string;
    headers: Headers;
  }
> => {
  const res = await runtime.fetch(req);
  const contentType = res.headers.get("Content-Type") ?? "";
  const isJson = contentType.includes("application/json");
  if (res.ok) {
    const json = isJson ? await res.json().catch(() => null) : await res.text().catch(() => "");
    return { ok: true, status: res.status, contentType, json, headers: res.headers };
  }
  const body = isJson ? JSON.stringify(await res.json().catch(() => null), null, 2) : await res.text().catch(() => "");
  return { ok: false, status: res.status, contentType, body: body || res.statusText, headers: res.headers };
};

const streamToOut = async (runtime: UbqAiRuntime, body: ReadableStream<Uint8Array>): Promise<void> => {
  const reader = body.getReader();
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) await runtime.out(value);
    }
  } finally {
    try {
      await reader.cancel();
    } catch {
      // ignore
    }
  }
};

const parseSseEvents = async function* (stream: ReadableStream<Uint8Array>): AsyncGenerator<unknown | "[DONE]"> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      const parts = buffer.split("\n\n");
      buffer = parts.pop() ?? "";
      for (const part of parts) {
        if (!part.trim()) continue;
        const lines = part.split("\n");
        const dataLines = lines.filter((line) => line.startsWith("data:"));
        const data = dataLines.map((line) => line.slice(5).trim()).join("\n");
        if (!data) continue;
        if (data === "[DONE]") {
          yield "[DONE]";
          continue;
        }
        try {
          yield JSON.parse(data);
        } catch {
          continue;
        }
      }
    }
  } finally {
    try {
      await reader.cancel();
    } catch {
      // ignore
    }
  }
};

const extractChatDelta = (ev: unknown): string => {
  if (!isRecord(ev)) return "";
  const choices = Array.isArray(ev.choices) ? ev.choices : null;
  if (!choices || choices.length === 0) return "";
  const choice0 = isRecord(choices[0]) ? choices[0] : null;
  if (!choice0) return "";
  const delta = isRecord(choice0.delta) ? choice0.delta : null;
  if (!delta) return "";
  const content = getString(delta.content);
  return content ?? "";
};

const extractResponseDelta = (ev: unknown): string => {
  if (!isRecord(ev)) return "";
  const type = getString(ev.type);
  if (type !== "response.output_text.delta") return "";
  return getString(ev.delta) ?? getString(ev.text_delta) ?? "";
};

const extractChatContent = (json: unknown): string | null => {
  if (!isRecord(json)) return null;
  const choices = Array.isArray(json.choices) ? json.choices : null;
  if (!choices || choices.length === 0) return null;
  const choice0 = isRecord(choices[0]) ? choices[0] : null;
  if (!choice0) return null;
  const message = isRecord(choice0.message) ? choice0.message : null;
  if (!message) return null;
  const content = getString(message.content);
  return content;
};

const extractResponseText = (json: unknown): string | null => {
  if (!isRecord(json)) return null;
  const output = Array.isArray(json.output) ? json.output : null;
  if (!output) return null;
  const messages: string[] = [];
  for (const item of output) {
    if (!isRecord(item)) continue;
    if (getString(item.type) !== "message") continue;
    if (getString(item.role) !== "assistant") continue;
    const content = Array.isArray(item.content) ? item.content : null;
    if (!content) continue;
    const parts: string[] = [];
    for (const part of content) {
      if (!isRecord(part)) continue;
      const partType = getString(part.type);
      if (partType !== "output_text" && partType !== "text") continue;
      const text = getString(part.text);
      if (text) parts.push(text);
    }
    if (parts.length > 0) messages.push(parts.join(""));
  }
  if (messages.length === 0) return null;
  return messages.join("\n");
};

const normalizeBaseUrl = (raw: string): string => raw.trim().replace(/\/$/, "") || "https://ai.ubq.fi";

const getFlagString = (flags: Record<string, FlagValue>, key: string): string | null => {
  const value = flags[key];
  if (typeof value === "string") return value;
  if (Array.isArray(value)) return value[value.length - 1] ?? null;
  return null;
};

type ApiKeyExpiryPreset = "day" | "week" | "month" | "quarter" | "year" | "forever";

const normalizeApiKeyExpiryPreset = (raw: string): ApiKeyExpiryPreset | null => {
  const normalized = raw.trim().toLowerCase().replace(/[\s_-]+/g, "");
  if (!normalized) return null;

  if (normalized === "day" || normalized === "1d" || normalized === "1day" || normalized === "oneday") return "day";
  if (normalized === "week" || normalized === "1w" || normalized === "1week" || normalized === "oneweek") return "week";
  if (normalized === "month" || normalized === "1m" || normalized === "1month" || normalized === "onemonth") {
    return "month";
  }
  if (normalized === "quarter" || normalized === "1q" || normalized === "1quarter" || normalized === "onequarter") {
    return "quarter";
  }
  if (normalized === "year" || normalized === "1y" || normalized === "1year" || normalized === "oneyear") return "year";
  if (
    normalized === "forever" || normalized === "never" || normalized === "noexpiry" || normalized === "noexpiration"
  ) {
    return "forever";
  }
  return null;
};

const apiKeyExpiresAtMsFromPreset = (preset: ApiKeyExpiryPreset, nowMs: number): number => {
  if (preset === "forever") return -1;
  const HOUR_MS = 60 * 60_000;
  const DAY_MS = 24 * HOUR_MS;
  const durations: Record<Exclude<ApiKeyExpiryPreset, "forever">, number> = {
    day: DAY_MS,
    week: 7 * DAY_MS,
    month: 30 * DAY_MS,
    quarter: 90 * DAY_MS,
    year: 365 * DAY_MS,
  };
  return nowMs + durations[preset];
};

const parseApiKeyExpiresAtMs = (
  flags: Record<string, FlagValue>,
): { ok: true; value: number | undefined } | { ok: false; message: string } => {
  const rawExpiresAtMs = getFlagString(flags, "expires-at-ms");
  const rawPreset = getFlagString(flags, "expires");
  if (rawExpiresAtMs && rawPreset) {
    return { ok: false, message: "Pass only one of --expires-at-ms or --expires." };
  }

  if (rawExpiresAtMs) {
    const trimmed = rawExpiresAtMs.trim();
    if (!trimmed) return { ok: false, message: "--expires-at-ms must be a number (Unix epoch ms) or -1." };
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) return { ok: false, message: "--expires-at-ms must be a finite number." };
    const expiresAtMs = Math.trunc(parsed);
    if (expiresAtMs === -1) return { ok: true, value: -1 };
    if (expiresAtMs < 0) return { ok: false, message: "--expires-at-ms must be -1 or a future timestamp." };
    if (expiresAtMs <= Date.now()) return { ok: false, message: "--expires-at-ms must be in the future (or -1)." };
    return { ok: true, value: expiresAtMs };
  }

  if (rawPreset) {
    const preset = normalizeApiKeyExpiryPreset(rawPreset);
    if (!preset) {
      return { ok: false, message: "--expires must be one of: day, week, month, quarter, year, forever." };
    }
    return { ok: true, value: apiKeyExpiresAtMsFromPreset(preset, Date.now()) };
  }

  return { ok: true, value: undefined };
};

const resolveClientToken = (flags: Record<string, FlagValue>, runtime: UbqAiRuntime): string | null => {
  const fromFlag = getFlagString(flags, "token");
  const fromEnv = runtime.envGet("UOS_AI_TOKEN") ?? "";
  const token = (fromFlag ?? fromEnv).trim();
  if (token) return token;
  return resolveAdminToken(flags, runtime);
};

const resolveAdminToken = (flags: Record<string, FlagValue>, runtime: UbqAiRuntime): string | null => {
  const fromFlag = getFlagString(flags, "admin-token");
  const fromEnv = runtime.envGet("DENO_DEPLOY_TOKEN") ?? "";
  const token = (fromFlag ?? fromEnv).trim();
  return token || null;
};

export const runUbqAi = async (argv: string[], runtime: UbqAiRuntime): Promise<number> => {
  const parsed = parseArgs(argv);
  const flags = parsed.flags;

  if (flags.help === true || flags.h === true || parsed._.length === 0) {
    await writeOutText(runtime, await usageText(runtime));
    return parsed._.length === 0 ? 2 : 0;
  }

  const baseUrl = normalizeBaseUrl(getFlagString(flags, "url") ?? "https://ai.ubq.fi");
  const homeDir = runtime.envGet("HOME");
  const wantsJson = flags.json === true;
  const wantsStream = flags.stream === true;
  const wantsRaw = flags.raw === true;
  const wantsVerbose = flags.verbose === true;

  const debug = async (line: string): Promise<void> => {
    if (!wantsVerbose) return;
    await writeErrText(runtime, line);
  };

  await debug(`[ubq-ai] url=${baseUrl}\n`);
  await debug(
    `[ubq-ai] env UOS_AI_TOKEN=${await describeSecret(runtime.envGet("UOS_AI_TOKEN"))}\n`,
  );
  await debug(
    `[ubq-ai] env DENO_DEPLOY_TOKEN=${await describeSecret(runtime.envGet("DENO_DEPLOY_TOKEN"))}\n`,
  );
  await debug(`[ubq-ai] env DENO_DEPLOY_TOKEN=${await describeSecret(runtime.envGet("DENO_DEPLOY_TOKEN"))}\n`);
  const clientSource = getFlagString(flags, "token")
    ? "--token"
    : (runtime.envGet("UOS_AI_TOKEN") ?? "").trim()
    ? "UOS_AI_TOKEN"
    : resolveAdminToken(flags, runtime)
    ? "(admin fallback)"
    : "(unset)";
  const adminSource = getFlagString(flags, "admin-token")
    ? "--admin-token"
    : (runtime.envGet("DENO_DEPLOY_TOKEN") ?? "").trim()
    ? "DENO_DEPLOY_TOKEN"
    : "(unset)";
  await debug(`[ubq-ai] token_sources client=${clientSource} admin=${adminSource}\n`);
  const resolvedClientToken = resolveClientToken(flags, runtime) ?? undefined;
  const resolvedAdminToken = resolveAdminToken(flags, runtime) ?? undefined;
  await debug(`[ubq-ai] resolved client_token=${await describeSecret(resolvedClientToken)}\n`);
  await debug(`[ubq-ai] resolved admin_token=${await describeSecret(resolvedAdminToken)}\n`);
  await debug(`[ubq-ai] client_token_shape=${resolvedClientToken ? classifyToken(resolvedClientToken) : "unset"}\n`);
  await debug(`[ubq-ai] admin_token_shape=${resolvedAdminToken ? classifyToken(resolvedAdminToken) : "unset"}\n`);
  await debug(`[ubq-ai] flags json=${wantsJson} stream=${wantsStream} raw=${wantsRaw}\n`);

  const cmd = parsed._[0] ?? "";
  const rest = parsed._.slice(1);

  const endpoint = (path: string): URL => new URL(path, baseUrl);

  const doFetchWithDebug = async (req: Request): Promise<Awaited<ReturnType<typeof doFetch>>> => {
    await debug(`[ubq-ai] -> ${req.method} ${req.url}\n`);
    const result = await doFetch(runtime, req);
    await debug(`[ubq-ai] <- ${result.status}\n`);
    return result;
  };

  if (cmd === "health") {
    const req = new Request(endpoint("/health"), { method: "GET", headers: { "Accept": "application/json" } });
    const result = await doFetchWithDebug(req);
    if (!result.ok) {
      await writeErrText(runtime, `Request failed (${result.status}).\n`);
      await writeErrText(runtime, `${result.body}\n`);
      return 1;
    }
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  if (cmd === "info") {
    const req = new Request(endpoint("/"), { method: "GET", headers: { "Accept": "application/json" } });
    const result = await doFetchWithDebug(req);
    if (!result.ok) {
      await writeErrText(runtime, `Request failed (${result.status}).\n`);
      await writeErrText(runtime, `${result.body}\n`);
      return 1;
    }
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  if (cmd === "whoami") {
    const token = resolveClientToken(flags, runtime);
    if (!token) {
      await writeErrText(
        runtime,
        "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n",
      );
      return 2;
    }
    const req = new Request(endpoint("/v1/auth"), {
      method: "GET",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Accept": "application/json",
      },
    });
    const result = await doFetchWithDebug(req);
    if (!result.ok) {
      await writeErrText(runtime, `Request failed (${result.status}).\n`);
      await writeErrText(runtime, `${result.body}\n`);
      return 1;
    }
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  if (cmd === "models") {
    const token = resolveClientToken(flags, runtime);
    if (!token) {
      await writeErrText(
        runtime,
        "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n",
      );
      return 2;
    }
    const req = new Request(endpoint("/v1/models"), {
      method: "GET",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Accept": "application/json",
      },
    });
    const result = await doFetchWithDebug(req);
    if (!result.ok) {
      await writeErrText(runtime, `Request failed (${result.status}).\n`);
      await writeErrText(runtime, `${result.body}\n`);
      return 1;
    }
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  if (cmd === "chat") {
    const token = resolveClientToken(flags, runtime);
    if (!token) {
      await writeErrText(
        runtime,
        "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n",
      );
      return 2;
    }
    const model = (getFlagString(flags, "model") ?? "gpt-5.2-chat-latest").trim() || "gpt-5.2-chat-latest";

    const messagesFromJson = (raw: string): unknown | null => {
      try {
        return JSON.parse(raw);
      } catch {
        return null;
      }
    };

    let messages: unknown;
    const messagesJson = getFlagString(flags, "messages-json");
    const messagesFile = getFlagString(flags, "messages-file");
    if (typeof messagesJson === "string") {
      const parsed = messagesFromJson(messagesJson);
      if (parsed === null) {
        await writeErrText(runtime, "Invalid JSON in --messages-json\n");
        return 2;
      }
      messages = parsed;
    } else if (typeof messagesFile === "string") {
      const path = expandTilde(messagesFile, homeDir);
      let text: string;
      try {
        text = await runtime.readTextFile(path);
      } catch (error) {
        await writeErrText(runtime, `Failed to read messages file: ${path}\n`);
        await writeErrText(runtime, `${error}\n`);
        return 2;
      }
      const parsed = messagesFromJson(text);
      if (parsed === null) {
        await writeErrText(runtime, `Invalid JSON in messages file: ${path}\n`);
        return 2;
      }
      messages = parsed;
    } else {
      const system = getFlagString(flags, "system") ?? "";
      const developer = getFlagString(flags, "developer") ?? "";
      let prompt = rest.join(" ").trim();
      if (!prompt && !runtime.stdinIsTerminal) {
        prompt = (await runtime.readStdin()).trim();
      }
      if (!prompt) {
        await writeErrText(runtime, "Missing prompt. Pass it as an argument, or pipe via stdin.\n");
        return 2;
      }

      const m: Array<{ role: string; content: string }> = [];
      if (system.trim()) m.push({ role: "system", content: system });
      if (developer.trim()) m.push({ role: "developer", content: developer });
      m.push({ role: "user", content: prompt });
      messages = m;
    }

    const body: Record<string, unknown> = {
      model,
      messages,
      stream: wantsStream,
    };

    const reasoningEffortRaw = (getFlagString(flags, "reasoning-effort") ?? "").trim();
    if (reasoningEffortRaw) {
      const normalized = reasoningEffortRaw.toLowerCase();
      if (!REASONING_EFFORTS.has(normalized)) {
        await writeErrText(
          runtime,
          "Invalid --reasoning-effort. Expected one of: none, minimal, low, medium, high, xhigh.\n",
        );
        return 2;
      }
      body.reasoning_effort = normalized;
    }

    const req = new Request(endpoint("/v1/chat/completions"), {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Content-Type": "application/json",
        "Accept": wantsStream ? "text/event-stream" : "application/json",
      },
      body: JSON.stringify(body),
    });

    if (wantsStream) {
      await debug(`[ubq-ai] -> ${req.method} ${req.url} (stream)\n`);
      const res = await runtime.fetch(req);
      await debug(`[ubq-ai] <- ${res.status}\n`);
      if (!res.ok) {
        const text = await res.text().catch(() => "");
        await writeErrText(runtime, `Request failed (${res.status}).\n`);
        await writeErrText(runtime, `${text || res.statusText}\n`);
        return 1;
      }
      if (!res.body) {
        await writeErrText(runtime, "Stream response missing body.\n");
        return 1;
      }

      if (wantsRaw) {
        await streamToOut(runtime, res.body);
        return 0;
      }

      for await (const ev of parseSseEvents(res.body)) {
        if (ev === "[DONE]") break;
        if (wantsJson) {
          await writeOutText(runtime, `${JSON.stringify(ev)}\n`);
          continue;
        }
        const delta = extractChatDelta(ev);
        if (delta) await runtime.out(TEXT_ENCODER.encode(delta));
      }
      if (!wantsJson) await runtime.out(TEXT_ENCODER.encode("\n"));
      return 0;
    }

    const result = await doFetchWithDebug(req);
    if (!result.ok) {
      await writeErrText(runtime, `Request failed (${result.status}).\n`);
      await writeErrText(runtime, `${result.body}\n`);
      return 1;
    }

    if (wantsJson) {
      await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
      return 0;
    }

    const content = extractChatContent(result.json);
    if (content !== null) {
      await writeOutText(runtime, `${content}\n`);
      return 0;
    }
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  if (cmd === "responses") {
    const token = resolveClientToken(flags, runtime);
    if (!token) {
      await writeErrText(
        runtime,
        "Missing client token. Set UOS_AI_TOKEN (or DENO_DEPLOY_TOKEN) or pass --token/--admin-token.\n",
      );
      return 2;
    }
    const model = (getFlagString(flags, "model") ?? "gpt-5.2-chat-latest").trim() || "gpt-5.2-chat-latest";
    const instructionsRaw = getFlagString(flags, "instructions");
    const instructions = typeof instructionsRaw === "string" ? instructionsRaw.trim() : "";

    const inputFromJson = (raw: string): unknown | null => {
      try {
        return JSON.parse(raw);
      } catch {
        return null;
      }
    };

    let input: unknown;
    const inputJson = getFlagString(flags, "input-json");
    const inputFile = getFlagString(flags, "input-file");
    if (typeof inputJson === "string") {
      const parsed = inputFromJson(inputJson);
      if (parsed === null) {
        await writeErrText(runtime, "Invalid JSON in --input-json\n");
        return 2;
      }
      input = parsed;
    } else if (typeof inputFile === "string") {
      const path = expandTilde(inputFile, homeDir);
      let text: string;
      try {
        text = await runtime.readTextFile(path);
      } catch (error) {
        await writeErrText(runtime, `Failed to read input file: ${path}\n`);
        await writeErrText(runtime, `${error}\n`);
        return 2;
      }
      const parsed = inputFromJson(text);
      if (parsed === null) {
        await writeErrText(runtime, `Invalid JSON in input file: ${path}\n`);
        return 2;
      }
      input = parsed;
    } else {
      let text = rest.join(" ").trim();
      if (!text && !runtime.stdinIsTerminal) {
        text = (await runtime.readStdin()).trim();
      }
      if (!text) {
        await writeErrText(runtime, "Missing input. Pass it as an argument, or pipe via stdin.\n");
        return 2;
      }
      input = text;
    }

    const body: Record<string, unknown> = {
      model,
      input,
      stream: wantsStream,
    };
    if (instructionsRaw !== undefined) {
      body.instructions = instructions;
    }

    const reasoningEffortRaw = (getFlagString(flags, "reasoning-effort") ?? "").trim();
    if (reasoningEffortRaw) {
      const normalized = reasoningEffortRaw.toLowerCase();
      if (!REASONING_EFFORTS.has(normalized)) {
        await writeErrText(
          runtime,
          "Invalid --reasoning-effort. Expected one of: none, minimal, low, medium, high, xhigh.\n",
        );
        return 2;
      }
      body.reasoning = { effort: normalized };
    }

    const req = new Request(endpoint("/v1/responses"), {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Content-Type": "application/json",
        "Accept": wantsStream ? "text/event-stream" : "application/json",
      },
      body: JSON.stringify(body),
    });

    if (wantsStream) {
      await debug(`[ubq-ai] -> ${req.method} ${req.url} (stream)\n`);
      const res = await runtime.fetch(req);
      await debug(`[ubq-ai] <- ${res.status}\n`);
      if (!res.ok) {
        const text = await res.text().catch(() => "");
        await writeErrText(runtime, `Request failed (${res.status}).\n`);
        await writeErrText(runtime, `${text || res.statusText}\n`);
        return 1;
      }
      if (!res.body) {
        await writeErrText(runtime, "Stream response missing body.\n");
        return 1;
      }

      if (wantsRaw) {
        await streamToOut(runtime, res.body);
        return 0;
      }

      for await (const ev of parseSseEvents(res.body)) {
        if (ev === "[DONE]") break;
        if (wantsJson) {
          await writeOutText(runtime, `${JSON.stringify(ev)}\n`);
          continue;
        }
        if (isRecord(ev) && getString(ev.type) === "response.completed") break;
        const delta = extractResponseDelta(ev);
        if (delta) await runtime.out(TEXT_ENCODER.encode(delta));
      }
      if (!wantsJson) await runtime.out(TEXT_ENCODER.encode("\n"));
      return 0;
    }

    const result = await doFetchWithDebug(req);
    if (!result.ok) {
      await writeErrText(runtime, `Request failed (${result.status}).\n`);
      await writeErrText(runtime, `${result.body}\n`);
      return 1;
    }

    if (wantsJson) {
      await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
      return 0;
    }

    const text = extractResponseText(result.json);
    if (text !== null) {
      await writeOutText(runtime, `${text}\n`);
      return 0;
    }
    await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
    return 0;
  }

  if (cmd === "admin") {
    const adminToken = resolveAdminToken(flags, runtime);
    if (!adminToken) {
      await writeErrText(
        runtime,
        "Missing admin token. Set DENO_DEPLOY_TOKEN or pass --admin-token.\n",
      );
      return 2;
    }

    const sub = rest[0] ?? "";
    const subRest = rest.slice(1);

    if (sub === "upload-auth") {
      const authJsonPath = expandTilde(getFlagString(flags, "auth-json") ?? "~/.codex/auth.json", homeDir);
      let authJsonText: string;
      try {
        authJsonText = await runtime.readTextFile(authJsonPath);
      } catch (error) {
        await writeErrText(runtime, `Failed to read auth.json at ${authJsonPath}:\n`);
        await writeErrText(runtime, `${error}\n`);
        return 2;
      }

      let authJson: unknown;
      try {
        authJson = JSON.parse(authJsonText) as unknown;
      } catch (error) {
        await writeErrText(runtime, `auth.json at ${authJsonPath} is not valid JSON:\n`);
        await writeErrText(runtime, `${error}\n`);
        return 2;
      }

      if (flags["skip-models"] !== undefined || flags["no-models"] !== undefined) {
        await writeErrText(runtime, "--skip-models is no longer supported; Codex model extraction is required.\n");
        return 2;
      }
      const codexBinFlag = getFlagString(flags, "codex-bin");
      let modelsPayload: Record<string, unknown> | null = null;
      const binary = await loadCodexBinaryText(runtime, codexBinFlag, homeDir);
      if (!binary) {
        await writeErrText(
          runtime,
          "Codex binary with model metadata not found on PATH. Pass --codex-bin to the real Codex binary.\n",
        );
        return 2;
      }
      const extractedModels = binary.models;
      const fallbackVersion = await resolveCodexClientVersion(runtime, [binary.sourcePath, binary.path], homeDir);
      const clientVersion = extractedModels.clientVersion ?? fallbackVersion ?? undefined;
      modelsPayload = {
        source: "codex_cli",
        client_version: clientVersion,
        updated_at_ms: Date.now(),
        models: extractedModels.models,
      };

      const payload = { auth: authJson, models: modelsPayload };
      const req = new Request(endpoint("/admin/codex/auth"), {
        method: "POST",
        headers: {
          "Authorization": `Bearer ${adminToken}`,
          "Content-Type": "application/json",
          "Accept": "application/json",
        },
        body: JSON.stringify(payload),
      });

      const result = await doFetchWithDebug(req);
      if (!result.ok) {
        await writeErrText(runtime, `Request failed (${result.status}).\n`);
        await writeErrText(runtime, `${result.body}\n`);
        return 1;
      }
      await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
      return 0;
    }

    if (sub === "keys") {
      const action = subRest[0] ?? "";

      if (action === "create") {
        const positionalName = subRest.slice(1).join(" ").trim();
        const name = (getFlagString(flags, "name") ?? positionalName).trim();
        if (!name.trim()) {
          await writeErrText(runtime, "Missing key name. Pass it as an argument or via --name.\n");
          return 2;
        }
        const token = (getFlagString(flags, "token") ?? "").trim();
        const tokenOnlyFlag = flags["token-only"];
        const tokenOnly = !wantsJson && tokenOnlyFlag !== false;

        const expiresAt = parseApiKeyExpiresAtMs(flags);
        if (!expiresAt.ok) {
          await writeErrText(runtime, `${expiresAt.message}\n`);
          return 2;
        }

        const usageLimitRaw = getFlagString(flags, "usage-limit");
        let usageLimit: number | undefined;
        if (usageLimitRaw) {
          const trimmed = usageLimitRaw.trim();
          if (trimmed === "unlimited" || trimmed === "-1") {
            usageLimit = -1;
          } else {
            const parsed = Number(trimmed);
            if (!Number.isFinite(parsed) || parsed < 0) {
              await writeErrText(runtime, "--usage-limit must be a positive number, -1, or 'unlimited'\n");
              return 2;
            }
            usageLimit = Math.trunc(parsed);
          }
        }

        const body: Record<string, unknown> = token ? { name, token } : { name };
        if (expiresAt.value !== undefined) body.expires_at_ms = expiresAt.value;
        if (usageLimit !== undefined) body.usage_limit_requests = usageLimit;

        const req = new Request(endpoint("/admin/api-keys"), {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Content-Type": "application/json",
            "Accept": "application/json",
          },
          body: JSON.stringify(body),
        });

        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }

        if (tokenOnly) {
          const tokenValue = (result.json && typeof result.json === "object" && "token" in result.json)
            ? (result.json as { token?: unknown }).token
            : null;
          if (typeof tokenValue === "string" && tokenValue.trim()) {
            await writeOutText(runtime, `${tokenValue}\n`);
            return 0;
          }
          await writeErrText(runtime, "Create succeeded but response was missing token.\n");
          return 1;
        }

        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      if (action === "list") {
        const req = new Request(endpoint("/admin/api-keys"), {
          method: "GET",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Accept": "application/json",
          },
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      if (action === "revoke") {
        const id = (getFlagString(flags, "id") ?? "").trim();
        if (!id) {
          await writeErrText(runtime, "Missing --id\n");
          return 2;
        }
        const req = new Request(endpoint("/admin/api-keys/revoke"), {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Content-Type": "application/json",
            "Accept": "application/json",
          },
          body: JSON.stringify({ id }),
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      await writeErrText(runtime, `Unknown admin keys command: ${action || "(missing)"}\n`);
      await writeOutText(runtime, await usageText(runtime));
      return 2;
    }

    if (sub === "kernel-pubkeys") {
      const action = subRest[0] ?? "";

      if (action === "list") {
        const req = new Request(endpoint("/admin/kernel-pubkeys"), {
          method: "GET",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Accept": "application/json",
          },
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      if (action === "add") {
        const appIdRaw = getFlagString(flags, "app-id");
        if (!appIdRaw) {
          await writeErrText(runtime, "Missing --app-id\n");
          return 2;
        }
        const appId = parseInt(appIdRaw, 10);
        if (isNaN(appId)) {
          await writeErrText(runtime, "--app-id must be a number\n");
          return 2;
        }

        let pem = getFlagString(flags, "pem");
        if (!pem && !runtime.stdinIsTerminal) {
          pem = (await runtime.readStdin()).trim();
        }
        if (!pem) {
          await writeErrText(runtime, "Missing --pem. Pass it as a flag or pipe via stdin.\n");
          return 2;
        }

        const owner = getFlagString(flags, "owner") ?? "cli";

        const req = new Request(endpoint("/admin/kernel-pubkeys"), {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Content-Type": "application/json",
            "Accept": "application/json",
          },
          body: JSON.stringify({ app_id: appId, pem, owner }),
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      if (action === "remove") {
        const appIdRaw = getFlagString(flags, "app-id");
        if (!appIdRaw) {
          await writeErrText(runtime, "Missing --app-id\n");
          return 2;
        }
        const appId = parseInt(appIdRaw, 10);
        if (isNaN(appId)) {
          await writeErrText(runtime, "--app-id must be a number\n");
          return 2;
        }

        const url = endpoint("/admin/kernel-pubkeys");
        url.searchParams.set("app_id", String(appId));

        const req = new Request(url, {
          method: "DELETE",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Accept": "application/json",
          },
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      await writeErrText(runtime, `Unknown admin kernel-pubkeys command: ${action || "(missing)"}\n`);
      await writeOutText(runtime, await usageText(runtime));
      return 2;
    }

    if (sub === "kernel-usage") {
      const action = subRest[0] ?? "";

      if (action === "get") {
        const owner = (getFlagString(flags, "owner") ?? "").trim();
        const repo = (getFlagString(flags, "repo") ?? "").trim();
        const scopeRaw = (getFlagString(flags, "scope") ?? "repo").trim().toLowerCase();
        const scope = scopeRaw === "org" ? "org" : "repo";
        if (!owner) {
          await writeErrText(runtime, "Missing --owner\n");
          return 2;
        }
        if (scope === "repo" && !repo) {
          await writeErrText(runtime, "Missing --repo for scope=repo\n");
          return 2;
        }
        if (scope === "org" && repo) {
          await writeErrText(runtime, "--repo must be omitted for scope=org\n");
          return 2;
        }

        const url = endpoint("/admin/kernel-usage");
        url.searchParams.set("owner", owner);
        url.searchParams.set("scope", scope);
        if (scope === "repo") url.searchParams.set("repo", repo);

        const req = new Request(url, {
          method: "GET",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Accept": "application/json",
          },
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      if (action === "set") {
        const owner = (getFlagString(flags, "owner") ?? "").trim();
        const repo = (getFlagString(flags, "repo") ?? "").trim();
        const scopeRaw = (getFlagString(flags, "scope") ?? (repo ? "repo" : "org")).trim().toLowerCase();
        const scope = scopeRaw === "org" ? "org" : "repo";
        if (!owner) {
          await writeErrText(runtime, "Missing --owner\n");
          return 2;
        }
        if (scope === "repo" && !repo) {
          await writeErrText(runtime, "Missing --repo for scope=repo\n");
          return 2;
        }
        if (scope === "org" && repo) {
          await writeErrText(runtime, "--repo must be omitted for scope=org\n");
          return 2;
        }

        const usageLimitRaw = getFlagString(flags, "usage-limit");
        if (!usageLimitRaw) {
          await writeErrText(runtime, "Missing --usage-limit\n");
          return 2;
        }
        const trimmed = usageLimitRaw.trim();
        let usageLimit: number;
        if (trimmed === "unlimited" || trimmed === "-1") {
          usageLimit = -1;
        } else {
          const parsed = Number(trimmed);
          if (!Number.isFinite(parsed) || parsed < 0) {
            await writeErrText(runtime, "--usage-limit must be a non-negative number, -1, or 'unlimited'\n");
            return 2;
          }
          usageLimit = Math.trunc(parsed);
        }

        const resetUsage = flags["reset-usage"] === true;
        const windowMsRaw = getFlagString(flags, "window-ms");
        let windowMs: number | undefined;
        if (windowMsRaw) {
          const parsed = Number(windowMsRaw.trim());
          if (!Number.isFinite(parsed) || parsed <= 0) {
            await writeErrText(runtime, "--window-ms must be a positive number\n");
            return 2;
          }
          windowMs = Math.trunc(parsed);
        }

        const body: Record<string, unknown> = {
          owner,
          usage_limit_requests: usageLimit,
          reset_usage: resetUsage,
          scope,
        };
        if (scope === "repo") body.repo = repo;
        if (windowMs !== undefined) body.window_ms = windowMs;

        const req = new Request(endpoint("/admin/kernel-usage"), {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${adminToken}`,
            "Content-Type": "application/json",
            "Accept": "application/json",
          },
          body: JSON.stringify(body),
        });
        const result = await doFetchWithDebug(req);
        if (!result.ok) {
          await writeErrText(runtime, `Request failed (${result.status}).\n`);
          await writeErrText(runtime, `${result.body}\n`);
          return 1;
        }
        await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
        return 0;
      }

      await writeErrText(runtime, `Unknown admin kernel-usage command: ${action || "(missing)"}\n`);
      await writeOutText(runtime, await usageText(runtime));
      return 2;
    }

    await writeErrText(runtime, `Unknown admin command: ${sub || "(missing)"}\n`);
    await writeOutText(runtime, await usageText(runtime));
    return 2;
  }

  await writeErrText(runtime, `Unknown command: ${cmd}\n`);
  await writeOutText(runtime, await usageText(runtime));
  return 2;
};

const getDefaultRuntime = (): UbqAiRuntime => ({
  fetch,
  envGet: (key: string) => Deno.env.get(key),
  readTextFile: (path: string) => Deno.readTextFile(path),
  stdinIsTerminal: Deno.stdin.isTerminal(),
  readStdin,
  out: async (chunk: Uint8Array) => {
    await Deno.stdout.write(chunk);
  },
  err: async (chunk: Uint8Array) => {
    await Deno.stderr.write(chunk);
  },
});

if (import.meta.main) {
  const code = await runUbqAi(Deno.args, getDefaultRuntime());
  Deno.exit(code);
}
