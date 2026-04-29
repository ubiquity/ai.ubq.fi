import { extractCodexModelsFromText, resolveCodexBinaryPath } from "./codex-models.ts";

const parseArgs = (args: string[]): Record<string, string | boolean> => {
  const out: Record<string, string | boolean> = {};
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (!arg.startsWith("--")) continue;
    const eq = arg.indexOf("=");
    if (eq !== -1) {
      out[arg.slice(2, eq)] = arg.slice(eq + 1);
      continue;
    }
    const key = arg.slice(2);
    const next = args[i + 1];
    if (!next || next.startsWith("--")) {
      out[key] = true;
      continue;
    }
    out[key] = next;
    i++;
  }
  return out;
};

const expandTilde = (path: string): string => {
  if (path === "~") return Deno.env.get("HOME") ?? path;
  if (path.startsWith("~/")) {
    const home = Deno.env.get("HOME");
    if (home) return `${home}${path.slice(1)}`;
  }
  return path;
};

const normalizeVersion = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed ? trimmed : null;
};

const readCodexVersionFile = async (): Promise<string | null> => {
  const home = Deno.env.get("HOME");
  if (!home) return null;
  try {
    const text = await Deno.readTextFile(`${home}/.codex/version.json`);
    const parsed = JSON.parse(text) as Record<string, unknown>;
    return normalizeVersion(parsed.latest_version ?? parsed.version ?? parsed.current_version);
  } catch {
    return null;
  }
};

const readCodexPackageVersion = async (codexPath: string): Promise<string | null> => {
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
    const text = await Deno.readTextFile(pkgPath);
    const parsed = JSON.parse(text) as Record<string, unknown>;
    return normalizeVersion(parsed.version);
  } catch {
    return null;
  }
};

const resolveCodexClientVersion = async (paths: string[]): Promise<string | null> => {
  for (const candidate of paths) {
    const version = await readCodexPackageVersion(candidate);
    if (version) return version;
  }
  return await readCodexVersionFile();
};

const listCodexBinaryCandidates = (codexBinFlag?: string): string[] => {
  const candidates: string[] = [];
  if (codexBinFlag) candidates.push(expandTilde(codexBinFlag));
  const pathValue = Deno.env.get("PATH") ?? "";
  const separator = Deno.build.os === "windows" ? ";" : ":";
  for (const segment of pathValue.split(separator).filter(Boolean)) {
    candidates.push(`${segment.replace(/\/$/, "")}/codex`);
  }
  return candidates;
};

const readPossiblyBinaryText = async (path: string): Promise<string> => {
  try {
    return await Deno.readTextFile(path);
  } catch (error) {
    try {
      const bytes = await Deno.readFile(path);
      return new TextDecoder().decode(bytes);
    } catch {
      throw error;
    }
  }
};

const loadCodexBinaryModels = async (
  codexBinFlag?: string,
): Promise<
  | {
    path: string;
    sourcePath: string;
    models: NonNullable<ReturnType<typeof extractCodexModelsFromText>>;
  }
  | null
> => {
  const seen = new Set<string>();
  for (const candidate of listCodexBinaryCandidates(codexBinFlag)) {
    if (seen.has(candidate)) continue;
    seen.add(candidate);
    try {
      const resolved = await resolveCodexBinaryPath(
        candidate,
        Deno.readTextFile,
        Deno.build.os,
        Deno.build.arch,
        Deno.realPath,
      );
      const text = await readPossiblyBinaryText(resolved);
      const models = extractCodexModelsFromText(text);
      if (!models) continue;
      return { path: resolved, sourcePath: candidate, models };
    } catch {
      // Try the next candidate.
    }
  }
  return null;
};

const usage = () => {
  console.log(`upload-codex-auth.ts

Uploads your local Codex ~/.codex/auth.json to ai.ubq.fi for validation + storage in Deno KV.
Extracts the Codex model catalog from the local Codex CLI and uploads it as the model snapshot.

Usage:
  deno run --allow-env --allow-net --allow-read scripts/upload-codex-auth.ts [--url https://ai.ubq.fi] [--auth-json ~/.codex/auth.json] [--codex-bin /path/to/codex]

Auth:
  Set DENO_DEPLOY_TOKEN in env, or pass --admin-token.
`);
};

if (Deno.args.includes("--help") || Deno.args.includes("-h")) {
  usage();
  Deno.exit(0);
}

const parsed = parseArgs(Deno.args);
const baseUrl = (parsed.url as string | undefined) ?? "https://ai.ubq.fi";
const authJsonPath = expandTilde((parsed["auth-json"] as string | undefined) ?? "~/.codex/auth.json");
const adminToken = (parsed["admin-token"] as string | undefined) ?? Deno.env.get("DENO_DEPLOY_TOKEN") ?? "";
if (parsed["skip-models"] !== undefined || parsed["no-models"] !== undefined) {
  console.error("--skip-models is no longer supported; Codex model extraction is required.");
  Deno.exit(2);
}

if (!adminToken) {
  console.error("Missing admin token. Set DENO_DEPLOY_TOKEN or pass --admin-token.");
  Deno.exit(2);
}

let authJsonText: string;
try {
  authJsonText = await Deno.readTextFile(authJsonPath);
} catch (error) {
  console.error(`Failed to read auth.json at ${authJsonPath}:`, error);
  Deno.exit(2);
}

let authJson: unknown;
try {
  authJson = JSON.parse(authJsonText) as unknown;
} catch (error) {
  console.error(`auth.json at ${authJsonPath} is not valid JSON:`, error);
  Deno.exit(2);
}
const codexBinFlag = parsed["codex-bin"] as string | undefined;
let modelsPayload: Record<string, unknown> | null = null;

const binary = await loadCodexBinaryModels(codexBinFlag);
if (!binary) {
  console.error("Codex binary with model metadata not found on PATH. Pass --codex-bin to the real Codex binary.");
  Deno.exit(2);
}
const clientVersion = binary.models.clientVersion ??
  await resolveCodexClientVersion([binary.sourcePath, binary.path]) ??
  undefined;
modelsPayload = {
  source: "codex_cli",
  client_version: clientVersion,
  updated_at_ms: Date.now(),
  models: binary.models.models,
};

const endpoint = new URL("/admin/codex/auth", baseUrl);
const requestPayload = { auth: authJson, models: modelsPayload };
const body = JSON.stringify(requestPayload);
const res = await fetch(endpoint, {
  method: "POST",
  headers: {
    "Authorization": `Bearer ${adminToken}`,
    "Content-Type": "application/json",
    "Accept": "application/json",
  },
  body,
});

const contentType = res.headers.get("Content-Type") ?? "";
const isJson = contentType.includes("application/json");
const payload = isJson ? await res.json().catch(() => null) : await res.text().catch(() => "");

if (!res.ok) {
  console.error(`Request failed (${res.status}).`);
  if (payload) console.error(isJson ? JSON.stringify(payload, null, 2) : payload);
  Deno.exit(1);
}

if (payload) {
  console.log(isJson ? JSON.stringify(payload, null, 2) : payload);
} else {
  console.log("OK");
}
