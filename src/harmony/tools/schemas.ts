/**
 * Canonical model-facing tool schemas (plan m04).
 *
 * Single source of truth for the compact Harmony agent tool surface:
 *
 * - filesystem.read / filesystem.find / filesystem.search
 * - browser.search / browser.open / browser.find
 * - shell.exec
 * - editor.apply_patch
 * - task.update_plan
 *
 * This module owns the stable model-facing JSON Schema of every tool,
 * argument validation with a uniform machine-readable failure mode, and the
 * rendering of the surface as m01 {@link ToolDefinition} entries. Every
 * definition is rendered with one uniform `strict` value (Cerebras rejects a
 * single request whose tools carry mixed strictness values), so the canonical
 * contract is a plain `strict: false`-style surface that any provider
 * translation (see `src/harmony/adapter.ts`) can re-render with a different
 * single strictness value without changing semantics.
 *
 * Schemas intentionally use minimally-required parameters (e.g.
 * `editor.apply_patch` accepts `path` plus optional `old`/`new`/`add`) to
 * keep the model-facing contract compact for a reasoning model with a bounded
 * context. Optional parameters are therefore NOT strict-mode JSON Schemas
 * (OpenAI strict mode requires every property to be required); `strict` may
 * still be applied uniformly for providers that accept it.
 *
 * This module performs no I/O and must not be imported by the gateway routes.
 * Execution lives in `router.ts`; backend dependencies are injected
 * (`backend.ts`), and deterministic fakes live in `fakes.ts`.
 */

import { type ToolDefinition } from "../types.ts";

/** Canonical tool names, in the order they are exposed to the model. */
export const CANONICAL_TOOL_NAMES = [
  "filesystem.read",
  "filesystem.find",
  "filesystem.search",
  "browser.search",
  "browser.open",
  "browser.find",
  "shell.exec",
  "editor.apply_patch",
  "task.update_plan",
] as const;

export type CanonicalToolName = (typeof CANONICAL_TOOL_NAMES)[number];

/** The single strictness value all canonical definitions expose by default. */
export const CANONICAL_TOOL_DEFAULT_STRICTNESS = false;

/** One canonical tool: model-facing name, description and JSON Schema. */
export interface CanonicalToolSchema {
  name: CanonicalToolName;
  description: string;
  /**
   * JSON Schema for the `parameters` object (root `type: "object"`).
   * `additionalProperties: false`; string/boolean/array-of-string types only.
   */
  parameters: Readonly<Record<string, unknown>>;
}

const property = (
  type: "string" | "boolean",
  extra: Readonly<Record<string, unknown>> = {},
): Readonly<Record<string, unknown>> => ({ type, ...extra });

const stringProperty = (extra: Readonly<Record<string, unknown>> = {}): Readonly<Record<string, unknown>> =>
  property("string", extra);

const nonEmptyStringProperty = (extra: Readonly<Record<string, unknown>> = {}): Readonly<Record<string, unknown>> =>
  property("string", { minLength: 1, ...extra });

const schema = (
  name: CanonicalToolName,
  description: string,
  properties: Readonly<Record<string, Readonly<Record<string, unknown>>>>,
  required: readonly string[],
): CanonicalToolSchema => ({
  name,
  description,
  parameters: {
    type: "object",
    properties,
    required: [...required],
    additionalProperties: false,
  },
});

