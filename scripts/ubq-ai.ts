type FlagValue = string | boolean | string[];

type ParsedArgs = Readonly<{
  _: string[];
  flags: Record<string, FlagValue>;
}>;

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null;

const getString = (value: unknown): string | null => (typeof value === "string" ? value : null);

const expandTilde = (path: string): string => {
  if (path === "~") return Deno.env.get("HOME") ?? path;
  if (path.startsWith("~/")) {
    const home = Deno.env.get("HOME");
    if (home) return `${home}${path.slice(1)}`;
  }
  return path;
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
  "stream",
  "token-only",
]);

const parseArgs = (args: string[]): ParsedArgs => {
  const flags: Record<string, FlagValue> = {};
  const positional: string[] = [];

  for (let i = 0; i < args.length; i++) {
    const arg = args[i] ?? "";
    if (arg === "--") continue;
    if (arg === "-h") {
      flags.help = true;
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
    if (!next || next.startsWith("-")) {
      if (key) pushFlag(flags, key, true);
      continue;
    }
    if (key) pushFlag(flags, key, next);
    i++;
  }

  return { _: positional, flags };
};

const usage = () => {
  console.log(`ubq-ai.ts

Unified CLI for https://ai.ubq.fi (client + admin).

Usage:
  deno run --allow-env --allow-net --allow-read scripts/ubq-ai.ts [--url <base>] <command> [options]

Global options:
  --url <url>                 Base URL (default: https://ai.ubq.fi)
  --token <token>             Client token (or set UBQ_AI_TOKEN)
  --admin-token <token>       Admin token (or set UBQ_AI_ADMIN_TOKEN; fallback DENO_DEPLOY_TOKEN)
  --json                      Print full JSON (default prints text when possible)
  --stream                    Stream output (when supported)
  --raw                       For streams: print raw SSE bytes (no parsing)
  -h, --help                  Show help

Commands:
  health
  info
  models
  chat [<prompt>] [--model <id>] [--system <text>] [--developer <text>] [--messages-json <json>] [--messages-file <path>]
  responses [<input>] [--model <id>] [--input-json <json>] [--input-file <path>]
  admin upload-auth [--auth-json <path>]
  admin keys create --name <name> [--token <token>] [--token-only]
  admin keys list
  admin keys revoke --id <id>

Examples:
  UBQ_AI_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts chat \"Tell me a short joke.\"
  UBQ_AI_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts chat --stream \"Say hello in 5 different ways.\"
  DENO_DEPLOY_TOKEN=... deno run --allow-env --allow-net --allow-read scripts/ubq-ai.ts admin upload-auth
  DENO_DEPLOY_TOKEN=... deno run --allow-env --allow-net scripts/ubq-ai.ts admin keys create --name \"example\" --token-only
`);
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
  const res = await fetch(req);
  const contentType = res.headers.get("Content-Type") ?? "";
  const isJson = contentType.includes("application/json");
  if (res.ok) {
    const json = isJson ? await res.json().catch(() => null) : await res.text().catch(() => "");
    return { ok: true, status: res.status, contentType, json, headers: res.headers };
  }
  const body = isJson ? JSON.stringify(await res.json().catch(() => null), null, 2) : await res.text().catch(() => "");
  return { ok: false, status: res.status, contentType, body: body || res.statusText, headers: res.headers };
};

const streamToStdout = async (body: ReadableStream<Uint8Array>): Promise<void> => {
  const reader = body.getReader();
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) await Deno.stdout.write(value);
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

const requireClientToken = (token: string): string => {
  if (!token.trim()) {
    console.error("Missing client token. Set UBQ_AI_TOKEN or pass --token.");
    Deno.exit(2);
  }
  return token.trim();
};

const requireAdminToken = (token: string): string => {
  if (!token.trim()) {
    console.error("Missing admin token. Set UBQ_AI_ADMIN_TOKEN (or DENO_DEPLOY_TOKEN) or pass --admin-token.");
    Deno.exit(2);
  }
  return token.trim();
};

const main = async () => {
  const parsed = parseArgs(Deno.args);
  const flags = parsed.flags;

  if (flags.help === true || flags.h === true || parsed._.length === 0) {
    usage();
    Deno.exit(parsed._.length === 0 ? 2 : 0);
  }

  const baseUrl = normalizeBaseUrl((flags.url as string | undefined) ?? "https://ai.ubq.fi");
  const wantsJson = flags.json === true;
  const wantsStream = flags.stream === true;
  const wantsRaw = flags.raw === true;

  const cmd = parsed._[0] ?? "";
  const rest = parsed._.slice(1);

  const endpoint = (path: string): URL => new URL(path, baseUrl);

  if (cmd === "health") {
    const req = new Request(endpoint("/health"), { method: "GET", headers: { "Accept": "application/json" } });
    const result = await doFetch(req);
    if (!result.ok) {
      console.error(`Request failed (${result.status}).`);
      console.error(result.body);
      Deno.exit(1);
    }
    console.log(JSON.stringify(result.json, null, 2));
    return;
  }

  if (cmd === "info") {
    const req = new Request(endpoint("/"), { method: "GET", headers: { "Accept": "application/json" } });
    const result = await doFetch(req);
    if (!result.ok) {
      console.error(`Request failed (${result.status}).`);
      console.error(result.body);
      Deno.exit(1);
    }
    console.log(JSON.stringify(result.json, null, 2));
    return;
  }

  if (cmd === "models") {
    const token = requireClientToken((flags.token as string | undefined) ?? Deno.env.get("UBQ_AI_TOKEN") ?? "");
    const req = new Request(endpoint("/v1/models"), {
      method: "GET",
      headers: {
        "Authorization": `Bearer ${token}`,
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
    return;
  }

  if (cmd === "chat") {
    const token = requireClientToken((flags.token as string | undefined) ?? Deno.env.get("UBQ_AI_TOKEN") ?? "");
    const model = ((flags.model as string | undefined) ?? "gpt-5.2-chat-latest").trim() || "gpt-5.2-chat-latest";

    const messagesFromJson = (raw: string): unknown => {
      try {
        return JSON.parse(raw);
      } catch {
        console.error("Invalid JSON in --messages-json");
        Deno.exit(2);
      }
    };

    let messages: unknown;
    const messagesJson = flags["messages-json"];
    const messagesFile = flags["messages-file"];
    if (typeof messagesJson === "string") {
      messages = messagesFromJson(messagesJson);
    } else if (typeof messagesFile === "string") {
      const path = expandTilde(messagesFile);
      const text = await Deno.readTextFile(path).catch((err) => {
        console.error(`Failed to read messages file: ${path}`);
        console.error(err);
        Deno.exit(2);
      });
      messages = messagesFromJson(text);
    } else {
      const system = typeof flags.system === "string" ? flags.system : "";
      const developer = typeof flags.developer === "string" ? flags.developer : "";
      let prompt = rest.join(" ").trim();
      if (!prompt && !Deno.stdin.isTerminal()) {
        prompt = (await readStdin()).trim();
      }
      if (!prompt) {
        console.error("Missing prompt. Pass it as an argument, or pipe via stdin.");
        Deno.exit(2);
      }

      const m: Array<{ role: string; content: string }> = [];
      if (system.trim()) m.push({ role: "system", content: system });
      if (developer.trim()) m.push({ role: "developer", content: developer });
      m.push({ role: "user", content: prompt });
      messages = m;
    }

    const body = {
      model,
      messages,
      stream: wantsStream,
    };

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
      const res = await fetch(req);
      if (!res.ok) {
        const text = await res.text().catch(() => "");
        console.error(`Request failed (${res.status}).`);
        console.error(text || res.statusText);
        Deno.exit(1);
      }
      if (!res.body) {
        console.error("Stream response missing body.");
        Deno.exit(1);
      }

      if (wantsRaw) {
        await streamToStdout(res.body);
        return;
      }

      for await (const ev of parseSseEvents(res.body)) {
        if (ev === "[DONE]") break;
        if (wantsJson) {
          console.log(JSON.stringify(ev));
          continue;
        }
        const delta = extractChatDelta(ev);
        if (delta) await Deno.stdout.write(new TextEncoder().encode(delta));
      }
      if (!wantsJson) await Deno.stdout.write(new TextEncoder().encode("\n"));
      return;
    }

    const result = await doFetch(req);
    if (!result.ok) {
      console.error(`Request failed (${result.status}).`);
      console.error(result.body);
      Deno.exit(1);
    }

    if (wantsJson) {
      console.log(JSON.stringify(result.json, null, 2));
      return;
    }

    const content = extractChatContent(result.json);
    if (content !== null) {
      console.log(content);
      return;
    }
    console.log(JSON.stringify(result.json, null, 2));
    return;
  }

  if (cmd === "responses") {
    const token = requireClientToken((flags.token as string | undefined) ?? Deno.env.get("UBQ_AI_TOKEN") ?? "");
    const model = ((flags.model as string | undefined) ?? "gpt-5.2").trim() || "gpt-5.2";

    const inputFromJson = (raw: string): unknown => {
      try {
        return JSON.parse(raw);
      } catch {
        console.error("Invalid JSON in --input-json");
        Deno.exit(2);
      }
    };

    let input: unknown;
    const inputJson = flags["input-json"];
    const inputFile = flags["input-file"];
    if (typeof inputJson === "string") {
      input = inputFromJson(inputJson);
    } else if (typeof inputFile === "string") {
      const path = expandTilde(inputFile);
      const text = await Deno.readTextFile(path).catch((err) => {
        console.error(`Failed to read input file: ${path}`);
        console.error(err);
        Deno.exit(2);
      });
      input = inputFromJson(text);
    } else {
      let text = rest.join(" ").trim();
      if (!text && !Deno.stdin.isTerminal()) {
        text = (await readStdin()).trim();
      }
      if (!text) {
        console.error("Missing input. Pass it as an argument, or pipe via stdin.");
        Deno.exit(2);
      }
      input = text;
    }

    const body = {
      model,
      input,
      stream: wantsStream,
    };

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
      const res = await fetch(req);
      if (!res.ok) {
        const text = await res.text().catch(() => "");
        console.error(`Request failed (${res.status}).`);
        console.error(text || res.statusText);
        Deno.exit(1);
      }
      if (!res.body) {
        console.error("Stream response missing body.");
        Deno.exit(1);
      }

      if (wantsRaw) {
        await streamToStdout(res.body);
        return;
      }

      for await (const ev of parseSseEvents(res.body)) {
        if (ev === "[DONE]") break;
        if (wantsJson) {
          console.log(JSON.stringify(ev));
          continue;
        }
        const delta = extractResponseDelta(ev);
        if (delta) await Deno.stdout.write(new TextEncoder().encode(delta));
      }
      if (!wantsJson) await Deno.stdout.write(new TextEncoder().encode("\n"));
      return;
    }

    const result = await doFetch(req);
    if (!result.ok) {
      console.error(`Request failed (${result.status}).`);
      console.error(result.body);
      Deno.exit(1);
    }

    if (wantsJson) {
      console.log(JSON.stringify(result.json, null, 2));
      return;
    }

    const text = extractResponseText(result.json);
    if (text !== null) {
      console.log(text);
      return;
    }
    console.log(JSON.stringify(result.json, null, 2));
    return;
  }

  if (cmd === "admin") {
    const adminToken = requireAdminToken(
      (flags["admin-token"] as string | undefined) ?? Deno.env.get("UBQ_AI_ADMIN_TOKEN") ??
        Deno.env.get("DENO_DEPLOY_TOKEN") ??
        "",
    );

    const sub = rest[0] ?? "";
    const subRest = rest.slice(1);

    if (sub === "upload-auth") {
      const authJsonPath = expandTilde((flags["auth-json"] as string | undefined) ?? "~/.codex/auth.json");
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

      const req = new Request(endpoint("/admin/codex/auth"), {
        method: "POST",
        headers: {
          "Authorization": `Bearer ${adminToken}`,
          "Content-Type": "application/json",
          "Accept": "application/json",
        },
        body: authJsonText,
      });

      const result = await doFetch(req);
      if (!result.ok) {
        console.error(`Request failed (${result.status}).`);
        console.error(result.body);
        Deno.exit(1);
      }
      console.log(JSON.stringify(result.json, null, 2));
      return;
    }

    if (sub === "keys") {
      const action = subRest[0] ?? "";

      if (action === "create") {
        const name = typeof flags.name === "string" ? flags.name : "";
        if (!name.trim()) {
          console.error("Missing --name");
          Deno.exit(2);
        }
        const token = typeof flags.token === "string" ? flags.token.trim() : "";
        const tokenOnly = flags["token-only"] === true;

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

        if (tokenOnly) {
          const tokenValue = (result.json && typeof result.json === "object" && "token" in result.json)
            ? (result.json as { token?: unknown }).token
            : null;
          console.log(typeof tokenValue === "string" ? tokenValue : "");
          return;
        }

        console.log(JSON.stringify(result.json, null, 2));
        return;
      }

      if (action === "list") {
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
        return;
      }

      if (action === "revoke") {
        const id = typeof flags.id === "string" ? flags.id.trim() : "";
        if (!id) {
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
        return;
      }

      console.error(`Unknown admin keys command: ${action || "(missing)"}`);
      usage();
      Deno.exit(2);
    }

    console.error(`Unknown admin command: ${sub || "(missing)"}`);
    usage();
    Deno.exit(2);
  }

  console.error(`Unknown command: ${cmd}`);
  usage();
  Deno.exit(2);
};

await main();
