import assert from "node:assert/strict";

import handler from "../src/handler.ts";
import { corsHeaders } from "../src/http.ts";
import { handleRoot, handleStaticAsset, hasStaticAsset } from "../src/static.ts";
import readmeText from "../README.md" with { type: "text" };
import adminCss from "../static/admin.css" with { type: "text" };
import adminCacheScript from "../static/admin-cache.js" with { type: "text" };
import adminHtml from "../static/admin.html" with { type: "text" };
import aboutHtml from "../static/about.html" with { type: "text" };
import adminScript from "../static/admin.js" with { type: "text" };
import authScript from "../static/auth.js" with { type: "text" };
import chatCss from "../static/chat.css" with { type: "text" };
import chatHtml from "../static/chat.html" with { type: "text" };
import chatScript from "../static/chat.js" with { type: "text" };
import companyLogoSvg from "../static/company-logo.svg" with { type: "text" };
import contactHtml from "../static/contact.html" with { type: "text" };
import developersHtml from "../static/developers.html" with { type: "text" };
import docsHtml from "../static/docs.html" with { type: "text" };
import indexHtml from "../static/index.html" with { type: "text" };
import llmsText from "../static/llms.txt" with { type: "text" };
import llmsFullText from "../static/docs/llms-agents.md" with { type: "text" };
import modelsCss from "../static/models.css" with { type: "text" };
import modelsHtml from "../static/models.html" with { type: "text" };
import modelsScript from "../static/models.js" with { type: "text" };
import openApiText from "../static/openapi.json" with { type: "text" };
import privacyHtml from "../static/privacy.html" with { type: "text" };
import styleCss from "../static/style.css" with { type: "text" };

Deno.test("static assets register frontend module dependencies", () => {
  for (
    const path of [
      "/admin.js",
      "/admin.css",
      "/auth.js",
      "/auth-relay.js",
      "/chat-stats.js",
      "/foreground-refresh.js",
      "/network.js",
      "/models.js",
      "/models.css",
      "/reasoning-select.js",
      "/admin-cache.js",
      "/toast.js",
    ]
  ) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
  assert.match(chatHtml, /<script type="module" src="\/chat\.js\?v=20260903-chat-motion-v1"><\/script>/);
  assert.match(chatScript, /from "\.\/chat-stats\.js\?v=20260827-response-stats-v4";/);
  assert.match(chatScript, /from "\.\/toast\.js\?v=20260903-toast-v1";/);
});

Deno.test("chat response stats use one conversation bar below the composer", () => {
  assert.equal((chatHtml.match(/\bdata-chat-stats\b/g) ?? []).length, 1);
  assert.match(
    chatHtml,
    /<\/form>\s*<div data-chat-stats role="status" aria-live="polite" aria-atomic="true" hidden><\/div>/,
  );
  assert.doesNotMatch(chatHtml, /data-message-stats/);
  assert.doesNotMatch(chatScript, /appendChatMessageStats|data-message-stats/);
  assert.match(chatScript, /recordCompletedChatResponse\(chatStats, sample\)/);
  assert.match(chatScript, /decodeMs: firstTokenAt === null \? undefined : completedAt - firstTokenAt/);
  assert.match(chatScript, /decodeTokens: readChatCompletionDecodeTokens\(responseUsage, reasoningEffort\)/);
  assert.match(chatScript, /if \(!assistantText\.trim\(\)\) \{[\s\S]*?return;[\s\S]*?recordAndRenderChatStats\(/);
  assert.doesNotMatch(chatScript, /firstStreamEventAt/);
  assert.match(chatCss, /\[data-chat-stats\]\s*\{[^}]*overflow-wrap:\s*anywhere;[^}]*white-space:\s*normal;/s);
  assert.doesNotMatch(chatCss, /\[data-chat-stats\]\s*\{[^}]*(?:overflow:\s*hidden|white-space:\s*nowrap)/s);
  assert.doesNotMatch(chatCss, /\[data-message-stats\]/);
});

Deno.test("public models page is registered", () => {
  assert.equal(hasStaticAsset("/models"), true);
  assert.equal(hasStaticAsset("/models.html"), true);
  assert.match(modelsHtml, /<script type="module" src="\/models\.js\?v=20260903-recent-reasoning-v3"><\/script>/);
});

Deno.test("public console pages share versioned styles, canonical navigation, and accurate active states", () => {
  const assetVersion = "public-console-20260903-v6";
  const canonicalLinks = [
    { href: "/models", label: "Models" },
    { href: "/developers", label: "Developers" },
    { href: "/docs", label: "Docs" },
    { href: "/chat", label: "Chat" },
    { href: "/admin", label: "Admin" },
  ];
  const pages = [
    { name: "index", html: indexHtml, pageCss: "home.css", activeHref: null },
    { name: "docs", html: docsHtml, pageCss: "docs.css", activeHref: "/docs" },
    { name: "developers", html: developersHtml, pageCss: "docs.css", activeHref: "/developers" },
    { name: "models", html: modelsHtml, pageCss: "models.css", activeHref: "/models" },
    { name: "chat", html: chatHtml, pageCss: "chat.css", activeHref: "/chat" },
    { name: "about", html: aboutHtml, pageCss: "docs.css", activeHref: null },
    { name: "contact", html: contactHtml, pageCss: "docs.css", activeHref: null },
    { name: "privacy", html: privacyHtml, pageCss: "docs.css", activeHref: null },
  ];

  for (const page of pages) {
    const stylesheetHrefs = [...page.html.matchAll(/<link\b(?=[^>]*\brel="stylesheet")[^>]*\bhref="([^"]+)"[^>]*>/g)]
      .map((match) => match[1]);
    assert.deepEqual(
      stylesheetHrefs,
      [`/style.css?v=${assetVersion}`, `/${page.pageCss}?v=${assetVersion}`],
      `${page.name} should load only the release-matched shared and page styles`,
    );
    for (const href of stylesheetHrefs) {
      assert.equal(hasStaticAsset(new URL(href, "https://ai.ubq.fi").pathname), true, `${href} should be registered`);
    }

    assert.equal(
      (page.html.match(/<header\b[^>]*\bdata-shared-header\b[^>]*>/g) ?? []).length,
      1,
      `${page.name} should have one shared header`,
    );
    assert.equal(
      (page.html.match(/<nav\b[^>]*\bdata-actions\b[^>]*>/g) ?? []).length,
      1,
      `${page.name} should have one actions navigation`,
    );
    const primaryNavMatches = [
      ...page.html.matchAll(
        /<nav\b(?=[^>]*\bdata-actions\b)(?=[^>]*\baria-label="Primary")[^>]*>([\s\S]*?)<\/nav>/g,
      ),
    ];
    assert.equal(primaryNavMatches.length, 1, `${page.name} should have one actions primary navigation`);
    const primaryNav = primaryNavMatches[0]?.[1] ?? "";
    assert.doesNotMatch(
      primaryNav,
      /<a\b[^>]*\bdata-variant="primary"/,
      `${page.name} primary navigation must derive its active treatment from aria-current`,
    );

    const links = [...primaryNav.matchAll(/<a\b([^>]*)>([\s\S]*?)<\/a>/g)].map((match) => ({
      attributes: match[1] ?? "",
      href: match[1]?.match(/\bhref="([^"]+)"/)?.[1] ?? "",
      label: (match[2] ?? "").replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim(),
    }));
    assert.deepEqual(
      links.map(({ href, label }) => ({ href, label })),
      canonicalLinks,
      `${page.name} should preserve the canonical primary navigation order and labels`,
    );
    for (const link of links) {
      assert.match(link.attributes, /\bdata-button\b/, `${page.name} ${link.label} should use the shared nav control`);
    }

    const currentLinks = links.filter((link) => /\baria-current="page"/.test(link.attributes));
    assert.equal(
      (primaryNav.match(/\baria-current=/g) ?? []).length,
      page.activeHref === null ? 0 : 1,
      `${page.name} should not expose an alternate or duplicate primary current state`,
    );
    assert.deepEqual(
      currentLinks.map(({ href }) => href),
      page.activeHref === null ? [] : [page.activeHref],
      `${page.name} should mark only its matching canonical destination as current`,
    );
  }

  assert.match(adminHtml, new RegExp(`/style\\.css\\?v=${assetVersion}`));
});

