export type ExtractedCodexModels = Readonly<{
  models: Record<string, unknown>[];
  clientVersion: string | null;
}>;

const looksLikeCodexWrapper = (text: string): boolean => {
  const trimmed = text.trimStart();
  if (!trimmed.startsWith("#!/usr/bin/env node")) return false;
  return text.includes("vendorRoot") && text.includes("targetTriple") && text.includes("codexBinaryName");
};

/** Platform-to-target-triple table; every combination not listed resolves to `null`. */
const TARGET_TRIPLES_BY_PLATFORM = new Map<string, string>([
  ["darwin:x86_64", "x86_64-apple-darwin"],
  ["darwin:aarch64", "aarch64-apple-darwin"],
  ["linux:x86_64", "x86_64-unknown-linux-musl"],
  ["linux:aarch64", "aarch64-unknown-linux-musl"],
  ["android:x86_64", "x86_64-unknown-linux-musl"],
  ["android:aarch64", "aarch64-unknown-linux-musl"],
  ["windows:x86_64", "x86_64-pc-windows-msvc"],
  ["windows:aarch64", "aarch64-pc-windows-msvc"],
]);

const detectTargetTriple = (os: string, arch: string): string | null => {
  const normalizedArch = arch === "arm64" ? "aarch64" : arch;
  return TARGET_TRIPLES_BY_PLATFORM.get(`${os}:${normalizedArch}`) ?? null;
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
  let prefix = "";
  if (drivePrefix) prefix = `${drivePrefix}${sep}`;
  else if (isAbsolute) prefix = sep;
  return `${prefix}${joined}`;
};

const dirname = (path: string, sep: string): string => {
  const normalized = normalizePath(path, sep);
  const index = normalized.lastIndexOf(sep);
  if (index <= 0) return normalized.slice(0, 1) || normalized;
  return normalized.slice(0, index);
};

const joinPath = (sep: string, ...parts: string[]): string => {
  // Blank and whitespace-only segments are dropped; trimming is only the test.
  const filtered = parts.filter((part) => part.trim());
  return normalizePath(filtered.join(sep), sep);
};

/** The binary inside the `@openai/<platform package>` layout, or `null` when the platform has no package. */
const resolvePlatformPackageBinary = async (
  wrapperDir: string,
  sep: string,
  targetTriple: string,
  binaryName: string,
  fileExists?: (path: string) => Promise<boolean>
): Promise<string | null> => {
  const packageName = platformPackageName(targetTriple);
  if (!packageName) return null;
  const packageRoot = joinPath(sep, wrapperDir, "..");
  const nodeModulesRoot = joinPath(sep, wrapperDir, "..", "..", "..");
  const nestedPath = joinPath(sep, packageRoot, "node_modules", "@openai", packageName, "vendor", targetTriple, "codex", binaryName);
  const siblingPath = joinPath(sep, nodeModulesRoot, "@openai", packageName, "vendor", targetTriple, "codex", binaryName);
  if (fileExists) {
    if (await fileExists(nestedPath)) return nestedPath;
    if (await fileExists(siblingPath)) return siblingPath;
  }
  return siblingPath;
};

const readTextFileOrNull = async (readTextFile: (path: string) => Promise<string>, path: string): Promise<string | null> => {
  try {
    return await readTextFile(path);
  } catch {
    return null;
  }
};

export const resolveCodexBinaryPath = async (
  codexPath: string,
  readTextFile: (path: string) => Promise<string>,
  os: string,
  arch: string,
  realPath?: (path: string) => Promise<string>,
  fileExists?: (path: string) => Promise<boolean>
): Promise<string> => {
  let resolvedPath = codexPath;
  if (realPath) {
    try {
      resolvedPath = await realPath(codexPath);
    } catch {
      resolvedPath = codexPath;
    }
  }

  const wrapperText = await readTextFileOrNull(readTextFile, resolvedPath);
  if (!wrapperText || !looksLikeCodexWrapper(wrapperText)) return codexPath;

  const targetTriple = detectTargetTriple(os, arch);
  if (!targetTriple) return codexPath;

  const sep = detectSeparator(resolvedPath, os);
  const wrapperDir = dirname(resolvedPath, sep);
  const binaryName = os === "windows" ? "codex.exe" : "codex";
  if (wrapperText.includes("PLATFORM_PACKAGE_BY_TARGET")) {
    const packageBinary = await resolvePlatformPackageBinary(wrapperDir, sep, targetTriple, binaryName, fileExists);
    if (packageBinary) return packageBinary;
  }

  const vendorRoot = joinPath(sep, wrapperDir, "..", "vendor");
  return joinPath(sep, vendorRoot, targetTriple, "codex", binaryName);
};

