import assert from "node:assert/strict";

import handler from "../src/handler.ts";
import { corsHeaders } from "../src/http.ts";
import { handleRoot, handleStaticAsset, hasStaticAsset } from "../src/static.ts";
import adminHtml from "../static/admin.html" with { type: "text" };
import aboutHtml from "../static/about.html" with { type: "text" };
import adminScript from "../static/admin.js" with { type: "text" };
import companyLogoSvg from "../static/company-logo.svg" with { type: "text" };
import contactHtml from "../static/contact.html" with { type: "text" };
import developersHtml from "../static/developers.html" with { type: "text" };
import indexHtml from "../static/index.html" with { type: "text" };
import llmsText from "../static/llms.txt" with { type: "text" };
import modelsHtml from "../static/models.html" with { type: "text" };
import openApiText from "../static/openapi.json" with { type: "text" };
import privacyHtml from "../static/privacy.html" with { type: "text" };
import styleCss from "../static/style.css" with { type: "text" };

Deno.test("static assets register frontend module dependencies", () => {
  for (
    const path of [
      "/admin.js",
      "/auth.js",
      "/auth-relay.js",
      "/foreground-refresh.js",
      "/network.js",
      "/models.js",
      "/models.css",
      "/reasoning-select.js",
    ]
  ) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});

Deno.test("public models page is registered", () => {
  assert.equal(hasStaticAsset("/models"), true);
  assert.equal(hasStaticAsset("/models.html"), true);
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
  assert.match(adminHtml, /admin\.js\?v=prompt-cache-analytics-v2-20260822/);
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