Deno.test("public console styles retain the bordered neutral admin surface without decorative Models blue", () => {
  const ruleBodies = (css: string, selector: string): string[] => {
    const escapedSelector = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    return [...css.matchAll(new RegExp(`${escapedSelector}\\s*\\{([^}]*)\\}`, "g"))].map((match) => match[1] ?? "");
  };
  const ruleBodyContaining = (css: string, selector: string, declaration: string): string => {
    const body = ruleBodies(css, selector).find((candidate) => candidate.includes(declaration));
    assert.ok(body, `${selector} should have a CSS rule containing ${declaration}`);
    return body;
  };

  const headerRule = ruleBodyContaining(styleCss, "header[data-shared-header]", "backdrop-filter");
  assert.match(headerRule, /border:\s*1px solid rgba\(255,\s*255,\s*255,\s*0\.07\)/);
  assert.match(headerRule, /background:\s*rgba\(15,\s*18,\s*22,\s*0\.82\)/);
  assert.match(headerRule, /backdrop-filter:\s*blur\(18px\)/);

  const navControlRule = ruleBodyContaining(styleCss, "[data-actions] a[data-button]", "min-height");
  assert.match(navControlRule, /min-height:\s*32px/);
  assert.match(navControlRule, /padding:\s*5px 10px/);
  assert.match(navControlRule, /border-radius:\s*7px/);
  assert.match(navControlRule, /background:\s*transparent/);
  assert.match(navControlRule, /color:\s*var\(--muted\)/);

  const activeNavRule = ruleBodyContaining(styleCss, '[data-actions] a[aria-current="page"]', "background");
  assert.match(activeNavRule, /background:\s*rgba\(255,\s*255,\s*255,\s*0\.1\)/);
  assert.match(activeNavRule, /color:\s*var\(--text\)/);

  const cssColorLiterals = modelsCss.match(/#[\da-f]{3,8}\b|rgba?\([^)]*\)/gi) ?? [];
  const colorChannels = (literal: string): [number, number, number] | null => {
    if (literal.startsWith("#")) {
      const hex = literal.slice(1);
      const rgb = hex.length === 3 || hex.length === 4
        ? hex.slice(0, 3).split("").map((channel) => channel.repeat(2)).join("")
        : hex.slice(0, 6);
      if (rgb.length !== 6) return null;
      return [0, 2, 4].map((offset) => Number.parseInt(rgb.slice(offset, offset + 2), 16)) as [number, number, number];
    }
    const match = literal.match(
      /^rgba?\(\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)/i,
    );
    return match ? [Number(match[1]), Number(match[2]), Number(match[3])] : null;
  };
  const decorativeBlues = cssColorLiterals.filter((literal) => {
    const channels = colorChannels(literal);
    if (!channels) return false;
    const [red, green, blue] = channels;
    return blue - red >= 24 && blue - green >= 12;
  });
  assert.deepEqual(decorativeBlues, [], "models.css should use neutral shared-console colors");
});

Deno.test("published guidance documents endpoint-specific output caps and repository-local auth uploads", () => {
  for (const publishedText of [readmeText, llmsFullText]) {
    assert.match(publishedText, /`max_completion_tokens` is the OpenAI Chat Completions cap/);
    assert.match(publishedText, /`max_output_tokens` is the OpenAI Responses cap/);
    assert.match(
      publishedText,
      /Chat Completions to Codex[\s\S]*translated to the Codex Responses field `max_output_tokens`/,
    );
    assert.match(
      publishedText,
      /Responses to Codex[\s\S]*`max_output_tokens` is forwarded as `max_output_tokens`/,
    );
    assert.match(
      publishedText,
      /Chat Completions to Cerebras \(`gpt-oss-120b`\)[\s\S]*forwarded unchanged/,
    );
    assert.match(
      publishedText,
      /Paid fallback \(Metered or Surplus\)[\s\S]*Chat `max_completion_tokens` arrives as `max_output_tokens`/,
    );
    assert.match(publishedText, /Do not swap these fields between endpoints/);
  }

  assert.doesNotMatch(readmeText, /cd lib\/ai\.ubq\.fi/);
  assert.match(readmeText, /git rev-parse --show-toplevel/);
  assert.match(llmsFullText, /scripts\/upload-codex-auth\.ts/);
  assert.match(
    llmsFullText,
    /deno task upload:auth --url https:\/\/ai\.ubq\.fi --auth-json ~\/\.codex\/auth\.json/,
  );
});

Deno.test("models page labels provider counts as catalog entries, not inference availability", () => {
  assert.match(modelsHtml, /Cataloged text models and the providers that list them/);
  assert.match(modelsHtml, /does not guarantee\s+inference availability or remaining quota/);
  assert.doesNotMatch(modelsHtml, /Live upstream catalog/);
  assert.match(modelsScript, /cataloged model/);
  assert.match(modelsScript, /cataloged models/);
  assert.doesNotMatch(modelsScript, /available models/);
});

Deno.test("public brand logos are inline and inherit the page foreground", async () => {
  assert.match(styleCss, /\[data-logo\]\s*\{[^}]*color: inherit;/);
  const sourcePath = companyLogoSvg.match(/<path\b[\s\S]*?\bd="([^"]+)"/)?.[1];
  assert.ok(sourcePath, "the source logo must define a path");

  for (
    const [path, sourceHtml] of [
      ["/about", aboutHtml],
      ["/contact", contactHtml],
      ["/developers", developersHtml],
      ["/models", modelsHtml],
      ["/privacy", privacyHtml],
    ]
  ) {
    const response = await handleStaticAsset(path);
    assert.equal(response?.status, 200, `${path} must be publicly served`);
    const html = await response!.text();
    assert.equal(html, sourceHtml, `${path} must serve the inline brand markup`);
    assert.doesNotMatch(html, /<img\b[^>]*(?:data-logo|data-models-logo)[^>]*>/, `${path} must not embed its logo`);
    assert.match(
      html,
      /<svg\b(?=[^>]*\bdata-logo\b)[^>]*>[\s\S]*?<path\b[\s\S]*?fill="currentColor"/,
      `${path} must use an inline currentColor logo`,
    );
    const inlinePath = html.match(/<svg\b(?=[^>]*\bdata-logo\b)[^>]*>[\s\S]*?<path\b[\s\S]*?\bd="([^"]+)"/)?.[1];
    assert.equal(inlinePath, sourcePath, `${path} must preserve the company logo path`);
  }
});

