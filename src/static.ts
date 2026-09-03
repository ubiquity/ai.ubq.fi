import { config } from "./config.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";
import aboutHtmlText from "../static/about.html" with { type: "text" };
import contactHtmlText from "../static/contact.html" with { type: "text" };
import developersHtmlText from "../static/developers.html" with { type: "text" };
import homeMarkdownText from "../static/home.md" with { type: "text" };
import indexHtmlText from "../static/index.html" with { type: "text" };
import llmsFullText from "../static/docs/llms-agents.md" with { type: "text" };
import llmsText from "../static/llms.txt" with { type: "text" };
import modelsHtmlText from "../static/models.html" with { type: "text" };
import openApiText from "../static/openapi.json" with { type: "text" };
import privacyHtmlText from "../static/privacy.html" with { type: "text" };
import robotsText from "../static/robots.txt" with { type: "text" };
import sitemapText from "../static/sitemap.xml" with { type: "text" };

type StaticBody = string | Uint8Array<ArrayBuffer>;

type StaticAsset = {
  url: URL;
  label: string;
  contentType: string;
  readAs: "text" | "bytes";
  security: "html" | "asset";
  body?: StaticBody;
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
  body?: string,
): StaticAsset =>
  registerAsset(routes, {
    url: fromStatic(path),
    label: `static/${path}`,
    contentType,
    readAs: "text",
    security,
    body,
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

const indexHtmlAsset = textAsset(["/index.html"], "index.html", "text/html; charset=utf-8", "html", indexHtmlText);
const indexMarkdownAsset: StaticAsset = {
  url: fromStatic("home.md"),
  label: "static/home.md",
  contentType: "text/markdown; charset=utf-8",
  readAs: "text",
  security: "asset",
  body: homeMarkdownText,
};

textAsset(["/docs", "/docs.html"], "docs.html", "text/html; charset=utf-8", "html");
textAsset(
  ["/developers", "/developers.html"],
  "developers.html",
  "text/html; charset=utf-8",
  "html",
  developersHtmlText,
);
textAsset(["/about", "/about.html"], "about.html", "text/html; charset=utf-8", "html", aboutHtmlText);
textAsset(["/contact", "/contact.html"], "contact.html", "text/html; charset=utf-8", "html", contactHtmlText);
textAsset(["/privacy", "/privacy.html"], "privacy.html", "text/html; charset=utf-8", "html", privacyHtmlText);
textAsset(["/chat", "/chat.html"], "chat.html", "text/html; charset=utf-8", "html");
textAsset(["/models", "/models.html"], "models.html", "text/html; charset=utf-8", "html", modelsHtmlText);
textAsset(["/admin", "/admin.html"], "admin.html", "text/html; charset=utf-8", "html");

textAsset(["/style.css"], "style.css", "text/css; charset=utf-8");
textAsset(["/docs.css"], "docs.css", "text/css; charset=utf-8");
textAsset(["/chat.css"], "chat.css", "text/css; charset=utf-8");
textAsset(["/models.css"], "models.css", "text/css; charset=utf-8");
textAsset(["/home.css"], "home.css", "text/css; charset=utf-8");
textAsset(["/admin.css"], "admin.css", "text/css; charset=utf-8");

textAsset(["/app.js"], "app.js", "text/javascript; charset=utf-8");
textAsset(["/docs.js"], "docs.js", "text/javascript; charset=utf-8");
textAsset(["/chat.js"], "chat.js", "text/javascript; charset=utf-8");
textAsset(["/chat-stats.js"], "chat-stats.js", "text/javascript; charset=utf-8");
textAsset(["/models.js"], "models.js", "text/javascript; charset=utf-8");
textAsset(["/toast.js"], "toast.js", "text/javascript; charset=utf-8");
textAsset(["/admin.js"], "admin.js", "text/javascript; charset=utf-8");
textAsset(["/admin-cache.js"], "admin-cache.js", "text/javascript; charset=utf-8");
textAsset(["/auth.js"], "auth.js", "text/javascript; charset=utf-8");
textAsset(["/auth-relay.js"], "auth-relay.js", "text/javascript; charset=utf-8");
textAsset(["/foreground-refresh.js"], "foreground-refresh.js", "text/javascript; charset=utf-8");
textAsset(["/network.js"], "network.js", "text/javascript; charset=utf-8");
textAsset(["/reasoning-select.js"], "reasoning-select.js", "text/javascript; charset=utf-8");

textAsset(["/company-logo.svg"], "company-logo.svg", "image/svg+xml; charset=utf-8");
textAsset(["/llms.txt"], "llms.txt", "text/plain; charset=utf-8", "asset", llmsText);
textAsset(["/llms-full.txt"], "docs/llms-agents.md", "text/plain; charset=utf-8", "asset", llmsFullText);
textAsset(
  ["/docs/llms-agents.md"],
  "docs/llms-agents.md",
  "text/markdown; charset=utf-8",
  "asset",
  llmsFullText,
);
textAsset(["/openapi.json"], "openapi.json", "application/json; charset=utf-8", "asset", openApiText);
textAsset(["/robots.txt"], "robots.txt", "text/plain; charset=utf-8", "asset", robotsText);
textAsset(["/sitemap.xml"], "sitemap.xml", "application/xml; charset=utf-8", "asset", sitemapText);

bytesAsset(["/favicon.ico", "/favicon.png"], "favicon.png", "image/png");
bytesAsset(["/favicon-32.png"], "favicon-32.png", "image/png");

const readAsset = async (asset: StaticAsset): Promise<StaticBody | null> => {
  try {
    if (asset.body !== undefined) return asset.body;
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
  json(404, {
    error: {
      message: "Public resource not found. Read /openapi.json for API endpoints or /llms.txt for agent guidance.",
      type: "not_found",
      code: "not_found",
    },
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

type RootRepresentation = "html" | "markdown" | "json";

type MediaPreference = Readonly<{
  quality: number;
  specificity: number;
  index: number;
}>;

const mediaPreference = (accept: string, target: string): MediaPreference | null => {
  const [targetType, targetSubtype] = target.split("/");
  if (!targetType || !targetSubtype) return null;

  let best: MediaPreference | null = null;
  for (const [index, rawEntry] of accept.split(",").entries()) {
    const [rawRange, ...rawParameters] = rawEntry.trim().toLowerCase().split(";");
    const [rangeType, rangeSubtype] = rawRange.trim().split("/");
    if (!rangeType || !rangeSubtype) continue;
    if (rangeType !== "*" && rangeType !== targetType) continue;
    if (rangeSubtype !== "*" && rangeSubtype !== targetSubtype) continue;

    const rawQuality = rawParameters
      .map((parameter) => parameter.trim().split("=", 2))
      .find(([name]) => name === "q")?.[1];
    const parsedQuality = rawQuality === undefined ? 1 : Number(rawQuality);
    const quality = Number.isFinite(parsedQuality) && parsedQuality >= 0 && parsedQuality <= 1 ? parsedQuality : 0;
    const specificity = rangeType === "*" ? 0 : rangeSubtype === "*" ? 1 : 2;
    const candidate = { quality, specificity, index };
    if (
      !best || candidate.specificity > best.specificity ||
      (candidate.specificity === best.specificity && candidate.index < best.index)
    ) {
      best = candidate;
    }
  }
  return best;
};

const preferredMedia = (...preferences: readonly (MediaPreference | null)[]): MediaPreference | null => {
  let best: MediaPreference | null = null;
  for (const candidate of preferences) {
    if (
      !candidate || !best || candidate.quality > best.quality ||
      (candidate.quality === best.quality && candidate.specificity > best.specificity) ||
      (candidate.quality === best.quality && candidate.specificity === best.specificity && candidate.index < best.index)
    ) {
      if (candidate) best = candidate;
    }
  }
  return best;
};

const rootRepresentation = (accept: string): RootRepresentation | null => {
  if (!accept.trim()) return "html";
  const candidates = ([
    ["html", preferredMedia(mediaPreference(accept, "text/html"), mediaPreference(accept, "application/xhtml+xml")), 2],
    ["markdown", mediaPreference(accept, "text/markdown"), 1],
    ["json", mediaPreference(accept, "application/json"), 0],
  ] as const).flatMap(([representation, preference, defaultPriority]) => {
    return preference?.quality ? [{ representation, preference, defaultPriority }] : [];
  });
  if (!candidates.length) return null;
  candidates.sort((left, right) =>
    right.preference.quality - left.preference.quality ||
    right.preference.specificity - left.preference.specificity ||
    left.preference.index - right.preference.index ||
    right.defaultPriority - left.defaultPriority
  );
  return candidates[0]!.representation;
};

const rootVaryHeaders = { "Vary": "Accept, Accept-Encoding" };

const notAcceptable = (): Response =>
  json(406, {
    error: {
      message: "The homepage is available as text/html, text/markdown, or application/json.",
      type: "not_acceptable",
      code: "not_acceptable",
    },
  }, rootVaryHeaders);

export const handleRoot = async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const path = url.pathname;

  if (path === "/index.html") return await serveAsset(indexHtmlAsset, rootVaryHeaders);

  const representation = rootRepresentation(req.headers.get("Accept") ?? "");
  if (representation === null) return notAcceptable();
  if (representation === "html") return await serveAsset(indexHtmlAsset, rootVaryHeaders);
  if (representation === "markdown") return await serveAsset(indexMarkdownAsset, rootVaryHeaders);

  const kv = await getKv();
  const auth = config.isDeploy && config.authTokens.size === 0 && !kv
    ? "misconfigured"
    : config.isDeploy || config.authTokens.size > 0 || Boolean(kv)
    ? "required"
    : "disabled (local only)";

  return json(
    200,
    {
      service: "ai.ubq.fi",
      name: "UbiquityOS AI Gateway",
      upstream: "chatgpt_codex",
      auth,
      endpoints: {
        openai_compat: "/v1/*",
        health: "/health",
      },
      discovery: {
        llms: "/llms.txt",
        llms_full: "/llms-full.txt",
        openapi: "/openapi.json",
        developers: "/developers",
        sitemap: "/sitemap.xml",
      },
    },
    rootVaryHeaders,
  );
};
