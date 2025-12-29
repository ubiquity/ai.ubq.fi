import { config } from "./config.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";

const INDEX_HTML_URL = new URL("../static/index.html", import.meta.url);
const CHAT_HTML_URL = new URL("../static/chat.html", import.meta.url);
const STYLE_CSS_URL = new URL("../static/style.css", import.meta.url);
const APP_JS_URL = new URL("../static/app.js", import.meta.url);
const CHAT_JS_URL = new URL("../static/chat.js", import.meta.url);
const FAVICON_32_URL = new URL("../favicon-32.png", import.meta.url);
const FAVICON_URL = new URL("../favicon.png", import.meta.url);

const indexHtmlPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(INDEX_HTML_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load static/index.html:", error);
    return null;
  }
})();

const chatHtmlPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(CHAT_HTML_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load static/chat.html:", error);
    return null;
  }
})();

const styleCssPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(STYLE_CSS_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load static/style.css:", error);
    return null;
  }
})();

const appJsPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(APP_JS_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load static/app.js:", error);
    return null;
  }
})();

const chatJsPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(CHAT_JS_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load static/chat.js:", error);
    return null;
  }
})();

const favicon32Promise: Promise<Uint8Array | null> = (async () => {
  try {
    return await Deno.readFile(FAVICON_32_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load favicon-32.png:", error);
    return null;
  }
})();

const faviconPromise: Promise<Uint8Array | null> = (async () => {
  try {
    return await Deno.readFile(FAVICON_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load favicon.png:", error);
    return null;
  }
})();

const htmlSecurityHeaders = (): HeadersInit => ({
  "Cache-Control": "public, max-age=300",
  "Content-Security-Policy":
    "default-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; img-src 'self' data:; object-src 'none'; style-src 'self'; script-src 'self'; connect-src 'self'",
  "Referrer-Policy": "no-referrer",
  "X-Content-Type-Options": "nosniff",
});

export const handleRoot = async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const path = url.pathname;

  const accept = req.headers.get("Accept") ?? "";
  const wantsHtml = path === "/index.html" || accept.includes("text/html") || accept.includes("application/xhtml+xml");
  if (wantsHtml) {
    const html = await indexHtmlPromise;
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
  const html = await chatHtmlPromise;
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
  const css = await styleCssPromise;
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
      "Cache-Control": "public, max-age=300",
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleAppJs = async (): Promise<Response> => {
  const js = await appJsPromise;
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
      "Cache-Control": "public, max-age=300",
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleChatJs = async (): Promise<Response> => {
  const js = await chatJsPromise;
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
      "Cache-Control": "public, max-age=300",
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleFavicon32 = async (): Promise<Response> => {
  const bytes = await favicon32Promise;
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
      "Cache-Control": "public, max-age=300",
      "X-Content-Type-Options": "nosniff",
    },
  });
};

export const handleFavicon = async (): Promise<Response> => {
  const bytes = await faviconPromise;
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
      "Cache-Control": "public, max-age=300",
      "X-Content-Type-Options": "nosniff",
    },
  });
};