Deno.test("public agent-readiness pages and crawl artifacts are registered", () => {
  for (
    const path of [
      "/developers",
      "/developers.html",
      "/about",
      "/about.html",
      "/contact",
      "/contact.html",
      "/privacy",
      "/privacy.html",
      "/robots.txt",
      "/sitemap.xml",
    ]
  ) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});

Deno.test("admin provider view places capacity history before current providers", () => {
  const listIndex = adminHtml.indexOf('id="provider-capacity-list"');
  const chartIndex = adminHtml.indexOf('id="provider-capacity-chart"');
  assert.ok(chartIndex >= 0);
  assert.ok(listIndex > chartIndex);

  assert.match(adminHtml, /Provider analytics/);
  assert.match(adminHtml, /Fifteen-minute capacity, cached-input, and cache-write history/);
  assert.match(adminHtml, /admin\.css\?v=admin-polish-20260903-v5/);
  assert.match(adminHtml, /admin\.js\?v=admin-indexeddb-cache-20260903-v8/);
  assert.doesNotMatch(adminHtml, /removed_provider-failover|debug-routing/);
  assert.doesNotMatch(adminScript, /RemovedProviderFailover|refresh=live/);
  assert.match(adminScript, /fetch\(apiUrl\("\/admin\/providers\/capacity"\)/);
  assert.match(adminScript, /Cached input share/);
  assert.match(adminScript, /written to cache/);
  assert.match(adminScript, /cache_write_input_tokens/);
  assert.match(adminScript, /cacheWriteFieldsMissing/);
  assert.match(adminScript, /snapshot\?\.prompt_cache\?\.buckets/);
  assert.match(adminScript, /snapshot\?\.prompt_cache\?\.status !== "ready"/);
  assert.match(adminScript, /cache analytics unavailable/);
  assert.match(adminScript, /const loadId = \+\+providersLoadId/);
  assert.match(adminScript, /if \(loadId !== providersLoadId\) return/);
  assert.match(adminScript, /cache: "no-store"/);
  assert.match(adminScript, /Authorization: `Bearer \$\{token\}`/);
  assert.match(adminScript, /auth\.js\?v=passkey-relay-20260823-v5/);
  assert.match(adminScript, /credentials: "include"/);
  assert.match(
    adminScript,
    /if \(!getAdminToken\(\) \|\| \(isRemoteRelayOrigin\(\) && relaySessionActive\)\) headers\.delete\("Authorization"\)/,
  );
  const cookieFirstIndex = adminScript.indexOf("authResult = await requestAuth({});");
  const bearerFallbackIndex = adminScript.indexOf(
    "authResult = await requestAuth({ Authorization: `Bearer ${token}` });",
  );
  assert.ok(cookieFirstIndex >= 0);
  assert.ok(bearerFallbackIndex > cookieFirstIndex);
  assert.match(
    adminScript,
    /relaySessionActive = false;\s+const authenticated = await testAdminToken\(\{ allowBearerFallback: false \}\)/,
  );
  assert.match(adminScript, /testAdminToken\(\{ allowBearerFallback: false \}\)/);
  assert.match(adminScript, /authResult\.data\?\.auth\?\.method\?\.kind === "passkey_session"/);
  assert.match(adminScript, /if \(relaySessionActive\) return ""/);
  assert.match(adminScript, /token: isRemoteRelayOrigin\(\) && relaySessionActive \? "" : token/);
  assert.match(adminScript, /if \(isRemoteRelayOrigin\(\) && relaySessionActive\) \{/);
  assert.match(adminScript, /authenticated: true/);
  assert.doesNotMatch(adminScript, /token: result\.token/);
  assert.doesNotMatch(adminScript, /applySignedInToken\(relay\.token/);
  assert.match(adminScript, /!adminAccessState\.isAdmin \|\| !hasAdminCredential\(\)/);
  assert.match(authScript, /relay_session !== true/);
  assert.match(authScript, /credentials: init\?\.credentials \?\? "include"/);
});

Deno.test("admin error tab opens its view", () => {
  assert.match(
    adminScript,
    /viewTabErrors\.addEventListener\("click", \(\) => setAdminView\("errors", \{ hashMode: "push", focusAuth: true \}\)\)/,
  );
  assert.match(adminScript, /const loadId = \+\+errorsLoadId/);
  assert.match(adminScript, /if \(loadId !== errorsLoadId\) return/);
  assert.match(adminScript, /invalidateAdminErrors\("Sign in to load gateway errors\."\)/);
  assert.match(adminScript, /invalidateAdminErrors\("Target changed\. Sign in to load gateway errors\."\)/);
});

Deno.test("admin responses use a scoped IndexedDB stale cache", () => {
  assert.match(adminCacheScript, /const DATABASE_NAME = "uos_ai\.admin-cache";/);
  assert.match(adminCacheScript, /store\.createIndex\(SCOPE_INDEX, "scope", \{ unique: false \}\)/);
  assert.match(adminCacheScript, /objectStore\(STORE_NAME\)\.get/);
  assert.doesNotMatch(adminCacheScript, /localStorage|Authorization|Cookie/);

  assert.match(
    adminScript,
    /import \{ createAdminSnapshotCache \} from "\.\/admin-cache\.js\?v=admin-indexeddb-cache-20260830-v7";/,
  );
  assert.match(adminScript, /const cacheFreshAdminRead = \(response, cacheKey, scope, epoch\) =>/);
  assert.match(adminScript, /if \(!response\.ok \|\| !cacheKey \|\| !scope/);
  assert.match(adminScript, /const hydrateAdminSnapshots = async \(\) =>/);
  assert.match(
    adminScript,
    /setAdminSnapshotScopeFromAuth\(data\.auth\);\s+setAdminAccessState\(\{ checked: true, isAdmin: true, isSuperAdmin \}\);\s+void hydrateAdminSnapshots\(\);/,
  );
  assert.match(adminScript, /globalThis\.requestAnimationFrame\(\(\) => \{\s+globalThis\.setTimeout/);
  assert.doesNotMatch(adminScript, /showPendingAdminViewAfterPrefetch/);
  assert.match(
    adminScript,
    /adminAccessState\.checked && adminAccessState\.isAdmin && currentAdminView === ADMIN_VIEW_DEFAULT\) \{\s+setAdminView\(ADMIN_VIEW_AUTHENTICATED_DEFAULT, \{ allowInaccessible: false \}\);/,
  );
  assert.match(adminScript, /"Cached · refreshing"/);
  assert.match(adminScript, /setKeysBadge\("ok", view === "all" \? "No API keys" : `No \$\{view\} API keys`\);/);
  assert.match(adminScript, /const readCachedApiKeyRequestLogs = async \(keyId\) =>/);
  assert.match(adminScript, /renderApiKeyRequestLogs\(panel, list, summary, cached\.records, "Cached · refreshing"\);/);
  assert.match(adminScript, /const inFlightEntry = \{ scope, epoch, request: null \};/);
  assert.match(
    adminScript,
    /isCurrentApiKeyRequestLogCacheEntry\(cached, scope, epoch\)[\s\S]*now - cached\.fetchedAt < API_KEY_REQUEST_LOGS_TTL_MS/,
  );
  assert.match(
    adminScript,
    /if \(isCurrentApiKeyRequestLogScope\(scope, epoch\)\) \{\s+apiKeyRequestLogCache\.set\(cacheKey, \{[\s\S]*scope,[\s\S]*epoch,/,
  );
  assert.match(
    adminScript,
    /if \(apiKeyRequestLogPromises\.get\(cacheKey\) === inFlightEntry\) \{\s+apiKeyRequestLogPromises\.delete\(cacheKey\);/,
  );
  assert.match(
    adminScript,
    /const scope = adminCacheScope;\s+const epoch = adminCacheEpoch;\s+const isCurrentPanel = \(\) =>[\s\S]*isCurrentApiKeyRequestLogScope\(scope, epoch\);/,
  );
  assert.match(
    adminScript,
    /const cached = await readCachedApiKeyRequestLogs\(currentKeyId\);\s+if \(!isCurrentPanel\(\)\)/,
  );
  assert.match(adminScript, /const response = await refresh;\s+if \(!isCurrentPanel\(\)\)/);
  assert.match(adminScript, /clearAdminSnapshotScope\(\);/);
});

Deno.test("admin boot probes server auth mode without requiring a browser token", () => {
  assert.doesNotMatch(adminScript, /if \(!token && !relaySessionActive && !isRemoteRelayOrigin\(\)\)/);
  assert.match(adminScript, /requestAuth\(token \? \{ Authorization: `Bearer \$\{token\}` \} : \{\}\)/);
  assert.match(adminScript, /resetAdminPrefetchState\("Checking admin session\.\.\."\)/);
});

Deno.test("expanded auth widget keeps its mobile toggle right-aligned", () => {
  assert.match(
    adminCss,
    /\[data-auth-widget\]\s*\{[^}]*grid-template-columns:\s*minmax\(0, 1fr\)/,
  );
  assert.match(
    adminCss,
    /#auth-widget-toggle\s*\{[^}]*grid-column:\s*1;[^}]*justify-self:\s*end/,
  );
  assert.match(adminCss, /#auth-widget-panel\s*\{[^}]*grid-column:\s*1/);
});

Deno.test("provider analytics graph reports inference 5xx buckets", () => {
  assert.match(adminScript, /label: "Failed inference responses \(HTTP 5xx\)"/);
  assert.match(adminScript, /five_xx_buckets/);
  assert.match(adminScript, /marker\.dataset\.capacityInferenceError/);
  assert.doesNotMatch(adminScript, /marker\.dataset\.capacityInference5xx/);
  assert.match(adminCss, /\[data-capacity-inference-error\] path/);
  assert.match(adminCss, /\[data-capacity-legend-item="inference-error"\]/);
  assert.match(adminScript, /15-minute bucket starting/);
  assert.match(adminScript, /Failed inference buckets/);
  assert.match(adminHtml, /Red diamonds mark 15-minute buckets containing failed inference responses \(HTTP 5xx\)/);
});

Deno.test("provider analytics graph preserves its SVG text aspect ratio", () => {
  assert.match(adminScript, /preserveAspectRatio: "xMinYMin meet"/);
  assert.match(adminScript, /viewBox: `0 0 \$\{width\} \$\{height\}`,[\s\S]*?width,[\s\S]*?height,/);
  assert.match(
    adminCss,
    /\[data-capacity-chart-svg\]\s*\{[\s\S]*?height:\s*var\(--capacity-chart-height-px, 180px\)/,
  );
  assert.doesNotMatch(
    adminCss,
    /\[data-capacity-chart-svg\]\s*\{[^}]*height:\s*clamp\(/,
  );
});

Deno.test("static assets register autonomous agent discovery documents", () => {
  for (const path of ["/llms.txt", "/llms-full.txt", "/docs/llms-agents.md", "/openapi.json"]) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});

Deno.test("OpenAPI discovery contract describes the public inference API", () => {
  const document = JSON.parse(openApiText);

  assert.equal(document.openapi, "3.1.0");
  assert.equal(document.servers[0].url, "https://ai.ubq.fi");
  assert.ok(document.components.securitySchemes.bearerAuth);
  for (
    const [path, method] of [
      ["/health", "get"],
      ["/v1/models", "get"],
      ["/v1/chat/completions", "post"],
      ["/v1/responses", "post"],
      ["/v1/images/generations", "post"],
      ["/v1/images/edits", "post"],
      ["/uos/models/capabilities", "get"],
    ]
  ) {
    const operation = document.paths[path][method];
    assert.equal(typeof operation.operationId, "string", `${method.toUpperCase()} ${path} needs an operation id`);
    assert.ok(operation.description.length > 80, `${method.toUpperCase()} ${path} needs a useful description`);
  }
  assert.equal(
    document.components.schemas.ChatCompletionRequest.properties.tools.items.$ref,
    "#/components/schemas/FunctionTool",
  );
  assert.equal(
    document.components.schemas.ChatCompletionRequest.properties.tool_choice.$ref,
    "#/components/schemas/ToolChoice",
  );
  assert.equal(
    document.components.schemas.ResponseRequest.properties.tools.items.$ref,
    "#/components/schemas/ResponseFunctionTool",
  );
  assert.equal(
    document.components.schemas.ResponseRequest.properties.tool_choice.$ref,
    "#/components/schemas/ResponseToolChoice",
  );
  assert.equal(document.components.schemas.ResponseRequest.required, undefined);
  const responseFunctionTool = document.components.schemas.ResponseFunctionTool;
  assert.deepEqual(responseFunctionTool.required, ["type", "name"]);
  assert.ok(responseFunctionTool.properties.name);
  assert.ok(responseFunctionTool.properties.description);
  assert.ok(responseFunctionTool.properties.parameters);
  assert.equal(responseFunctionTool.properties.function, undefined);
  const responseForcedToolChoice = document.components.schemas.ResponseToolChoice.oneOf[1];
  assert.deepEqual(responseForcedToolChoice.required, ["type", "name"]);
  assert.ok(responseForcedToolChoice.properties.name);
  assert.equal(responseForcedToolChoice.properties.function, undefined);
  assert.ok(document.components.headers.RateLimit);
  assert.ok(document.components.headers.RateLimitPolicy);
  assert.ok(document.components.responses.RateLimited.headers["Retry-After"]);
  assert.ok(document.components.responses.RateLimited.headers.RateLimit);

  const generation = document.paths["/v1/images/generations"].post;
  const edit = document.paths["/v1/images/edits"].post;
  assert.equal(generation.operationId, "createImage");
  assert.equal(edit.operationId, "createImageEdit");
  assert.deepEqual(Object.keys(generation.requestBody.content), ["application/json"]);
  assert.deepEqual(
    Object.keys(edit.requestBody.content).sort(),
    ["application/json", "multipart/form-data"],
  );
  assert.equal(
    generation.requestBody.content["application/json"].schema.$ref,
    "#/components/schemas/ImageGenerationRequest",
  );
  assert.equal(
    edit.requestBody.content["application/json"].schema.$ref,
    "#/components/schemas/ImageEditJsonRequest",
  );
  assert.equal(
    edit.requestBody.content["multipart/form-data"].schema.$ref,
    "#/components/schemas/ImageEditMultipartRequest",
  );
  assert.deepEqual(document.components.schemas.ImageGenerationRequest.required, ["prompt"]);
  assert.deepEqual(document.components.schemas.ImageEditJsonRequest.required, ["images", "prompt"]);
  assert.deepEqual(document.components.schemas.ImageEditMultipartRequest.required, ["prompt"]);
  assert.deepEqual(document.components.schemas.ImageEditMultipartRequest.anyOf, [
    { required: ["image"] },
    { required: ["image[]"] },
  ]);
  assert.deepEqual(
    document.components.schemas.ImageEditMultipartRequest.properties["image[]"].oneOf,
    document.components.schemas.ImageEditMultipartRequest.properties.image.oneOf,
  );
  assert.equal(
    document.components.schemas.ImageEditJsonRequest.properties.images.items.$ref,
    "#/components/schemas/ImageRef",
  );
  assert.equal(
    document.components.schemas.ImageEditJsonRequest.properties.mask.$ref,
    "#/components/schemas/ImageMaskRef",
  );
  const imageRef = document.components.schemas.ImageRef;
  assert.deepEqual(imageRef.required, ["image_url"]);
  assert.equal(imageRef.anyOf, undefined);
  assert.equal(imageRef.not, undefined);
  assert.equal(imageRef.additionalProperties, false);
  assert.equal(imageRef.properties.image_url.format, "uri");
  assert.equal(imageRef.properties.image_url.minLength, 1);
  assert.equal(imageRef.properties.image_url.maxLength, 20_971_520);
  assert.equal(
    imageRef.properties.image_url.pattern,
    "^(?:[hH][tT][tT][pP][sS]?://|[dD][aA][tT][aA]:[iI][mM][aA][gG][eE]/(?:[pP][nN][gG]|[xX]-[pP][nN][gG]|[jJ][pP](?:[eE][gG]|[gG])|[wW][eE][bB][pP]);[bB][aA][sS][eE]64,)",
  );
  assert.equal(imageRef.properties.file_id, undefined);
  const imageMaskRef = document.components.schemas.ImageMaskRef;
  assert.deepEqual(imageMaskRef.required, ["image_url"]);
  assert.equal(imageMaskRef.anyOf, undefined);
  assert.equal(imageMaskRef.not, undefined);
  assert.equal(imageMaskRef.additionalProperties, false);
  assert.equal(imageMaskRef.properties.image_url.maxLength, 20_971_520);
  assert.equal(
    imageMaskRef.properties.image_url.pattern,
    "^[dD][aA][tT][aA]:[iI][mM][aA][gG][eE]/[pP][nN][gG];[bB][aA][sS][eE]64,",
  );
  assert.equal(imageMaskRef.properties.file_id, undefined);
  const generationSchema = document.components.schemas.ImageGenerationRequest;
  const editJsonSchema = document.components.schemas.ImageEditJsonRequest;
  const editMultipartSchema = document.components.schemas.ImageEditMultipartRequest;
  assert.deepEqual(generationSchema.properties.n.type, ["integer", "null"]);
  assert.equal(generationSchema.properties.model.default, "gpt-image-1");
  assert.equal(editJsonSchema.properties.model.default, "gpt-image-1.5");
  assert.equal(editMultipartSchema.properties.model.default, "gpt-image-1.5");
  assert.match(editMultipartSchema.description, /together they must total no more than 50 MiB/);
  assert.deepEqual(editJsonSchema.properties.input_fidelity.type, [
    "string",
    "null",
  ]);
  for (const schema of [generationSchema, editJsonSchema, editMultipartSchema]) {
    assert.equal(schema.additionalProperties, false);
    assert.equal(schema.properties.prompt.maxLength, 32_000);
    assert.equal(schema.properties.user.type, "string");
    assert.match(schema.properties.user.description, /x-uos-warning: user_ignored/);
    assert.deepEqual(schema.properties.stream.enum, [false, null]);
    assert.deepEqual(schema.properties.output_compression.type, ["integer", "null"]);
  }
  assert.deepEqual(generationSchema.properties.response_format.enum, ["b64_json", null]);
  assert.equal(generationSchema.properties.style, undefined);
  assert.deepEqual(
    generationSchema.properties.quality.enum,
    ["low", "medium", "high", "auto", null],
  );
  assert.deepEqual(editJsonSchema.properties.size.type, ["string", "null"]);
  assert.equal(editJsonSchema.properties.size.minLength, 1);
  assert.equal(editJsonSchema.properties.size.enum, undefined);
  assert.deepEqual(editJsonSchema.properties.quality.enum, ["low", "medium", "high", "auto", null]);
  assert.deepEqual(
    editMultipartSchema.properties.quality.enum,
    ["low", "medium", "high", "auto", null],
  );
  assert.equal(editMultipartSchema.properties.moderation, undefined);
  assert.deepEqual(editMultipartSchema.properties.response_format.enum, ["b64_json", null]);
  assert.equal(
    document.components.schemas.ImagesResponse.properties.data.items.$ref,
    "#/components/schemas/Image",
  );
  assert.ok(document.components.schemas.ImagesResponse.properties.output_format);
  assert.equal(document.components.schemas.Image.properties.output_format, undefined);
});

Deno.test("published agent contracts distinguish bodyless catalog revalidation and conditional rate limits", () => {
  const document = JSON.parse(openApiText);
  const developerText = developersHtml.replace(/<[^>]+>/g, " ").replace(/\s+/g, " ");
  const modelResponses = document.paths["/v1/models"].get.responses;
  const modelList = modelResponses["200"];
  const notModified = document.paths["/v1/models"].get.responses["304"];

  assert.equal(modelList.headers.ETag.$ref, "#/components/headers/ETag");
  assert.equal(notModified.headers.ETag.$ref, "#/components/headers/ETag");
  assert.match(document.components.headers.ETag.description, /versioned 200 and 304 responses/i);
  assert.match(document.components.headers.ETag.description, /omitted from the unversioned/i);
  assert.equal(notModified.content, undefined);
  assert.match(notModified.description, /no response body; reuse the cached catalog/i);
  assert.match(
    developerText,
    /matching If-None-Match request to GET \/v1\/models\?client_version=X\.Y\.Z can return 304 Not Modified with no body/i,
  );
  assert.match(llmsFullText, /matching `If-None-Match` requests receive `304 Not Modified`\s+with no body/i);

  for (const publishedText of [openApiText, llmsFullText]) {
    assert.doesNotMatch(publishedText, /RFC 9449/);
    assert.match(publishedText, /draft-ietf-httpapi-ratelimit-headers-11/);
  }
  assert.match(document.components.headers.RetryAfter.description, /when present/i);
  assert.match(document.components.responses.RateLimited.description, /Retry-After when present/i);
  assert.match(developerText, /For a 429 error, honor Retry-After when present/i);
  assert.match(llmsText, /honor `Retry-After` when present/);
});

Deno.test("agent discovery documents are served with useful media types", async () => {
  const llms = await handleStaticAsset("/llms.txt");
  const llmsFull = await handleStaticAsset("/llms-full.txt");
  const markdown = await handleStaticAsset("/docs/llms-agents.md");
  const openapi = await handleStaticAsset("/openapi.json");

  assert.equal(llms?.status, 200);
  assert.equal(llms?.headers.get("content-type"), "text/plain; charset=utf-8");
  assert.match(await llms!.text(), /https:\/\/ai\.ubq\.fi\/openapi\.json/);
  assert.equal(llmsFull?.headers.get("content-type"), "text/plain; charset=utf-8");
  assert.equal(markdown?.headers.get("content-type"), "text/markdown; charset=utf-8");
  assert.equal(openapi?.headers.get("content-type"), "application/json; charset=utf-8");
});

Deno.test("homepage content negotiation serves server-rendered HTML, Markdown, and explicit JSON", async () => {
  const generic = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "*/*" } }));
  const html = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "text/html" } }));
  const xhtml = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "application/xhtml+xml" } }));
  const markdown = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "text/markdown" } }));
  const unacceptable = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "image/png" } }));

  for (const response of [generic, html, xhtml]) {
    assert.equal(response.headers.get("content-type"), "text/html; charset=utf-8");
    assert.equal(response.headers.get("vary"), "Accept, Accept-Encoding");
  }
  const rawHtml = await html.text();
  assert.match(rawHtml, /<h1>UbiquityOS AI Gateway<\/h1>/);
  const readableText = rawHtml.replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim();
  assert.ok(readableText.length >= 500);
  assert.ok(readableText.length / rawHtml.length >= 0.05, "homepage should keep readable text above 5% of HTML");

  assert.equal(markdown.headers.get("content-type"), "text/markdown; charset=utf-8");
  assert.equal(markdown.headers.get("vary"), "Accept, Accept-Encoding");
  assert.match(await markdown.text(), /^# UbiquityOS AI Gateway/m);

  assert.equal(unacceptable.status, 406);
  assert.equal(unacceptable.headers.get("content-type"), "application/json");
  assert.equal((await unacceptable.json()).error.code, "not_acceptable");
});

