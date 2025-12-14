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
  console.log(`api-keys.ts

Manage ai.ubq.fi client API keys (admin-only).

Usage:
  deno run --allow-env --allow-net scripts/api-keys.ts <command> [options]

Commands:
  create --name "<label>" [--token "<exact token>"] [--token-only]
  list
  revoke --id "<id>"

Options:
  --url https://ai.ubq.fi          Base URL (default: https://ai.ubq.fi)
  --admin-token "<token>"         Admin token (or set UBQ_AI_ADMIN_TOKEN or DENO_DEPLOY_TOKEN)
`);
};

if (Deno.args.includes("--help") || Deno.args.includes("-h")) {
  usage();
  Deno.exit(0);
}

const [commandRaw, ...rest] = Deno.args;
const command = commandRaw?.trim() ?? "";
if (!command || command.startsWith("--")) {
  usage();
  Deno.exit(2);
}

const parsed = parseArgs(rest);
const baseUrl = (parsed.url as string | undefined) ?? "https://ai.ubq.fi";
const adminToken = (parsed["admin-token"] as string | undefined) ?? Deno.env.get("UBQ_AI_ADMIN_TOKEN") ??
  Deno.env.get("DENO_DEPLOY_TOKEN") ?? "";

if (!adminToken) {
  console.error("Missing admin token. Set UBQ_AI_ADMIN_TOKEN (or DENO_DEPLOY_TOKEN) or pass --admin-token.");
  Deno.exit(2);
}

const doFetch = async (
  req: Request,
): Promise<{ ok: true; json: unknown } | { ok: false; status: number; body: string }> => {
  const res = await fetch(req);
  const contentType = res.headers.get("Content-Type") ?? "";
  const isJson = contentType.includes("application/json");
  if (res.ok) {
    const json = isJson ? await res.json().catch(() => null) : await res.text().catch(() => "");
    return { ok: true, json };
  }
  const body = isJson ? JSON.stringify(await res.json().catch(() => null), null, 2) : await res.text().catch(() => "");
  return { ok: false, status: res.status, body: body || res.statusText };
};

const endpoint = (path: string): URL => new URL(path, baseUrl);

if (command === "create") {
  const name = (parsed.name as string | undefined) ?? "";
  if (!name.trim()) {
    console.error("Missing --name");
    Deno.exit(2);
  }

  const token = (parsed.token as string | undefined) ?? null;
  const req = new Request(endpoint("/admin/api-keys"), {
    method: "POST",
    headers: {
      "Authorization": `Bearer ${adminToken}`,
      "Content-Type": "application/json",
      "Accept": "application/json",
    },
    body: JSON.stringify(token ? { name, token } : { name }),
  });

  const result = await doFetch(req);
  if (!result.ok) {
    console.error(`Request failed (${result.status}).`);
    console.error(result.body);
    Deno.exit(1);
  }

  const tokenOnly = parsed["token-only"] === true;
  if (tokenOnly) {
    const tokenValue = (result.json && typeof result.json === "object" && "token" in result.json)
      ? (result.json as { token?: unknown }).token
      : null;
    console.log(typeof tokenValue === "string" ? tokenValue : "");
  } else {
    console.log(JSON.stringify(result.json, null, 2));
  }
  Deno.exit(0);
}

if (command === "list") {
  const req = new Request(endpoint("/admin/api-keys"), {
    method: "GET",
    headers: {
      "Authorization": `Bearer ${adminToken}`,
      "Accept": "application/json",
    },
  });
  const result = await doFetch(req);
  if (!result.ok) {
    console.error(`Request failed (${result.status}).`);
    console.error(result.body);
    Deno.exit(1);
  }
  console.log(JSON.stringify(result.json, null, 2));
  Deno.exit(0);
}

if (command === "revoke") {
  const id = (parsed.id as string | undefined) ?? "";
  if (!id.trim()) {
    console.error("Missing --id");
    Deno.exit(2);
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
  const result = await doFetch(req);
  if (!result.ok) {
    console.error(`Request failed (${result.status}).`);
    console.error(result.body);
    Deno.exit(1);
  }
  console.log(JSON.stringify(result.json, null, 2));
  Deno.exit(0);
}

console.error(`Unknown command: ${command}`);
usage();
Deno.exit(2);
