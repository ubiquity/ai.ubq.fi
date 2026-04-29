export type ExtractedCodexModels = Readonly<{
  models: Record<string, unknown>[];
  clientVersion: string | null;
}>;

const looksLikeCodexWrapper = (text: string): boolean => {
  const trimmed = text.trimStart();
  if (!trimmed.startsWith("#!/usr/bin/env node")) return false;
  return text.includes("vendorRoot") && text.includes("targetTriple") && text.includes("codexBinaryName");
};

const detectTargetTriple = (os: string, arch: string): string | null => {
  const normalizedArch = arch === "arm64" ? "aarch64" : arch;
  if (os === "darwin") {
    if (normalizedArch === "x86_64") return "x86_64-apple-darwin";
    if (normalizedArch === "aarch64") return "aarch64-apple-darwin";
    return null;
  }
  if (os === "linux" || os === "android") {
    if (normalizedArch === "x86_64") return "x86_64-unknown-linux-musl";
    if (normalizedArch === "aarch64") return "aarch64-unknown-linux-musl";
    return null;
  }
  if (os === "windows") {
    if (normalizedArch === "x86_64") return "x86_64-pc-windows-msvc";
    if (normalizedArch === "aarch64") return "aarch64-pc-windows-msvc";
  }
  return null;
};

const platformPackageName = (targetTriple: string): string | null => {
  switch (targetTriple) {
    case "x86_64-unknown-linux-musl":
      return "codex-linux-x64";
    case "aarch64-unknown-linux-musl":
      return "codex-linux-arm64";
    case "x86_64-apple-darwin":
      return "codex-darwin-x64";
    case "aarch64-apple-darwin":
      return "codex-darwin-arm64";
    case "x86_64-pc-windows-msvc":
      return "codex-win32-x64";
    case "aarch64-pc-windows-msvc":
      return "codex-win32-arm64";
    default:
      return null;
  }
};

const detectSeparator = (path: string, os: string): string => {
  if (os === "windows") return "\\";
  return path.includes("\\") ? "\\" : "/";
};

const normalizePath = (path: string, sep: string): string => {
  let drivePrefix = "";
  let rest = path;
  if (/^[A-Za-z]:/.test(path)) {
    drivePrefix = path.slice(0, 2);
    rest = path.slice(2);
  }
  const normalized = rest.replace(/[\\/]+/g, sep);
  const isAbsolute = normalized.startsWith(sep);
  const parts = normalized.split(sep);
  const stack: string[] = [];
  for (const part of parts) {
    if (!part || part === ".") continue;
    if (part === "..") {
      if (stack.length && stack[stack.length - 1] !== "..") stack.pop();
      else if (!isAbsolute) stack.push(part);
      continue;
    }
    stack.push(part);
  }
  const joined = stack.join(sep);
  const prefix = drivePrefix ? `${drivePrefix}${sep}` : isAbsolute ? sep : "";
  return `${prefix}${joined}`;
};

const dirname = (path: string, sep: string): string => {
  const normalized = normalizePath(path, sep);
  const index = normalized.lastIndexOf(sep);
  if (index <= 0) return normalized.slice(0, 1) || normalized;
  return normalized.slice(0, index);
};

const joinPath = (sep: string, ...parts: string[]): string => {
  const filtered = parts.filter((part) => part && part.trim());
  return normalizePath(filtered.join(sep), sep);
};

