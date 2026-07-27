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

const usage = () => {
  console.log(`ubq-ai health check

Checks gateway readiness and exits non-zero when unhealthy.

Usage:
  deno run --allow-net --allow-env scripts/health-check.ts [--url https://ai.ubq.fi] [--json]
`);
};

if (Deno.args.includes("--help") || Deno.args.includes("-h")) {
  usage();
  Deno.exit(0);
}

const parsed = parseArgs(Deno.args);
const baseUrl = (parsed.url as string | undefined) ?? "https://ai.ubq.fi";
const wantJson = parsed.json === true;
const endpoint = new URL("/health", baseUrl);

let res: Response;
try {
  res = await fetch(endpoint, { headers: { "Accept": "application/json" }, redirect: "manual" });
} catch (error) {
  const detail = error instanceof Error ? error.message : String(error);
  console.error(`Request failed: ${detail}`);
  Deno.exit(2);
}

const contentType = res.headers.get("Content-Type") ?? "";
const isJson = contentType.includes("application/json");
const payload = isJson ? await res.json().catch(() => null) : null;

const ok = res.ok && payload?.status === "available";

if (wantJson) {
  console.log(JSON.stringify({ status: res.status, ok, payload }, null, 2));
} else if (ok) {
  console.log(`OK ${res.status} upstream=chatgpt_codex mode=readiness`);
} else {
  const errorText = payload?.error ?? res.statusText;
  console.error(`FAIL ${res.status} upstream=chatgpt_codex mode=readiness error=${errorText}`);
}

if (!ok) Deno.exit(1);