/** Canonical tool registry, keyed by model-facing tool name. */
export const TOOL_SCHEMAS: Readonly<Record<CanonicalToolName, CanonicalToolSchema>> = {
  "filesystem.read": schema(
    "filesystem.read",
    "Read the UTF-8 text of a file inside the workspace. Returns the file content, or a not_found error when the path does not exist.",
    { path: nonEmptyStringProperty({ description: "Workspace-relative file path." }) },
    ["path"],
  ),
  "filesystem.find": schema(
    "filesystem.find",
    "List workspace-relative paths of files under a directory, optionally filtered by a glob pattern ('*' one segment, '**' across segments, '?' one character).",
    {
      path: nonEmptyStringProperty({ description: "Workspace-relative directory path to list." }),
      pattern: nonEmptyStringProperty({
        description: "Glob pattern; defaults to ** (all files under the directory).",
      }),
    },
    ["path"],
  ),
  "filesystem.search": schema(
    "filesystem.search",
    "Case-insensitive substring search inside the contents of files under a directory. Returns at most 200 matches as path:line:content.",
    {
      path: nonEmptyStringProperty({ description: "Workspace-relative directory path to search." }),
      query: nonEmptyStringProperty({ description: "Text to search for." }),
    },
    ["path", "query"],
  ),
  "browser.search": schema(
    "browser.search",
    "Search the web and return matching result titles, URLs and snippets. Returns (no results) when nothing matches.",
    { query: nonEmptyStringProperty({ description: "Search query." }) },
    ["query"],
  ),
  "browser.open": schema(
    "browser.open",
    "Open a URL and return the page title and text content. The opened page becomes the current page for browser.find.",
    { url: nonEmptyStringProperty({ description: "URL to open, e.g. https://example.com/page." }) },
    ["url"],
  ),
  "browser.find": schema(
    "browser.find",
    "Case-insensitive search for text in the currently-open browser page. Returns at most 200 matches as line:content.",
    { query: nonEmptyStringProperty({ description: "Text to find on the current page." }) },
    ["query"],
  ),
  "shell.exec": schema(
    "shell.exec",
    "Run a shell command in the workspace with sh -c and return its exit code, standard output and standard error. Non-zero exit codes are errors.",
    { command: nonEmptyStringProperty({ description: "Shell command to execute." }) },
    ["command"],
  ),
  "editor.apply_patch": schema(
    "editor.apply_patch",
    "Replace the unique occurrence of old with new in a file, or create a new file when add is true. old may be empty to insert at the start.",
    {
      path: nonEmptyStringProperty({ description: "Workspace-relative file path." }),
      old: stringProperty({
        description: "Exact text to replace; must occur exactly once (empty inserts at the start).",
      }),
      new: stringProperty({ description: "Replacement text, or the file content when add is true." }),
      add: property("boolean", { description: "Create the file instead of patching an existing one." }),
    },
    ["path"],
  ),
  "task.update_plan": schema(
    "task.update_plan",
    "Replace the current task plan with the given ordered list of step strings.",
    {
      plan: {
        type: "array",
        items: stringProperty({ minLength: 1 }),
        description: "Ordered plan steps; each entry must be a non-empty string.",
      },
    },
    ["plan"],
  ),
};

const NON_EMPTY_PARAMS: Readonly<Record<CanonicalToolName, readonly string[]>> = {
  "filesystem.read": ["path"],
  "filesystem.find": ["path", "pattern"],
  "filesystem.search": ["path", "query"],
  "browser.search": ["query"],
  "browser.open": ["url"],
  "browser.find": ["query"],
  "shell.exec": ["command"],
  "editor.apply_patch": ["path"],
  "task.update_plan": [],
};

/** Returns the canonical schema for a tool name, or null when unknown. */
export const lookupToolSchema = (tool: string): CanonicalToolSchema | null =>
  (TOOL_SCHEMAS as Readonly<Record<string, CanonicalToolSchema>>)[tool] ?? null;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const describeExpected = (type: "string" | "boolean" | "string[]"): string =>
  type === "string[]" ? "an array of non-empty strings" : `a ${type}`;

/**
 * Type-checks and normalizes tool arguments against the canonical schema.
 *
 * Every present argument is validated (not only required ones), unknown
 * arguments are rejected, and required string parameters must be non-empty.
 * The returned `arguments` object preserves the caller's values unchanged.
 */
export function validateToolArguments(
  tool: string,
  args: unknown,
): { valid: true; arguments: Record<string, unknown> } | { valid: false; reason: string } {
  const toolSchema = lookupToolSchema(tool);
  if (toolSchema === null) return { valid: false, reason: `unknown tool ${JSON.stringify(tool)}` };
  if (!isRecord(args)) return { valid: false, reason: "arguments must be an object" };

  const parameters = toolSchema.parameters.properties as Readonly<Record<string, Readonly<Record<string, unknown>>>>;
  const required = toolSchema.parameters.required as readonly string[];
  const nonEmpty = NON_EMPTY_PARAMS[toolSchema.name];

  for (const key of Object.keys(args)) {
    if (!(key in parameters)) return { valid: false, reason: `unexpected argument ${JSON.stringify(key)}` };
  }
  for (const key of required) {
    if (!(key in args) || args[key] === undefined) {
      return { valid: false, reason: `missing required argument ${JSON.stringify(key)}` };
    }
  }
  for (const [key, value] of Object.entries(args)) {
    if (value === undefined) return { valid: false, reason: `argument ${JSON.stringify(key)} must not be undefined` };
    const param = parameters[key];
    const type = typeof param.type === "string" ? param.type : "";
    if (type === "string") {
      if (typeof value !== "string") {
        return { valid: false, reason: `argument ${JSON.stringify(key)} must be a string` };
      }
      if (nonEmpty.includes(key) && (value as string).length === 0) {
        return { valid: false, reason: `argument ${JSON.stringify(key)} must be a non-empty string` };
      }
    } else if (type === "boolean") {
      if (typeof value !== "boolean") {
        return { valid: false, reason: `argument ${JSON.stringify(key)} must be a boolean` };
      }
    } else if (type === "array") {
      if (!Array.isArray(value) || value.some((item) => typeof item !== "string" || (item as string).length === 0)) {
        return { valid: false, reason: `argument ${JSON.stringify(key)} must be ${describeExpected("string[]")}` };
      }
    }
  }
  return { valid: true, arguments: args };
}