export const resolveCodexBinaryPath = async (
  codexPath: string,
  readTextFile: (path: string) => Promise<string>,
  os: string,
  arch: string,
  realPath?: (path: string) => Promise<string>,
  fileExists?: (path: string) => Promise<boolean>,
): Promise<string> => {
  let resolvedPath = codexPath;
  if (realPath) {
    try {
      resolvedPath = await realPath(codexPath);
    } catch {
      resolvedPath = codexPath;
    }
  }

  let wrapperText: string | null = null;
  try {
    wrapperText = await readTextFile(resolvedPath);
  } catch {
    return codexPath;
  }
  if (!wrapperText || !looksLikeCodexWrapper(wrapperText)) return codexPath;

  const targetTriple = detectTargetTriple(os, arch);
  if (!targetTriple) return codexPath;

  const sep = detectSeparator(resolvedPath, os);
  const wrapperDir = dirname(resolvedPath, sep);
  const binaryName = os === "windows" ? "codex.exe" : "codex";
  if (wrapperText.includes("PLATFORM_PACKAGE_BY_TARGET")) {
    const packageName = platformPackageName(targetTriple);
    if (packageName) {
      const packageRoot = joinPath(sep, wrapperDir, "..");
      const nodeModulesRoot = joinPath(sep, wrapperDir, "..", "..", "..");
      const nestedPath = joinPath(
        sep,
        packageRoot,
        "node_modules",
        "@openai",
        packageName,
        "vendor",
        targetTriple,
        "codex",
        binaryName,
      );
      const siblingPath = joinPath(
        sep,
        nodeModulesRoot,
        "@openai",
        packageName,
        "vendor",
        targetTriple,
        "codex",
        binaryName,
      );
      if (fileExists) {
        if (await fileExists(nestedPath)) return nestedPath;
        if (await fileExists(siblingPath)) return siblingPath;
      }
      return siblingPath;
    }
  }

  const vendorRoot = joinPath(sep, wrapperDir, "..", "vendor");
  return joinPath(sep, vendorRoot, targetTriple, "codex", binaryName);
};

const findMatchingBrace = (text: string, start: number): number | null => {
  let depth = 0;
  let inString = false;
  let escape = false;
  for (let i = start; i < text.length; i++) {
    const ch = text[i];
    if (inString) {
      if (escape) {
        escape = false;
        continue;
      }
      if (ch === "\\") {
        escape = true;
        continue;
      }
      if (ch === '"') inString = false;
      continue;
    }
    if (ch === '"') {
      inString = true;
      continue;
    }
    if (ch === "{") {
      depth++;
      continue;
    }
    if (ch === "}") {
      depth--;
      if (depth === 0) return i;
    }
  }
  return null;
};

const extractJsonObjectAt = (text: string, index: number): string | null => {
  let start = text.lastIndexOf("{", index);
  while (start !== -1) {
    const end = findMatchingBrace(text, start);
    if (end !== null && end >= index) {
      return text.slice(start, end + 1);
    }
    start = text.lastIndexOf("{", start - 1);
  }
  return null;
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const getString = (value: unknown): string | null => (typeof value === "string" ? value : null);

export const extractCodexModelsFromText = (text: string): ExtractedCodexModels | null => {
  const versionMatch = text.match(/codex_cli_rs\/([0-9]+(?:\.[0-9]+){1,2})/);
  const clientVersion = versionMatch ? versionMatch[1] : null;

  const slugRegex = /"slug"\s*:\s*"([^"]+)"/g;
  const models: Record<string, unknown>[] = [];
  const seen = new Set<string>();
  let match: RegExpExecArray | null;
  while ((match = slugRegex.exec(text))) {
    const slug = match[1];
    if (!slug) continue;
    if (seen.has(slug)) continue;
    const objectText = extractJsonObjectAt(text, match.index);
    if (!objectText) continue;
    let parsed: unknown;
    try {
      parsed = JSON.parse(objectText);
    } catch {
      continue;
    }
    if (!isRecord(parsed)) continue;
    const normalizedSlug = getString(parsed.slug) ?? getString(parsed.id) ?? getString(parsed.model) ?? slug;
    if (!normalizedSlug || seen.has(normalizedSlug)) continue;
    const normalized: Record<string, unknown> = { slug: normalizedSlug };
    const displayName = getString(parsed.display_name) ?? getString(parsed.displayName) ?? getString(parsed.name);
    if (displayName) normalized.display_name = displayName;
    const description = getString(parsed.description);
    if (description) normalized.description = description;
    const defaultReasoning = getString(parsed.default_reasoning_level);
    if (defaultReasoning) normalized.default_reasoning_level = defaultReasoning;
    if (Array.isArray(parsed.supported_reasoning_levels)) {
      const levels = parsed.supported_reasoning_levels
        .map((entry) => {
          if (typeof entry === "string") return entry;
          if (isRecord(entry)) return getString(entry.effort);
          return null;
        })
        .filter((entry): entry is string => typeof entry === "string" && entry.length > 0);
      if (levels.length) normalized.supported_reasoning_levels = levels;
    }
    models.push(normalized);
    seen.add(normalizedSlug);
  }

  if (!models.length) return null;
  return { models, clientVersion };
};
