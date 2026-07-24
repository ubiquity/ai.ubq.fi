import { config } from "./config.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";

type StaticBody = string | Uint8Array<ArrayBuffer>;

type StaticAsset = {
  url: URL;
  label: string;
  contentType: string;
  readAs: "text" | "bytes";
  security: "html" | "asset";
};

const staticCacheControl = config.isDeploy ? "public, max-age=300" : "no-store";

const fromStatic = (path: string): URL => new URL(`../static/${path}`, import.meta.url);
const fromRoot = (path: string): URL => new URL(`../${path}`, import.meta.url);

const staticAssets = new Map<string, StaticAsset>();

const registerAsset = (
  routes: string[],
  asset: StaticAsset,
): StaticAsset => {
  for (const route of routes) {
    staticAssets.set(route, asset);
  }
  return asset;
};

const textAsset = (
  routes: string[],
  path: string,
  contentType: string,
  security: StaticAsset["security"] = "asset",
): StaticAsset =>
  registerAsset(routes, {
    url: fromStatic(path),
    label: `static/${path}`,
    contentType,
    readAs: "text",
    security,
  });

const bytesAsset = (
  routes: string[],
  path: string,
  contentType: string,
): StaticAsset =>
  registerAsset(routes, {
    url: fromRoot(path),
    label: path,
    contentType,
    readAs: "bytes",
    security: "asset",
  });

const indexHtmlAsset = textAsset(["/index.html"], "index.html", "text/html; charset=utf-8", "html");

textAsset(["/docs", "/docs.html"], "docs.html", "text/html; charset=utf-8", "html");
textAsset(["/chat", "/chat.html"], "chat.html", "text/html; charset=utf-8", "html");
textAsset(["/admin", "/admin.html"], "admin.html", "text/html; charset=utf-8", "html");

textAsset(["/style.css"], "style.css", "text/css; charset=utf-8");
textAsset(["/docs.css"], "docs.css", "text/css; charset=utf-8");
textAsset(["/chat.css"], "chat.css", "text/css; charset=utf-8");
textAsset(["/home.css"], "home.css", "text/css; charset=utf-8");
textAsset(["/admin.css"], "admin.css", "text/css; charset=utf-8");

textAsset(["/app.js"], "app.js", "text/javascript; charset=utf-8");
textAsset(["/docs.js"], "docs.js", "text/javascript; charset=utf-8");
textAsset(["/chat.js"], "chat.js", "text/javascript; charset=utf-8");
textAsset(["/admin.js"], "admin.js", "text/javascript; charset=utf-8");
textAsset(["/auth.js"], "auth.js", "text/javascript; charset=utf-8");
textAsset(["/auth-relay.js"], "auth-relay.js", "text/javascript; charset=utf-8");
textAsset(["/foreground-refresh.js"], "foreground-refresh.js", "text/javascript; charset=utf-8");
textAsset(["/network.js"], "network.js", "text/javascript; charset=utf-8");
textAsset(["/reasoning-select.js"], "reasoning-select.js", "text/javascript; charset=utf-8");

textAsset(["/company-logo.svg"], "company-logo.svg", "image/svg+xml; charset=utf-8");
textAsset(["/docs/llms-agents.md"], "docs/llms-agents.md", "text/markdown; charset=utf-8");

bytesAsset(["/favicon.ico", "/favicon.png"], "favicon.png", "image/png");
bytesAsset(["/favicon-32.png"], "favicon-32.png", "image/png");

const readAsset = async (asset: StaticAsset): Promise<StaticBody | null> => {
  try {
    if (asset.readAs === "text") {
      return await Deno.readTextFile(asset.url);
    }

    const bytes = await Deno.readFile(asset.url);
    const copy: Uint8Array<ArrayBuffer> = new Uint8Array(bytes.byteLength);
    copy.set(bytes);
    return copy;
  } catch (error) {
    console.error(`[ai.ubq.fi] Failed to load ${asset.label}:`, error);
    return null;
  }
};

const notFoundResponse = (): Response =>
  new Response("Not found", {
    status: 404,
    headers: { "Content-Type": "text/plain; charset=utf-8" },
  });

const htmlSecurityHeaders = (): HeadersInit => ({
  "Cache-Control": staticCacheControl,
  "Content-Security-Policy":
    "default-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; img-src 'self' data:; object-src 'none'; style-src 'self'; script-src 'self'; connect-src 'self' https://ai.ubq.fi",
  "Referrer-Policy": "no-referrer",
  "X-Content-Type-Options": "nosniff",
});

const staticHeaders = (asset: StaticAsset, extra: HeadersInit = {}): HeadersInit => ({
  "Content-Type": asset.contentType,
  ...(asset.security === "html" ? htmlSecurityHeaders() : {
    "Cache-Control": staticCacheControl,
    "X-Content-Type-Options": "nosniff",
  }),
  ...extra,
});

const serveAsset = async (asset: StaticAsset, extraHeaders?: HeadersInit): Promise<Response> => {
  const body = await readAsset(asset);
  if (body === null) return notFoundResponse();

  return new Response(body, {
    status: 200,
    headers: staticHeaders(asset, extraHeaders),
  });
};

export const handleStaticAsset = async (path: string): Promise<Response | null> => {
  const asset = staticAssets.get(path);
  if (!asset) return null;
  return await serveAsset(asset);
};

export const hasStaticAsset = (path: string): boolean => staticAssets.has(path);

export const handleRoot = async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const path = url.pathname;

  const accept = req.headers.get("Accept") ?? "";
  const wantsHtml = path === "/index.html" || accept.includes("text/html") || accept.includes("application/xhtml+xml");
  if (wantsHtml) {
    return await serveAsset(indexHtmlAsset, { "Vary": "Accept" });
  }

  const kv = await getKv();
  const auth = config.isDeploy && config.authTokens.size === 0 && !kv
    ? "misconfigured"
    : config.isDeploy || config.authTokens.size > 0 || Boolean(kv)
    ? "required"
    : "disabled (local only)";

  return json(
    200,
    {
      ok: true,
      service: "ai.ubq.fi",
      upstream: "chatgpt_codex",
      auth,
      endpoints: {
        openai_compat: "/v1/*",
        health: "/health",
      },
    },
    { "Vary": "Accept" },
  );
};
