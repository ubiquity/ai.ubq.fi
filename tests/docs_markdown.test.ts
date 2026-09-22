import assert from "node:assert/strict";
import { parseMarkdown } from "../static/docs.js";

Deno.test("parseMarkdown keeps wrapped list continuation lines inside <li>", () => {
  const markdown = [
    "- First item with a wrapped line",
    "  that continues on the second line.",
    "- Second item without wrap.",
    "1. Numbered item with wrap",
    "   that continues here.",
    "2. Numbered item two.",
  ].join("\n");

  const { html } = parseMarkdown(markdown);

  assert.equal(
    html.trim(),
    [
      "<ul>",
      "<li>First item with a wrapped line that continues on the second line.</li>",
      "<li>Second item without wrap.</li>",
      "</ul>",
      "<ol>",
      "<li>Numbered item with wrap that continues here.</li>",
      "<li>Numbered item two.</li>",
      "</ol>",
    ].join("\n")
  );
});

Deno.test("parseMarkdown closes open list on blank line, heading, code block, or table", () => {
  const markdown = [
    "- List item with continuation",
    "  still in list",
    "",
    "Paragraph after list.",
    "",
    "- Item before heading",
    "## Next Section",
    "",
    "- Item before code",
    "```bash",
    "echo ok",
    "```",
    "",
    "- Item before table",
    "| Col1 | Col2 |",
    "| --- | --- |",
    "| Val1 | Val2 |",
  ].join("\n");

  const { html, toc } = parseMarkdown(markdown);

  assert.match(html, /<ul>\s*<li>List item with continuation still in list<\/li>\s*<\/ul>/);
  assert.match(html, /<p>Paragraph after list\.<\/p>/);
  assert.match(html, /<ul>\s*<li>Item before heading<\/li>\s*<\/ul>/);
  assert.match(html, /<h2 id="next-section">Next Section<\/h2>/);
  assert.match(html, /<ul>\s*<li>Item before code<\/li>\s*<\/ul>/);
  assert.match(html, /<div data-code-block>/);
  assert.match(html, /<ul>\s*<li>Item before table<\/li>\s*<\/ul>/);
  assert.match(html, /<table/);
  assert.equal(toc.length, 1);
  assert.equal(toc[0].id, "next-section");
});

Deno.test("parseMarkdown reproduces exact llms-agents.md wrapped lines structure", () => {
  const markdown = [
    "- Admin tokens (Deno Deploy token or allowlisted admin token) are also accepted for client routes, but application",
    "  integrations should not label client credentials as `DENO_DEPLOY_TOKEN`.",
    "- GitHub tokens are accepted only when paired with kernel attestation headers.",
  ].join("\n");

  const { html } = parseMarkdown(markdown);

  assert.equal(
    html.trim(),
    [
      "<ul>",
      "<li>Admin tokens (Deno Deploy token or allowlisted admin token) are also accepted for client routes, but application integrations should not label client credentials as <code>DENO_DEPLOY_TOKEN</code>.</li>",
      "<li>GitHub tokens are accepted only when paired with kernel attestation headers.</li>",
      "</ul>",
    ].join("\n")
  );
});