Deno.test("public HEAD routes preserve GET metadata without a response body", async () => {
  const routes = [
    ["/", { Accept: "text/html" }],
    ["/index.html", {}],
    ["/style.css", {}],
    ["/docs/llms-agents.md", {}],
    ["/openapi.json", {}],
  ] as const;

  for (const [path, headers] of routes) {
    const getResponse = await handler(new Request("https://ai.ubq.fi" + path, { headers }));
    const headResponse = await handler(
      new Request("https://ai.ubq.fi" + path, { method: "HEAD", headers }),
    );

    assert.equal(headResponse.status, getResponse.status, path);
    assert.deepEqual(
      [...headResponse.headers].filter(([name]) => name !== "x-uos-request-id"),
      [...getResponse.headers].filter(([name]) => name !== "x-uos-request-id"),
      path,
    );
    assert.equal(await headResponse.text(), "", path);
  }
});

Deno.test("unknown public HEAD routes remain 404", async () => {
  const response = await handler(new Request("https://ai.ubq.fi/not-a-real-public-route", { method: "HEAD" }));

  assert.equal(response.status, 404);
  assert.equal(await response.text(), "");
});

Deno.test("credentialed root responses retain content-negotiation Vary tokens", async () => {
  const response = await handler(
    new Request("https://ai.ubq.fi/", {
      headers: {
        Accept: "text/html",
        Origin: "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net",
      },
    }),
  );
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("vary"), "Accept, Accept-Encoding, Origin");
});