/** Parameter type view of a canonical schema, for tooling and compatibility shims. */
export const toolParameterTypes = (
  toolSchema: CanonicalToolSchema,
): Readonly<Record<string, "string" | "boolean" | "string[]">> => {
  const parameters = toolSchema.parameters.properties as Readonly<Record<string, Readonly<Record<string, unknown>>>>;
  const out: Record<string, "string" | "boolean" | "string[]"> = {};
  for (const [key, param] of Object.entries(parameters)) {
    if (param.type === "boolean") out[key] = "boolean";
    else if (param.type === "array") out[key] = "string[]";
    else out[key] = "string";
  }
  return out;
};

/** Renders the whole canonical surface as m01 ToolDefinition entries. */
export const toolDefinitions = (
  opts: Readonly<{ strict?: boolean }> = {},
): readonly ToolDefinition[] => {
  const strict = opts.strict ?? CANONICAL_TOOL_DEFAULT_STRICTNESS;
  return CANONICAL_TOOL_NAMES.map((name) => {
    const toolSchema = TOOL_SCHEMAS[name];
    return {
      name: toolSchema.name,
      description: toolSchema.description,
      parameters: toolSchema.parameters,
      strict,
    };
  });
};

const isStrictSchemaNode = (value: unknown): boolean => {
  if (typeof value !== "object" || value === null) return true;
  if (Array.isArray(value)) return value.every(isStrictSchemaNode);
  const record = value as Record<string, unknown>;
  for (const key of Object.keys(record)) {
    if (key === "default" || key === "anyOf" || key === "oneOf" || key === "not") return false;
    if (key === "type" && (Array.isArray(record.type) || record.type === "null" || record.type === "integer")) {
      return false;
    }
    if (!isStrictSchemaNode(record[key])) return false;
  }
  return true;
};

/**
 * Well-formedness proof for the canonical surface (used by focused tests).
 *
 * `strict: true` additionally requires every property to be listed in
 * `required` (the OpenAI strict-mode restriction). The canonical default is
 * `strict: false` because `editor.apply_patch` and `filesystem.find` keep
 * optional parameters by design.
 */
export const assertCanonicalToolSchemas = (
  strict: boolean,
): { ok: true; names: readonly CanonicalToolName[] } | { ok: false; name: string; reason: string } => {
  for (const toolSchema of Object.values(TOOL_SCHEMAS)) {
    const parameters = toolSchema.parameters;
    if (parameters.type !== "object") {
      return { ok: false, name: toolSchema.name, reason: "parameters root must be an object schema" };
    }
    if (parameters.additionalProperties !== false) {
      return { ok: false, name: toolSchema.name, reason: "additionalProperties must be false" };
    }
    const properties = parameters.properties;
    if (typeof properties !== "object" || properties === null || Array.isArray(properties)) {
      return { ok: false, name: toolSchema.name, reason: "properties must be an object" };
    }
    const required = Array.isArray(parameters.required) ? (parameters.required as unknown[]) : [];
    if (required.some((key) => typeof key !== "string" || !(key in (properties as Record<string, unknown>)))) {
      return { ok: false, name: toolSchema.name, reason: "required entries must name declared properties" };
    }
    const propertyNames = Object.keys(properties as Record<string, unknown>);
    if (strict && (required as string[]).length !== propertyNames.length) {
      return { ok: false, name: toolSchema.name, reason: "strict mode requires every property to be required" };
    }
    for (const key of propertyNames) {
      const param = (properties as Record<string, unknown>)[key];
      if (!isStrictSchemaNode(param)) {
        return { ok: false, name: toolSchema.name, reason: `property ${key} is not strict-mode compatible` };
      }
    }
    if (!isStrictSchemaNode(parameters)) {
      return { ok: false, name: toolSchema.name, reason: "parameters schema contains non-strict constructs" };
    }
  }
  return { ok: true, names: CANONICAL_TOOL_NAMES };
};
