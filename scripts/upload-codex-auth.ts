import {
  extractCodexInstructionsFromText,
  extractCodexModelsFromText,
  resolveCodexBinaryPath,
} from "./codex-models.ts";

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

const findCodexBinaryOnPath = async (): Promise<string | null> => {
  const pathValue = Deno.env.get("PATH") ?? "";
  const separator = Deno.build.os === "windows" ? ";" : ":";
  const segments = pathValue.split(separator).filter(Boolean);
  for (const segment of segments) {
    const candidate = `${segment.replace(/\/$/, "")}/codex`;
    try {
      const stat = await Deno.stat(candidate);
      if (stat.isFile) return candidate;
    } catch {
      // ignore
    }
  }
  return null;
};

const usage = () => {
  console.log(`upload-codex-auth.ts

Uploads your local Codex ~/.codex/auth.json to ai.ubq.fi for validation + storage in Deno KV.
Also extracts Codex models + instructions from the local Codex binary unless --skip-models.

Usage:
  deno run --allow-env --allow-net --allow-read scripts/upload-codex-auth.ts [--url https://ai.ubq.fi] [--auth-json ~/.codex/auth.json] [--codex-bin /path/to/codex] [--skip-models]

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
const skipModels = parsed["skip-models"] === true || parsed["no-models"] === true;
const codexBinFlag = parsed["codex-bin"] as string | undefined;
let modelsPayload: Record<string, unknown> | null = null;
let instructionsPayload: Record<string, unknown> | null = null;

if (!skipModels) {
  const codexPath = codexBinFlag ? expandTilde(codexBinFlag) : await findCodexBinaryOnPath();
  if (!codexPath) {
    console.error("Codex binary not found on PATH; skipping model + instructions extraction.");
  } else {
    let resolvedPath = codexPath;
    try {
      resolvedPath = await resolveCodexBinaryPath(
        codexPath,
        (path) => Deno.readTextFile(path),
        Deno.build.os,
        Deno.build.arch,
        Deno.realPath,
      );
      const text = await Deno.readTextFile(resolvedPath);
      const extractedModels = extractCodexModelsFromText(text);
      const extractedInstructions = extractCodexInstructionsFromText(text);
      if (!extractedModels) {
        console.error("Failed to extract Codex models from the CLI binary; skipping model upload.");
      }
      if (!extractedInstructions) {
        console.error("Failed to extract Codex instructions from the CLI binary; skipping instructions upload.");
      }
      if (extractedModels || extractedInstructions) {
        const fallbackVersion = await resolveCodexClientVersion([resolvedPath, codexPath]);
        const clientVersion = extractedModels?.clientVersion ?? fallbackVersion ?? undefined;
        if (extractedModels) {
          modelsPayload = {
            source: "codex_cli",
            client_version: clientVersion,
            updated_at_ms: Date.now(),
            models: extractedModels.models,
          };
        }
        if (extractedInstructions) {
          instructionsPayload = {
            source: "codex_cli",
            client_version: clientVersion,
            updated_at_ms: Date.now(),
            instructions: extractedInstructions,
          };
        }
      }
    } catch (error) {
      console.error(`Failed to read Codex binary at ${resolvedPath}:`, error);
    }
  }
}

const endpoint = new URL("/admin/codex/auth", baseUrl);
const requestPayload = modelsPayload || instructionsPayload
  ? { auth: authJson, models: modelsPayload ?? undefined, instructions: instructionsPayload ?? undefined }
  : authJson;
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
