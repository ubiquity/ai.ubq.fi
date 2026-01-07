import { config } from "./config.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";

const INDEX_HTML_URL = new URL("../static/index.html", import.meta.url);
const CHAT_HTML_URL = new URL("../static/chat.html", import.meta.url);
const ADMIN_HTML_URL = new URL("../static/admin.html", import.meta.url);
const STYLE_CSS_URL = new URL("../static/style.css", import.meta.url);
const APP_JS_URL = new URL("../static/app.js", import.meta.url);
const CHAT_JS_URL = new URL("../static/chat.js", import.meta.url);
const ADMIN_JS_URL = new URL("../static/admin.js", import.meta.url);
const FAVICON_32_URL = new URL("../favicon-32.png", import.meta.url);
const FAVICON_URL = new URL("../favicon.png", import.meta.url);

const readTextFile = async (url: URL, label: string): Promise<string | null> => {
  try {
    return await Deno.readTextFile(url);
  } catch (error) {
    console.error(`[ai.ubq.fi] Failed to load ${label}:`, error);
    return null;
  }
};

const readBytesFile = async (url: URL, label: string): Promise<Uint8Array | null> => {
  try {
    return await Deno.readFile(url);
  } catch (error) {
    console.error(`[ai.ubq.fi] Failed to load ${label}:`, error);
    return null;
  }
};

const indexHtmlPromise = readTextFile(INDEX_HTML_URL, "static/index.html");
const chatHtmlPromise = readTextFile(CHAT_HTML_URL, "static/chat.html");
const adminHtmlPromise = readTextFile(ADMIN_HTML_URL, "static/admin.html");
const styleCssPromise = readTextFile(STYLE_CSS_URL, "static/style.css");
const appJsPromise = readTextFile(APP_JS_URL, "static/app.js");
const chatJsPromise = readTextFile(CHAT_JS_URL, "static/chat.js");
const adminJsPromise = readTextFile(ADMIN_JS_URL, "static/admin.js");

const favicon32Promise = readBytesFile(FAVICON_32_URL, "favicon-32.png");
const faviconPromise = readBytesFile(FAVICON_URL, "favicon.png");

const staticCacheControl = config.isDeploy ? "public, max-age=300" : "no-store";

const loadText = async (url: URL, label: string, cached: Promise<string | null>): Promise<string | null> =>
  config.isDeploy ? await cached : await readTextFile(url, label);

const loadBytes = async (
  url: URL,
  label: string,
  cached: Promise<Uint8Array | null>,
): Promise<Uint8Array | null> => (config.isDeploy ? await cached : await readBytesFile(url, label));

const htmlSecurityHeaders = (): HeadersInit => ({
  "Cache-Control": staticCacheControl,
  "Content-Security-Policy":
    "default-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; img-src 'self' data:; object-src 'none'; style-src 'self'; script-src 'self'; connect-src 'self' https://ai.ubq.fi",
  "Referrer-Policy": "no-referrer",
  "X-Content-Type-Options": "nosniff",
});

export const handleRoot = async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const path = url.pathname;

  const accept = req.headers.get("Accept") ?? "";
  const wantsHtml = path === "/index.html" || accept.includes("text/html") || accept.includes("application/xhtml+xml");
  if (wantsHtml) {
    const html = await loadText(INDEX_HTML_URL, "static/index.html", indexHtmlPromise);
    if (html) {
      return new Response(html, {
        status: 200,
        headers: {
          "Content-Type": "text/html; charset=utf-8",
          "Vary": "Accept",
          ...htmlSecurityHeaders(),
        },
      });
    }
  }

  const kv = await kvPromise;
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

export const handleChatPage = async (): Promise<Response> => {
  const html = await loadText(CHAT_HTML_URL, "static/chat.html", chatHtmlPromise);
  if (!html) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(html, {
    status: 200,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      ...htmlSecurityHeaders(),
    },
  });
};

export const handleAdminPage = async (): Promise<Response> => {
  const html = await loadText(ADMIN_HTML_URL, "static/admin.html", adminHtmlPromise);
  if (!html) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(html, {
    status: 200,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      ...htmlSecurityHeaders(),
    },
  });
};

export const handleStyleCss = async (): Promise<Response> => {
  const css = await loadText(STYLE_CSS_URL, "static/style.css", styleCssPromise);
  if (!css) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(css, {
    status: 200,
    headers: {
      "Content-Type": "text/css; charset=utf-8",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleAppJs = async (): Promise<Response> => {
  const js = await loadText(APP_JS_URL, "static/app.js", appJsPromise);
  if (!js) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(js, {
    status: 200,
    headers: {
      "Content-Type": "text/javascript; charset=utf-8",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleChatJs = async (): Promise<Response> => {
  const js = await loadText(CHAT_JS_URL, "static/chat.js", chatJsPromise);
  if (!js) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(js, {
    status: 200,
    headers: {
      "Content-Type": "text/javascript; charset=utf-8",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleAdminJs = async (): Promise<Response> => {
  const js = await loadText(ADMIN_JS_URL, "static/admin.js", adminJsPromise);
  if (!js) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(js, {
    status: 200,
    headers: {
      "Content-Type": "text/javascript; charset=utf-8",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleFavicon32 = async (): Promise<Response> => {
  const bytes = await loadBytes(FAVICON_32_URL, "favicon-32.png", favicon32Promise);
  if (!bytes) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(bytes, {
    status: 200,
    headers: {
      "Content-Type": "image/png",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleFavicon = async (): Promise<Response> => {
  const bytes = await loadBytes(FAVICON_URL, "favicon.png", faviconPromise);
  if (!bytes) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(bytes, {
    status: 200,
    headers: {
      "Content-Type": "image/png",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};
