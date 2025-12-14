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

const usage = () => {
  console.log(`upload-codex-auth.ts

Uploads your local Codex ~/.codex/auth.json to ai.ubq.fi for validation + storage in Deno KV.

Usage:
  deno run --allow-env --allow-net --allow-read scripts/upload-codex-auth.ts [--url https://ai.ubq.fi] [--auth-json ~/.codex/auth.json]

Auth:
  Set UBQ_AI_ADMIN_TOKEN in env, or pass --admin-token.
`);
};

if (Deno.args.includes("--help") || Deno.args.includes("-h")) {
  usage();
  Deno.exit(0);
}

const parsed = parseArgs(Deno.args);
const baseUrl = (parsed.url as string | undefined) ?? "https://ai.ubq.fi";
const authJsonPath = expandTilde((parsed["auth-json"] as string | undefined) ?? "~/.codex/auth.json");
const adminToken = (parsed["admin-token"] as string | undefined) ?? Deno.env.get("UBQ_AI_ADMIN_TOKEN") ??
  Deno.env.get("DENO_DEPLOY_TOKEN") ?? "";

if (!adminToken) {
  console.error("Missing admin token. Set UBQ_AI_ADMIN_TOKEN (or DENO_DEPLOY_TOKEN) or pass --admin-token.");
  Deno.exit(2);
}

let authJsonText: string;
try {
  authJsonText = await Deno.readTextFile(authJsonPath);
} catch (error) {
  console.error(`Failed to read auth.json at ${authJsonPath}:`, error);
  Deno.exit(2);
}

try {
  JSON.parse(authJsonText);
} catch (error) {
  console.error(`auth.json at ${authJsonPath} is not valid JSON:`, error);
  Deno.exit(2);
}

const endpoint = new URL("/admin/codex/auth", baseUrl);
const res = await fetch(endpoint, {
  method: "POST",
  headers: {
    "Authorization": `Bearer ${adminToken}`,
    "Content-Type": "application/json",
    "Accept": "application/json",
  },
  body: authJsonText,
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