/** String-literal scan state, so braces inside quoted text are never counted. */
type CodexScanState = Readonly<{ inString: boolean; escape: boolean }>;

const advanceCodexScanState = (state: CodexScanState, ch: string): CodexScanState => {
  if (state.escape) return { inString: state.inString, escape: false };
  if (state.inString) {
    if (ch === "\\") return { inString: true, escape: true };
    if (ch === '"') return { inString: false, escape: false };
    return state;
  }
  if (ch === '"') return { inString: true, escape: false };
  return state;
};

const findMatchingBrace = (text: string, start: number): number | null => {
  let depth = 0;
  let state: CodexScanState = { inString: false, escape: false };
  for (let i = start; i < text.length; i++) {
    const ch = text[i];
    const wasInString = state.inString;
    state = advanceCodexScanState(state, ch);
    // Quotes and quoted text are skipped: only structural braces affect depth.
    if (wasInString || ch === '"') continue;
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

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value);

const getString = (value: unknown): string | null => (typeof value === "string" ? value : null);

const getNonNegativeInteger = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const normalized = Math.trunc(value);
  return normalized >= 0 ? normalized : null;
};

const isHiddenCodexModel = (value: Record<string, unknown>): boolean =>
  getString(value.visibility)?.trim().toLowerCase() === "hide" && value.supported_in_api !== true;

const getReasoningLevelString = (entry: unknown): string | null => {
  if (entry === null) return "none";
  if (typeof entry === "string") return entry;
  if (isRecord(entry)) return entry.effort === null ? "none" : getString(entry.effort);
  return null;
};

/** Parses the JSON object whose opening brace precedes `index`, or `null` when it does not parse. */
const parseCodexModelObjectAt = (text: string, index: number): Record<string, unknown> | null => {
  const objectText = extractJsonObjectAt(text, index);
  if (!objectText) return null;
  try {
    const parsed: unknown = JSON.parse(objectText);
    return isRecord(parsed) ? parsed : null;
  } catch {
    return null;
  }
};

const normalizeExtractedCodexModel = (parsed: Record<string, unknown>, slug: string): Record<string, unknown> => {
  const normalized: Record<string, unknown> = { slug };
  const displayName = getString(parsed.display_name) ?? getString(parsed.displayName) ?? getString(parsed.name);
  if (displayName) normalized.display_name = displayName;
  const description = getString(parsed.description);
  if (description) normalized.description = description;
  for (const key of ["context_window", "max_context_window", "auto_compact_token_limit"]) {
    if (parsed[key] === null) {
      normalized[key] = null;
      continue;
    }
    const count = getNonNegativeInteger(parsed[key]);
    if (count !== null) normalized[key] = count;
  }
  const defaultReasoning = parsed.default_reasoning_level === null ? "none" : getString(parsed.default_reasoning_level);
  if (defaultReasoning) normalized.default_reasoning_level = defaultReasoning;
  const rawLevels = parsed.supported_reasoning_levels;
  if (Array.isArray(rawLevels)) {
    const levels = rawLevels.map(getReasoningLevelString).filter((entry): entry is string => typeof entry === "string" && entry.length > 0);
    if (levels.length) normalized.supported_reasoning_levels = levels;
  }
  return normalized;
};

export const extractCodexModelsFromText = (text: string): ExtractedCodexModels | null => {
  const versionMatch = /codex_cli_rs\/(\d+(?:\.\d+){1,2})/.exec(text);
  const clientVersion = versionMatch ? versionMatch[1] : null;

  const slugRegex = /"slug"\s*:\s*"([^"]+)"/g;
  const models: Record<string, unknown>[] = [];
  const seen = new Set<string>();
  let match: RegExpExecArray | null;
  while ((match = slugRegex.exec(text))) {
    const slug = match[1];
    if (!slug || seen.has(slug)) continue;
    const parsed = parseCodexModelObjectAt(text, match.index);
    if (!parsed || isHiddenCodexModel(parsed)) continue;
    const normalizedSlug = getString(parsed.slug) ?? getString(parsed.id) ?? getString(parsed.model) ?? slug;
    if (!normalizedSlug || seen.has(normalizedSlug)) continue;
    models.push(normalizeExtractedCodexModel(parsed, normalizedSlug));
    seen.add(normalizedSlug);
  }

  if (!models.length) return null;
  return { models, clientVersion };
};