Deno.test("JSON root advertises autonomous agent discovery documents", async () => {
  const response = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "application/json" } }));
  const body = await response.json();

  assert.equal(response.headers.get("vary"), "Accept, Accept-Encoding");
  assert.equal(body.name, "UbiquityOS AI Gateway");
  assert.equal(body.discovery.llms, "/llms.txt");
  assert.equal(body.discovery.llms_full, "/llms-full.txt");
  assert.equal(body.discovery.openapi, "/openapi.json");
  assert.equal(body.discovery.developers, "/developers");
  assert.equal(body.discovery.sitemap, "/sitemap.xml");
});

Deno.test("homepage metadata and agent guidance expose the canonical service identity", () => {
  assert.match(indexHtml, /<html lang="en">/);
  assert.match(indexHtml, /<link rel="canonical" href="https:\/\/ai\.ubq\.fi\/" \/>/);
  assert.match(indexHtml, /<meta property="og:type" content="website" \/>/);
  assert.match(indexHtml, /<meta property="og:image" content="https:\/\/ai\.ubq\.fi\/favicon\.png" \/>/);
  for (const path of ["/developers", "/openapi.json", "/llms.txt", "/llms-full.txt"]) {
    assert.match(indexHtml, new RegExp(`href="${path}"`), `homepage should link ${path}`);
  }
  const jsonLdMatch = indexHtml.match(/<script type="application\/ld\+json">\s*([\s\S]*?)\s*<\/script>/);
  if (!jsonLdMatch?.[1]) throw new Error("homepage should include JSON-LD");
  const jsonLd = JSON.parse(jsonLdMatch[1]);
  assert.ok(jsonLd.some((entry: { "@type"?: string }) => entry["@type"] === "SoftwareApplication"));
  assert.ok(jsonLd.some((entry: { "@type"?: string }) => entry["@type"] === "Organization"));
  assert.match(llmsText, /## When to use this service/);
  assert.match(llmsText, /https:\/\/ai\.ubq\.fi\/developers/);
});

Deno.test("trust pages and crawl artifacts are public, substantial, and well formed", async () => {
  for (const path of ["/developers", "/about", "/contact", "/privacy"]) {
    const response = await handleStaticAsset(path);
    assert.equal(response?.status, 200, `${path} should be public`);
    assert.equal(response?.headers.get("content-type"), "text/html; charset=utf-8");
    const body = await response!.text();
    assert.ok(body.replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim().length >= 500, `${path} should have real text`);
  }

  const robots = await handleStaticAsset("/robots.txt");
  const sitemap = await handleStaticAsset("/sitemap.xml");
  assert.equal(robots?.headers.get("content-type"), "text/plain; charset=utf-8");
  assert.match(await robots!.text(), /Sitemap: https:\/\/ai\.ubq\.fi\/sitemap\.xml/);
  assert.equal(sitemap?.headers.get("content-type"), "application/xml; charset=utf-8");
  const sitemapText = await sitemap!.text();
  assert.match(sitemapText, /^<\?xml version="1\.0" encoding="UTF-8"\?>/);
  for (const path of ["", "/developers", "/docs", "/about", "/contact", "/privacy"]) {
    const sitemapPath = path || "/";
    assert.match(sitemapText, new RegExp(`<loc>https://ai\\.ubq\\.fi${sitemapPath}<\\/loc>`));
  }
  assert.match(sitemapText, /<lastmod>2026-08-21<\/lastmod>/);
});

Deno.test("public delivery returns structured JSON errors and exposes rate-limit fields to browsers", async () => {
  const missing = await handler(new Request("https://ai.ubq.fi/not-a-real-public-route"));
  assert.equal(missing.status, 404);
  assert.equal(missing.headers.get("content-type"), "application/json");
  const body = await missing.json();
  assert.equal(body.error.code, "not_found");
  assert.match(body.error.message, /Not found/);

  const exposed = new Headers(corsHeaders()).get("Access-Control-Expose-Headers") ?? "";
  for (const header of ["RateLimit", "RateLimit-Policy", "RateLimit-Limit", "RateLimit-Remaining", "RateLimit-Reset"]) {
    assert.match(exposed, new RegExp(header));
  }
});
