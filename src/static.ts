import { config } from "./config.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";

const INDEX_HTML_URL = new URL("../static/index.html", import.meta.url);
const DOCS_HTML_URL = new URL("../static/docs.html", import.meta.url);
const CHAT_HTML_URL = new URL("../static/chat.html", import.meta.url);
const ADMIN_HTML_URL = new URL("../static/admin.html", import.meta.url);
const STYLE_CSS_URL = new URL("../static/style.css", import.meta.url);
const CHAT_CSS_URL = new URL("../static/chat.css", import.meta.url);
const HOME_CSS_URL = new URL("../static/home.css", import.meta.url);
const DOCS_CSS_URL = new URL("../static/docs.css", import.meta.url);
const ADMIN_CSS_URL = new URL("../static/admin.css", import.meta.url);
const APP_JS_URL = new URL("../static/app.js", import.meta.url);
const CHAT_JS_URL = new URL("../static/chat.js", import.meta.url);
const ADMIN_JS_URL = new URL("../static/admin.js", import.meta.url);
const NETWORK_JS_URL = new URL("../static/network.js", import.meta.url);
const DOCS_JS_URL = new URL("../static/docs.js", import.meta.url);
const DOCS_LLM_AGENTS_MD_URL = new URL("../static/docs/llms-agents.md", import.meta.url);
const COMPANY_LOGO_URL = new URL("../static/company-logo.svg", import.meta.url);
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

const readBytesFile = async (url: URL, label: string): Promise<Uint8Array<ArrayBuffer> | null> => {
  try {
    return await Deno.readFile(url);
  } catch (error) {
    console.error(`[ai.ubq.fi] Failed to load ${label}:`, error);
    return null;
  }
};

const indexHtmlPromise = readTextFile(INDEX_HTML_URL, "static/index.html");
const docsHtmlPromise = readTextFile(DOCS_HTML_URL, "static/docs.html");
const chatHtmlPromise = readTextFile(CHAT_HTML_URL, "static/chat.html");
const adminHtmlPromise = readTextFile(ADMIN_HTML_URL, "static/admin.html");
const styleCssPromise = readTextFile(STYLE_CSS_URL, "static/style.css");
const chatCssPromise = readTextFile(CHAT_CSS_URL, "static/chat.css");
const homeCssPromise = readTextFile(HOME_CSS_URL, "static/home.css");
const docsCssPromise = readTextFile(DOCS_CSS_URL, "static/docs.css");
const adminCssPromise = readTextFile(ADMIN_CSS_URL, "static/admin.css");
const appJsPromise = readTextFile(APP_JS_URL, "static/app.js");
const chatJsPromise = readTextFile(CHAT_JS_URL, "static/chat.js");
const adminJsPromise = readTextFile(ADMIN_JS_URL, "static/admin.js");
const networkJsPromise = readTextFile(NETWORK_JS_URL, "static/network.js");
const docsJsPromise = readTextFile(DOCS_JS_URL, "static/docs.js");
const docsLlmAgentsMdPromise = readTextFile(DOCS_LLM_AGENTS_MD_URL, "static/docs/llms-agents.md");
const companyLogoPromise = readTextFile(COMPANY_LOGO_URL, "static/company-logo.svg");

const favicon32Promise = readBytesFile(FAVICON_32_URL, "favicon-32.png");
const faviconPromise = readBytesFile(FAVICON_URL, "favicon.png");

const staticCacheControl = config.isDeploy ? "public, max-age=300" : "no-store";

const loadText = async (url: URL, label: string, cached: Promise<string | null>): Promise<string | null> =>
  config.isDeploy ? await cached : await readTextFile(url, label);

const loadBytes = async (
  url: URL,
  label: string,
  cached: Promise<Uint8Array<ArrayBuffer> | null>,
): Promise<Uint8Array<ArrayBuffer> | null> => (config.isDeploy ? await cached : await readBytesFile(url, label));

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

export const handleDocsPage = async (): Promise<Response> => {
  const html = await loadText(DOCS_HTML_URL, "static/docs.html", docsHtmlPromise);
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

export const handleChatCss = async (): Promise<Response> => {
  const css = await loadText(CHAT_CSS_URL, "static/chat.css", chatCssPromise);
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

export const handleHomeCss = async (): Promise<Response> => {
  const css = await loadText(HOME_CSS_URL, "static/home.css", homeCssPromise);
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

export const handleDocsCss = async (): Promise<Response> => {
  const css = await loadText(DOCS_CSS_URL, "static/docs.css", docsCssPromise);
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

export const handleAdminCss = async (): Promise<Response> => {
  const css = await loadText(ADMIN_CSS_URL, "static/admin.css", adminCssPromise);
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

export const handleNetworkJs = async (): Promise<Response> => {
  const js = await loadText(NETWORK_JS_URL, "static/network.js", networkJsPromise);
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

export const handleDocsJs = async (): Promise<Response> => {
  const js = await loadText(DOCS_JS_URL, "static/docs.js", docsJsPromise);
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

export const handleDocsLlmAgentsMd = async (): Promise<Response> => {
  const md = await loadText(
    DOCS_LLM_AGENTS_MD_URL,
    "static/docs/llms-agents.md",
    docsLlmAgentsMdPromise,
  );
  if (!md) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(md, {
    status: 200,
    headers: {
      "Content-Type": "text/markdown; charset=utf-8",
      "Cache-Control": staticCacheControl,
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleCompanyLogo = async (): Promise<Response> => {
  const svg = await loadText(COMPANY_LOGO_URL, "static/company-logo.svg", companyLogoPromise);
  if (!svg) {
    return new Response("Not found", {
      status: 404,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  return new Response(svg, {
    status: 200,
    headers: {
      "Content-Type": "image/svg+xml; charset=utf-8",
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
