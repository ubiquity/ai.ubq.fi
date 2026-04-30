import "./network.js";

const contentEl = document.querySelector("[data-docs-content]");
const tocEl = document.querySelector("[data-docs-toc]");
const source = contentEl?.dataset.docsSource;

const escapeHtml = (value) =>
  value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");

const escapeAttr = (value) => escapeHtml(value).replaceAll("`", "&#96;");

const sanitizeUrl = (raw) => {
  const trimmed = raw.trim();
  if (!trimmed) return "#";
  if (/^(https?:|mailto:|\/|#)/i.test(trimmed)) return trimmed;
  return "#";
};

const renderLinks = (text) => {
  const linkRegex = /\[([^\]]+)\]\(([^)]+)\)/g;
  let result = "";
  let lastIndex = 0;
  for (const match of text.matchAll(linkRegex)) {
    const index = match.index ?? 0;
    const label = match[1] ?? "";
    const url = match[2] ?? "";
    result += escapeHtml(text.slice(lastIndex, index));
    result += `<a href="${escapeAttr(sanitizeUrl(url))}" rel="noreferrer">${escapeHtml(label)}</a>`;
    lastIndex = index + match[0].length;
  }
  result += escapeHtml(text.slice(lastIndex));
  return result;
};

const renderInline = (text) => {
  const parts = text.split("`");
  return parts
    .map((part, index) => {
      if (index % 2 === 1) {
        return `<code>${escapeHtml(part)}</code>`;
      }
      return renderLinks(part);
    })
    .join("");
};

const slugify = (text, counts) => {
  const base = text
    .toLowerCase()
    .replace(/[^a-z0-9\s-]/g, "")
    .trim()
    .replace(/\s+/g, "-")
    .replace(/-+/g, "-");
  const count = (counts.get(base) ?? 0) + 1;
  counts.set(base, count);
  if (count === 1) return base || "section";
  return `${base || "section"}-${count}`;
};

const parseMarkdown = (markdown) => {
  const lines = markdown.replace(/\r\n/g, "\n").split("\n");
  const html = [];
  const toc = [];
  const headingCounts = new Map();
  let inCode = false;
  let codeLang = "";
  let codeLines = [];
  let paragraph = [];
  let listType = null;

  const flushParagraph = () => {
    if (!paragraph.length) return;
    const text = paragraph.join(" ").trim();
    if (text) html.push(`<p>${renderInline(text)}</p>`);
    paragraph = [];
  };

  const closeList = () => {
    if (!listType) return;
    html.push(`</${listType}>`);
    listType = null;
  };

  for (const rawLine of lines) {
    const line = rawLine.replace(/\s+$/g, "");
    if (inCode) {
      if (line.startsWith("```")) {
        const languageClass = codeLang ? ` class="language-${escapeAttr(codeLang)}"` : "";
        html.push(`<pre data-code><code${languageClass}>${escapeHtml(codeLines.join("\n"))}</code></pre>`);
        inCode = false;
        codeLang = "";
        codeLines = [];
      } else {
        codeLines.push(rawLine);
      }
      continue;
    }

    if (line.startsWith("```")) {
      flushParagraph();
      closeList();
      inCode = true;
      codeLang = line.slice(3).trim();
      codeLines = [];
      continue;
    }

    if (!line.trim()) {
      flushParagraph();
      closeList();
      continue;
    }

    const headingMatch = line.match(/^(#{1,4})\s+(.+)$/);
    if (headingMatch) {
      flushParagraph();
      closeList();
      const level = headingMatch[1].length;
      const text = headingMatch[2].trim();
      const id = slugify(text, headingCounts);
      if (level >= 2) {
        toc.push({ level, text, id });
      }
      html.push(`<h${level} id="${escapeAttr(id)}">${renderInline(text)}</h${level}>`);
      continue;
    }

    const listMatch = line.match(/^\s*(?:([*+-])|(\d+)\.)\s+(.+)$/);
    if (listMatch) {
      flushParagraph();
      const type = listMatch[1] ? "ul" : "ol";
      if (listType && listType !== type) {
        closeList();
      }
      if (!listType) {
        listType = type;
        html.push(`<${listType}>`);
      }
      const itemText = listMatch[3] ?? "";
      html.push(`<li>${renderInline(itemText.trim())}</li>`);
      continue;
    }

    paragraph.push(line.trim());
  }

  flushParagraph();
  closeList();

  if (inCode) {
    html.push(`<pre data-code><code>${escapeHtml(codeLines.join("\n"))}</code></pre>`);
  }

  return { html: html.join("\n"), toc };
};

const renderToc = (toc) => {
  if (!tocEl) return;
  if (!toc.length) {
    tocEl.innerHTML = "<p data-empty>No sections found.</p>";
    return;
  }
  tocEl.innerHTML = toc
    .map((entry) => {
      const label = escapeHtml(entry.text);
      const id = escapeAttr(entry.id);
      return `<a href="#${id}" data-level="${entry.level}">${label}</a>`;
    })
    .join("\n");
};

const loadDocs = async () => {
  if (!contentEl) return;
  if (!source) {
    contentEl.innerHTML = "<p>Missing docs source.</p>";
    return;
  }
  try {
    const res = await fetch(source, { cache: "no-store" });
    if (!res.ok) {
      contentEl.innerHTML = `<p>Failed to load docs (${res.status}).</p>`;
      return;
    }
    const text = await res.text();
    const { html, toc } = parseMarkdown(text);
    contentEl.innerHTML = html;
    renderToc(toc);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    contentEl.innerHTML = `<p>Failed to load docs (${escapeHtml(message)}).</p>`;
  }
};

loadDocs();
