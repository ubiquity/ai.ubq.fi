import {
  RESPONSES_TERMINAL_EVENT_TYPES,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  type ResponsesStreamIterator,
} from "./responses_stream.ts";
import { getString, isRecord } from "./utils.ts";

export const OPENROUTER_REASONING_PROJECTION_POLICY = Object.freeze({
  omitRawContent: true,
  omitEncryptedContent: true,
  preserveSummary: true,
});

export const FAILOVER_WARNING_TEXT = (actualModel: string): string =>
  `⚠ Failover active: your request was rerouted to \`openrouter:${actualModel}\` because the primary Codex service was unavailable.`;

const LEGACY_FAILOVER_WARNING_BODY =
  "⚠ Failover active: this response is from `openrouter:[^`]+` because the Codex upstream was unavailable\\.";
const CURRENT_FAILOVER_WARNING_BODY =
  "⚠ Failover active: your request was rerouted to `openrouter:[^`]+` because the primary Codex service was unavailable\\.";
const LEGACY_FAILOVER_WARNING_PATTERN = new RegExp(`^${LEGACY_FAILOVER_WARNING_BODY}$`);
const CURRENT_FAILOVER_WARNING_PATTERN = new RegExp(`^${CURRENT_FAILOVER_WARNING_BODY}$`);
const LEGACY_FAILOVER_WARNING_REPEAT_PATTERN = new RegExp(`^(?:${LEGACY_FAILOVER_WARNING_BODY})+$`);
const CURRENT_FAILOVER_WARNING_REPEAT_PATTERN = new RegExp(`^(?:${CURRENT_FAILOVER_WARNING_BODY})+$`);
const FAILOVER_WARNING_SHAPES = [
  {
    prefix: "⚠ Failover active: this response is from `openrouter:",
    suffix: "` because the Codex upstream was unavailable.",
  },
  {
    prefix: "⚠ Failover active: your request was rerouted to `openrouter:",
    suffix: "` because the primary Codex service was unavailable.",
  },
] as const;
const MAX_FAILOVER_WARNING_PREFIX_LENGTH = 1_024;

export const isFailoverWarningText = (value: unknown): value is string =>
  typeof value === "string" &&
  (
    CURRENT_FAILOVER_WARNING_PATTERN.test(value) || LEGACY_FAILOVER_WARNING_PATTERN.test(value) ||
    CURRENT_FAILOVER_WARNING_REPEAT_PATTERN.test(value) || LEGACY_FAILOVER_WARNING_REPEAT_PATTERN.test(value)
  );

const isFailoverWarningPrefix = (value: string): boolean => {
  if (value.length > MAX_FAILOVER_WARNING_PREFIX_LENGTH) return false;
  return FAILOVER_WARNING_SHAPES.some(({ prefix, suffix }) => {
    if (value.length <= prefix.length) return prefix.startsWith(value);
    if (!value.startsWith(prefix)) return false;
    const remainder = value.slice(prefix.length);
    const closingQuote = remainder.indexOf("`");
    if (closingQuote < 0) return true;
    if (closingQuote === 0) return false;
    return suffix.startsWith(remainder.slice(closingQuote + 1));
  });
};

const messageText = (value: Record<string, unknown>): string | null => {
  if (typeof value.content === "string") return value.content;
  if (!Array.isArray(value.content)) return null;
  const parts = value.content.filter(isRecord);
  if (!parts.length || parts.length !== value.content.length) return null;
  const texts = parts.map((part) => getString(part.text));
  return texts.every((text): text is string => text !== null) ? texts.join("") : null;
};

export const isGatewayFailoverWarningItem = (value: unknown): boolean => {
  if (!isRecord(value) || Array.isArray(value) || value.type !== "message" || value.role !== "assistant") return false;
  const text = messageText(value);
  return text !== null && isFailoverWarningText(text);
};

export const hasFailoverWarningAfterLatestUserMessage = (input: readonly unknown[]): boolean => {
  for (let index = input.length - 1; index >= 0; index -= 1) {
    const item = input[index];
    if (isGatewayFailoverWarningItem(item)) return true;
    if (isRecord(item) && !Array.isArray(item) && item.type === "message" && item.role === "user") return false;
  }
  return false;
};

export class OpenRouterProjectionError extends Error {
  readonly code = "openrouter_translation_invalid";
  readonly status = 400;
  readonly param: string | null;

  constructor(message: string, param?: string, options?: ErrorOptions) {
    super(message, options);
    this.name = "OpenRouterProjectionError";
    this.param = param ?? null;
  }
}

type JsonSchema = Record<string, unknown>;
type ProjectedToolKind = "function" | "custom" | "local_shell";

export type OpenRouterToolProjection = Readonly<{
  originalType: ProjectedToolKind;
  originalName: string;
  projectedName: string;
  parameters: JsonSchema;
  expectedCallType: "function_call" | "custom_tool_call" | "local_shell_call";
  expectedOutputType: "function_call_output" | "custom_tool_call_output";
  originalTool: Readonly<Record<string, unknown>>;
}>;

export type OpenRouterProjectionRegistry = Readonly<{
  entries: readonly OpenRouterToolProjection[];
  byProjectedName: ReadonlyMap<string, OpenRouterToolProjection>;
  byOriginalKey: ReadonlyMap<string, OpenRouterToolProjection>;
}>;

export type OpenRouterRequestProjection = Readonly<{
  input: string | readonly Record<string, unknown>[];
  tools: readonly Record<string, unknown>[] | undefined;
  toolChoice: unknown;
  registry: OpenRouterProjectionRegistry;
}>;

const objectSchema = (): JsonSchema => ({
  type: "object",
  properties: {},
  additionalProperties: false,
});

const customWrapperSchema = (): JsonSchema => ({
  type: "object",
  properties: { input: { type: "string" } },
  required: ["input"],
  additionalProperties: false,
});

const localShellWrapperSchema = (): JsonSchema => ({
  type: "object",
  properties: {
    command: { type: "array", items: { type: "string" } },
    workdir: { type: "string" },
    timeout_ms: { type: "integer" },
    env: { type: "object", additionalProperties: { type: "string" } },
    user: { type: "string" },
    with_escalated_permissions: { type: "boolean" },
    justification: { type: "string" },
  },
  required: ["command"],
  additionalProperties: false,
});

const stableHash = (value: string): string => {
  let hash = 0x811c9dc5;
  for (const character of value) {
    hash ^= character.codePointAt(0) ?? 0;
    hash = Math.imul(hash, 0x01000193);
  }
  return (hash >>> 0).toString(16).padStart(8, "0");
};

const validFunctionName = (value: string): boolean => /^[A-Za-z0-9_-]{1,64}$/.test(value);

const safeNamePart = (value: string): string => {
  const normalized = value.replace(/[^A-Za-z0-9_-]+/g, "_").replace(/^[_-]+|[_-]+$/g, "");
  return normalized || "tool";
};

const boundedName = (prefix: string, originalName: string, stableKey: string): string => {
  const suffix = stableHash(stableKey);
  const room = Math.max(1, 64 - prefix.length - suffix.length - 2);
  return `${prefix}${safeNamePart(originalName).slice(0, room)}_${suffix}`.slice(0, 64);
};

const copyJson = <T>(value: T): T => structuredClone(value);

const asObject = (value: unknown, param: string): JsonSchema => {
  if (!isRecord(value) || Array.isArray(value)) {
    throw new OpenRouterProjectionError(`${param} must be an object`, param);
  }
  return copyJson(value);
};

const fieldString = (value: Record<string, unknown>, field: string, param: string): string => {
  const result = getString(value[field])?.trim();
  if (!result) throw new OpenRouterProjectionError(`${param}.${field} must be a non-empty string`, `${param}.${field}`);
  return result;
};

type NormalizedTool = Readonly<{
  type: ProjectedToolKind;
  name: string;
  description?: string;
  parameters: JsonSchema;
  originalTool: Readonly<Record<string, unknown>>;
}>;

const normalizeTool = (value: unknown, index: number): NormalizedTool => {
  const param = `tools[${index}]`;
  if (!isRecord(value) || Array.isArray(value)) {
    throw new OpenRouterProjectionError(`${param} must be an object`, param);
  }
  const type = getString(value.type);
  if (type === "local_shell") {
    return {
      type: "local_shell",
      name: "local_shell",
      ...(typeof value.description === "string" ? { description: value.description } : {}),
      parameters: localShellWrapperSchema(),
      originalTool: copyJson(value),
    };
  }
  if (type !== "function" && type !== "custom") {
    throw new OpenRouterProjectionError(
      `${param}.type cannot be projected safely to an OpenRouter function`,
      `${param}.type`,
    );
  }
  const nested = isRecord(value.function) && !Array.isArray(value.function) ? value.function : null;
  const name = getString(value.name)?.trim() ?? getString(nested?.name)?.trim() ?? "";
  if (!name) throw new OpenRouterProjectionError(`${param}.name must be a non-empty string`, `${param}.name`);
  if (type === "custom") {
    if (value.format !== undefined && (!isRecord(value.format) || Array.isArray(value.format))) {
      throw new OpenRouterProjectionError(`${param}.format must be an object`, `${param}.format`);
    }
    return {
      type,
      name,
      ...(typeof value.description === "string" ? { description: value.description } : {}),
      parameters: customWrapperSchema(),
      originalTool: copyJson(value),
    };
  }
  const parameters = value.parameters ?? nested?.parameters;
  return {
    type,
    name,
    ...(typeof value.description === "string" ? { description: value.description } : {}),
    parameters: parameters === undefined ? objectSchema() : asObject(parameters, `${param}.parameters`),
    originalTool: copyJson(value),
  };
};

const toolsFromAdditionalInput = (
  value: unknown,
): readonly Record<string, unknown>[] => {
  if (!Array.isArray(value)) return [];
  const extracted: Record<string, unknown>[] = [];
  for (const [index, raw] of value.entries()) {
    if (!isRecord(raw) || raw.type !== "additional_tools") continue;
    const itemParam = `input[${index}]`;
    if (!Array.isArray(raw.tools)) {
      throw new OpenRouterProjectionError(`${itemParam}.tools must be an array`, `${itemParam}.tools`);
    }
    for (const [namespaceIndex, namespaceRaw] of raw.tools.entries()) {
      const namespaceParam = `${itemParam}.tools[${namespaceIndex}]`;
      if (!isRecord(namespaceRaw) || namespaceRaw.type !== "namespace") {
        throw new OpenRouterProjectionError(
          `${namespaceParam}.type must be namespace`,
          `${namespaceParam}.type`,
        );
      }
      if (!Array.isArray(namespaceRaw.tools)) {
        throw new OpenRouterProjectionError(`${namespaceParam}.tools must be an array`, `${namespaceParam}.tools`);
      }
      for (const [toolIndex, toolRaw] of namespaceRaw.tools.entries()) {
        if (!isRecord(toolRaw) || Array.isArray(toolRaw)) {
          throw new OpenRouterProjectionError(
            `${namespaceParam}.tools[${toolIndex}] must be an object`,
            `${namespaceParam}.tools[${toolIndex}]`,
          );
        }
        extracted.push(copyJson(toolRaw));
      }
    }
  }
  return extracted;
};

const originalKey = (type: ProjectedToolKind, name: string): string => `${type}:${name}`;

const projectedNameFor = (
  tool: NormalizedTool,
  used: ReadonlySet<string>,
): string => {
  const preferred = tool.type === "function" && validFunctionName(tool.name) && !used.has(tool.name)
    ? tool.name
    : boundedName(
      tool.type === "custom" ? "uos_custom_" : tool.type === "local_shell" ? "uos_local_shell_" : "uos_function_",
      tool.name,
      `${tool.type}\u0000${tool.name}`,
    );
  if (!used.has(preferred)) return preferred;
  for (let ordinal = 2; ordinal < 10_000; ordinal += 1) {
    const candidate = `${preferred.slice(0, 64 - String(ordinal).length - 1)}_${ordinal}`;
    if (!used.has(candidate)) return candidate;
  }
  throw new OpenRouterProjectionError("OpenRouter tool names could not be made collision-free.", "tools");
};

const projectedTool = (tool: NormalizedTool, projectedName: string): Record<string, unknown> => {
  const result: Record<string, unknown> = {
    type: "function",
    name: projectedName,
    parameters: copyJson(tool.parameters),
  };
  if (tool.description !== undefined) result.description = tool.description;
  return result;
};

export const buildOpenRouterProjectionRegistry = (value: unknown): {
  registry: OpenRouterProjectionRegistry;
  tools: readonly Record<string, unknown>[] | undefined;
} => {
  if (value === undefined) {
    return {
      registry: { entries: [], byProjectedName: new Map(), byOriginalKey: new Map() },
      tools: undefined,
    };
  }
  if (!Array.isArray(value)) throw new OpenRouterProjectionError("tools must be an array", "tools");
  const normalized = value.map(normalizeTool);
  const entries: OpenRouterToolProjection[] = [];
  const byProjectedName = new Map<string, OpenRouterToolProjection>();
  const byOriginalKey = new Map<string, OpenRouterToolProjection>();
  const projectedTools: Record<string, unknown>[] = [];
  for (const tool of normalized) {
    const key = originalKey(tool.type, tool.name);
    if (byOriginalKey.has(key)) {
      throw new OpenRouterProjectionError(`Duplicate tool definition: ${tool.name}`, "tools");
    }
    const projectedName = projectedNameFor(tool, new Set(byProjectedName.keys()));
    const entry: OpenRouterToolProjection = Object.freeze({
      originalType: tool.type,
      originalName: tool.name,
      projectedName,
      parameters: copyJson(tool.parameters),
      expectedCallType: tool.type === "custom"
        ? "custom_tool_call"
        : tool.type === "local_shell"
        ? "local_shell_call"
        : "function_call",
      expectedOutputType: tool.type === "custom" ? "custom_tool_call_output" : "function_call_output",
      originalTool: tool.originalTool,
    });
    entries.push(entry);
    byProjectedName.set(projectedName, entry);
    byOriginalKey.set(key, entry);
    projectedTools.push(projectedTool(tool, projectedName));
  }
  return {
    registry: { entries: Object.freeze(entries), byProjectedName, byOriginalKey },
    tools: projectedTools,
  };
};

const schemaTypeMatches = (value: unknown, type: string): boolean => {
  if (type === "object") return isRecord(value) && !Array.isArray(value);
  if (type === "array") return Array.isArray(value);
  if (type === "string") return typeof value === "string";
  if (type === "integer") return typeof value === "number" && Number.isSafeInteger(value);
  if (type === "number") return typeof value === "number" && Number.isFinite(value);
  if (type === "boolean") return typeof value === "boolean";
  if (type === "null") return value === null;
  return true;
};

const validateSchemaValue = (value: unknown, schema: unknown, param: string): void => {
  if (!isRecord(schema) || Array.isArray(schema)) return;
  if (
    Array.isArray(schema.enum) && !schema.enum.some((candidate) => JSON.stringify(candidate) === JSON.stringify(value))
  ) {
    throw new OpenRouterProjectionError(`${param} is not one of the allowed values`, param);
  }
  const type = getString(schema.type);
  if (type && !schemaTypeMatches(value, type)) {
    throw new OpenRouterProjectionError(`${param} must be a ${type}`, param);
  }
  if (type === "object" && isRecord(value) && !Array.isArray(value)) {
    const properties = isRecord(schema.properties) && !Array.isArray(schema.properties) ? schema.properties : {};
    if (Array.isArray(schema.required)) {
      for (const required of schema.required) {
        if (typeof required === "string" && !Object.prototype.hasOwnProperty.call(value, required)) {
          throw new OpenRouterProjectionError(`${param}.${required} is required`, `${param}.${required}`);
        }
      }
    }
    if (schema.additionalProperties === false) {
      for (const key of Object.keys(value)) {
        if (!Object.prototype.hasOwnProperty.call(properties, key)) {
          throw new OpenRouterProjectionError(`${param}.${key} is not allowed`, `${param}.${key}`);
        }
      }
    }
    if (isRecord(schema.additionalProperties) && !Array.isArray(schema.additionalProperties)) {
      for (const key of Object.keys(value)) {
        if (!Object.prototype.hasOwnProperty.call(properties, key)) {
          validateSchemaValue(value[key], schema.additionalProperties, `${param}.${key}`);
        }
      }
    }
    for (const [key, propertySchema] of Object.entries(properties)) {
      if (Object.prototype.hasOwnProperty.call(value, key)) {
        validateSchemaValue(value[key], propertySchema, `${param}.${key}`);
      }
    }
  }
  if (type === "array" && Array.isArray(value) && schema.items !== undefined) {
    value.forEach((item, index) => validateSchemaValue(item, schema.items, `${param}[${index}]`));
  }
};

const parseArguments = (argumentsText: unknown, entry: OpenRouterToolProjection, param: string): unknown => {
  if (typeof argumentsText !== "string") {
    throw new OpenRouterProjectionError(`${param}.arguments must be a JSON string`, `${param}.arguments`);
  }
  let value: unknown;
  try {
    value = JSON.parse(argumentsText);
  } catch (cause) {
    throw new OpenRouterProjectionError(`${param}.arguments is not valid JSON`, `${param}.arguments`, { cause });
  }
  validateSchemaValue(value, entry.parameters, `${param}.arguments`);
  return value;
};

const callId = (value: Record<string, unknown>, param: string): string => {
  const id = getString(value.call_id)?.trim();
  if (!id) throw new OpenRouterProjectionError(`${param}.call_id must be a non-empty string`, `${param}.call_id`);
  return id;
};

const outputValue = (value: Record<string, unknown>, param: string): unknown => {
  const output = value.output;
  if (typeof output === "string") return output;
  if (!Array.isArray(output)) {
    throw new OpenRouterProjectionError(`${param}.output must be a string or an array`, `${param}.output`);
  }
  for (const [index, item] of output.entries()) {
    if (!isRecord(item) || Array.isArray(item) || typeof item.type !== "string") {
      throw new OpenRouterProjectionError(`${param}.output[${index}] is invalid`, `${param}.output[${index}]`);
    }
    if (
      item.type !== "input_text" && item.type !== "output_text" && item.type !== "input_image" &&
      item.type !== "input_file"
    ) {
      throw new OpenRouterProjectionError(
        `${param}.output[${index}].type is not supported`,
        `${param}.output[${index}].type`,
      );
    }
    if ((item.type === "input_text" || item.type === "output_text") && typeof item.text !== "string") {
      throw new OpenRouterProjectionError(
        `${param}.output[${index}].text must be a string`,
        `${param}.output[${index}].text`,
      );
    }
  }
  return copyJson(output);
};

const reasoningSummary = (value: unknown, param: string): unknown => {
  if (!Array.isArray(value)) throw new OpenRouterProjectionError(`${param} must be an array`, param);
  for (const [index, item] of value.entries()) {
    if (!isRecord(item) || Array.isArray(item) || typeof item.text !== "string") {
      throw new OpenRouterProjectionError(`${param}[${index}] must contain text`, `${param}[${index}]`);
    }
  }
  return copyJson(value);
};

const projectReasoning = (item: Record<string, unknown>, param: string): Record<string, unknown> => {
  if (item.type !== "reasoning") {
    throw new OpenRouterProjectionError(`${param}.type must be reasoning`, `${param}.type`);
  }
  const projected: Record<string, unknown> = { type: "reasoning" };
  if (item.id !== undefined && typeof item.id === "string") projected.id = item.id;
  if (item.summary !== undefined) projected.summary = reasoningSummary(item.summary, `${param}.summary`);
  if (item.content !== undefined && item.content !== null && !Array.isArray(item.content)) {
    throw new OpenRouterProjectionError(`${param}.content must be an array`, `${param}.content`);
  }
  if (
    item.encrypted_content !== undefined && item.encrypted_content !== null &&
    typeof item.encrypted_content !== "string"
  ) {
    throw new OpenRouterProjectionError(
      `${param}.encrypted_content must be a string or null`,
      `${param}.encrypted_content`,
    );
  }
  return projected;
};

const projectMessage = (item: Record<string, unknown>, param: string): Record<string, unknown> => {
  const role = getString(item.role);
  if (role !== "user" && role !== "assistant" && role !== "developer") {
    throw new OpenRouterProjectionError(`${param}.role is invalid`, `${param}.role`);
  }
  const content = item.content;
  if (typeof content === "string") {
    return {
      type: "message",
      role,
      content: [{ type: role === "assistant" ? "output_text" : "input_text", text: content }],
    };
  }
  if (!Array.isArray(content)) {
    throw new OpenRouterProjectionError(`${param}.content must be an array`, `${param}.content`);
  }
  for (const [index, part] of content.entries()) {
    if (!isRecord(part) || Array.isArray(part) || typeof part.type !== "string") {
      throw new OpenRouterProjectionError(`${param}.content[${index}] is invalid`, `${param}.content[${index}]`);
    }
    if (["input_text", "output_text"].includes(part.type) && typeof part.text !== "string") {
      throw new OpenRouterProjectionError(
        `${param}.content[${index}].text must be a string`,
        `${param}.content[${index}].text`,
      );
    }
    if (
      ![
        "input_text",
        "output_text",
        "input_image",
        "input_file",
      ].includes(part.type)
    ) {
      throw new OpenRouterProjectionError(
        `${param}.content[${index}].type is not supported`,
        `${param}.content[${index}].type`,
      );
    }
  }
  return { type: "message", role, content: copyJson(content) };
};

const findOriginal = (
  registry: OpenRouterProjectionRegistry,
  type: ProjectedToolKind,
  name: string,
  param: string,
): OpenRouterToolProjection => {
  const entry = registry.byOriginalKey.get(originalKey(type, name));
  if (!entry) throw new OpenRouterProjectionError(`No advertised ${type} tool matches ${name}`, `${param}.name`);
  return entry;
};

const projectLocalShellArguments = (item: Record<string, unknown>, param: string): string => {
  const action = isRecord(item.action) && !Array.isArray(item.action) ? item.action : null;
  if (!action || action.type !== "exec") {
    throw new OpenRouterProjectionError(`${param}.action must be an exec action`, `${param}.action`);
  }
  if (
    !Array.isArray(action.command) || !action.command.length ||
    action.command.some((part) => typeof part !== "string")
  ) {
    throw new OpenRouterProjectionError(
      `${param}.action.command must be an array of strings`,
      `${param}.action.command`,
    );
  }
  const args: Record<string, unknown> = { command: copyJson(action.command) };
  if (typeof action.working_directory === "string") args.workdir = action.working_directory;
  if (action.timeout_ms !== undefined) args.timeout_ms = action.timeout_ms;
  if (action.env !== undefined) args.env = copyJson(action.env);
  if (action.user !== undefined) args.user = action.user;
  if (action.with_escalated_permissions !== undefined) {
    args.with_escalated_permissions = action.with_escalated_permissions;
  }
  if (action.justification !== undefined) args.justification = action.justification;
  return JSON.stringify(args);
};

const projectCall = (
  item: Record<string, unknown>,
  registry: OpenRouterProjectionRegistry,
  param: string,
): { value: Record<string, unknown>; entry: OpenRouterToolProjection | null } => {
  const type = getString(item.type);
  if (type === "function_call") {
    const name = fieldString(item, "name", param);
    const id = callId(item, param);
    if (typeof item.arguments !== "string") {
      throw new OpenRouterProjectionError(`${param}.arguments must be a JSON string`, `${param}.arguments`);
    }
    const entry = registry.byOriginalKey.get(originalKey("function", name)) ?? null;
    if (!entry) {
      if (registry.entries.some((candidate) => candidate.originalName === name)) {
        throw new OpenRouterProjectionError(
          `${param}.name does not match the advertised function tool kind`,
          `${param}.name`,
        );
      }
      return { value: copyJson(item), entry: null };
    }
    return {
      value: { type: "function_call", name: entry.projectedName, arguments: item.arguments, call_id: id },
      entry,
    };
  }
  if (type === "custom_tool_call") {
    const name = fieldString(item, "name", param);
    const entry = findOriginal(registry, "custom", name, param);
    const id = callId(item, param);
    if (typeof item.input !== "string") {
      throw new OpenRouterProjectionError(`${param}.input must be a string`, `${param}.input`);
    }
    return {
      value: {
        type: "function_call",
        name: entry.projectedName,
        arguments: JSON.stringify({ input: item.input }),
        call_id: id,
      },
      entry,
    };
  }
  if (type === "local_shell_call") {
    const entry = findOriginal(registry, "local_shell", "local_shell", param);
    const id = getString(item.call_id)?.trim() ?? getString(item.id)?.trim();
    if (!id) throw new OpenRouterProjectionError(`${param} must include a call_id or id`, `${param}.call_id`);
    const argumentsText = projectLocalShellArguments(item, param);
    parseArguments(argumentsText, entry, param);
    return {
      value: {
        type: "function_call",
        name: entry.projectedName,
        arguments: argumentsText,
        call_id: id,
      },
      entry,
    };
  }
  throw new OpenRouterProjectionError(`${param}.type is not a supported typed Responses item`, `${param}.type`);
};

const projectOutput = (
  item: Record<string, unknown>,
  calls: ReadonlyMap<string, OpenRouterToolProjection>,
  usedOutputs: Set<string>,
  param: string,
): Record<string, unknown> => {
  const type = getString(item.type);
  const id = callId(item, param);
  const entry = calls.get(id);
  if (!entry) {
    if (type !== "function_call_output" && type !== "custom_tool_call_output") {
      throw new OpenRouterProjectionError(`${param}.type is not a supported tool output`, `${param}.type`);
    }
    if (usedOutputs.has(id)) {
      throw new OpenRouterProjectionError(`${param} duplicates call_id ${id}`, `${param}.call_id`);
    }
    const output = outputValue(item, param);
    usedOutputs.add(id);
    return type === "custom_tool_call_output" ? { type: "function_call_output", call_id: id, output } : copyJson(item);
  }
  if (usedOutputs.has(id)) throw new OpenRouterProjectionError(`${param} duplicates call_id ${id}`, `${param}.call_id`);
  const isCustom = type === "custom_tool_call_output";
  if (type !== "function_call_output" && !isCustom) {
    throw new OpenRouterProjectionError(`${param}.type is not a supported tool output`, `${param}.type`);
  }
  if (isCustom && entry.originalType !== "custom") {
    throw new OpenRouterProjectionError(`${param} does not match the advertised tool kind`, `${param}.type`);
  }
  if (!isCustom && entry.originalType === "custom") {
    throw new OpenRouterProjectionError(`${param} must use custom_tool_call_output`, `${param}.type`);
  }
  usedOutputs.add(id);
  return { type: "function_call_output", call_id: id, output: outputValue(item, param) };
};

const projectInput = (
  value: unknown,
  registry: OpenRouterProjectionRegistry,
): string | readonly Record<string, unknown>[] => {
  if (typeof value === "string") return value;
  if (!Array.isArray(value)) throw new OpenRouterProjectionError("input must be a string or an array", "input");
  const output: Record<string, unknown>[] = [];
  const calls = new Map<string, OpenRouterToolProjection>();
  const seenCalls = new Set<string>();
  const usedOutputs = new Set<string>();
  for (const [index, raw] of value.entries()) {
    const param = `input[${index}]`;
    if (!isRecord(raw) || Array.isArray(raw)) throw new OpenRouterProjectionError(`${param} must be an object`, param);
    if (isGatewayFailoverWarningItem(raw)) continue;
    const type = getString(raw.type);
    if (type === "message") {
      output.push(projectMessage(raw, param));
      continue;
    }
    if (type === "reasoning") {
      output.push(projectReasoning(raw, param));
      continue;
    }
    if (type === "additional_tools") {
      continue;
    }
    if (type === "function_call" || type === "custom_tool_call" || type === "local_shell_call") {
      const projected = projectCall(raw, registry, param);
      const projectedCallId = projected.value.call_id as string;
      if (seenCalls.has(projectedCallId)) {
        throw new OpenRouterProjectionError(
          `${param} duplicates call_id ${String(projectedCallId)}`,
          `${param}.call_id`,
        );
      }
      seenCalls.add(projectedCallId);
      if (projected.entry) calls.set(projectedCallId, projected.entry);
      output.push(projected.value);
      continue;
    }
    if (type === "function_call_output" || type === "custom_tool_call_output") {
      output.push(projectOutput(raw, calls, usedOutputs, param));
      continue;
    }
    throw new OpenRouterProjectionError(`${param}.type is not supported on the OpenRouter route`, `${param}.type`);
  }
  return output;
};

const projectToolChoice = (value: unknown, registry: OpenRouterProjectionRegistry): unknown => {
  if (value === undefined || typeof value === "string") return value;
  if (!isRecord(value) || Array.isArray(value)) {
    throw new OpenRouterProjectionError("tool_choice must be a string or object", "tool_choice");
  }
  const type = getString(value.type);
  if (type !== "function" && type !== "custom") return copyJson(value);
  const name = getString(value.name)?.trim();
  if (!name) throw new OpenRouterProjectionError("tool_choice.name must be a non-empty string", "tool_choice.name");
  const matches = registry.entries.filter((candidate) =>
    candidate.originalName === name && candidate.originalType === type
  );
  if (!matches.length) {
    throw new OpenRouterProjectionError(`tool_choice references unknown tool ${name}`, "tool_choice.name");
  }
  if (matches.length > 1) {
    throw new OpenRouterProjectionError(`tool_choice references an ambiguous tool ${name}`, "tool_choice.name");
  }
  const entry = matches[0]!;
  return { type: "function", name: entry.projectedName };
};

export const buildOpenRouterRequestProjection = (canonical: Record<string, unknown>): OpenRouterRequestProjection => {
  const additionalTools = toolsFromAdditionalInput(canonical.input);
  const effectiveTools = Array.isArray(canonical.tools)
    ? [...canonical.tools, ...additionalTools]
    : additionalTools.length
    ? additionalTools
    : canonical.tools;
  const { registry, tools } = buildOpenRouterProjectionRegistry(effectiveTools);
  return {
    input: projectInput(canonical.input, registry),
    tools,
    toolChoice: projectToolChoice(canonical.tool_choice, registry),
    registry,
  };
};

type CallState = {
  entry: OpenRouterToolProjection | null;
  callId: string | null;
  itemId: string | null;
  outputIndex: number | null;
  projectedName: string | null;
  deltaArguments: string;
  completeArguments: string | null;
  nativeDoneEmitted: boolean;
  nativeItem: Record<string, unknown> | null;
};

const eventFromValue = (value: Record<string, unknown>): ResponsesStreamEvent => {
  const type = getString(value.type)?.trim();
  if (!type) throw new ResponsesStreamError("Responses event rewrite omitted type.", { kind: "malformed_event" });
  return {
    raw: `event: ${type}\ndata: ${JSON.stringify(value)}\n\n`,
    value,
    type,
    terminal: RESPONSES_TERMINAL_EVENT_TYPES.has(type),
  };
};

const outputIndex = (value: Record<string, unknown>): number | null =>
  typeof value.output_index === "number" && Number.isSafeInteger(value.output_index) ? value.output_index : null;

const failProjection = (message: string): never => {
  throw new ResponsesStreamError(message, { kind: "malformed_event" });
};

const providerEntryForItem = (
  item: Record<string, unknown>,
  registry: OpenRouterProjectionRegistry,
): OpenRouterToolProjection | null => {
  const type = getString(item.type);
  const name = getString(item.name)?.trim();
  if (type === "local_shell_call") return registry.byOriginalKey.get(originalKey("local_shell", "local_shell")) ?? null;
  if (!type || !name) return null;
  if (type === "function_call") return registry.byProjectedName.get(name) ?? null;
  if (type === "custom_tool_call") return registry.byOriginalKey.get(originalKey("custom", name)) ?? null;
  return null;
};

const callStateKey = (value: Record<string, unknown>): string | null => {
  const callId = getString(value.call_id)?.trim();
  if (callId) return `call:${callId}`;
  const itemId = getString(value.item_id)?.trim() ??
    (isRecord(value.item) && !Array.isArray(value.item) ? getString(value.item.id)?.trim() : null);
  if (itemId) return `item:${itemId}`;
  const index = outputIndex(value);
  return index === null ? null : `index:${index}`;
};

const nativeCallId = (item: Record<string, unknown>, state: CallState): string => {
  const id = getString(item.call_id)?.trim() ?? state.callId;
  if (!id) return failProjection("OpenRouter tool call omitted a stable call_id.");
  return id;
};

const setCallIdentity = (state: CallState, item: Record<string, unknown>): void => {
  const itemId = getString(item.id)?.trim();
  const callId = getString(item.call_id)?.trim();
  if (itemId && state.itemId && itemId !== state.itemId) failProjection("OpenRouter tool call changed item identity.");
  if (callId && state.callId && callId !== state.callId) failProjection("OpenRouter tool call changed call identity.");
  if (itemId) state.itemId = itemId;
  if (callId) state.callId = callId;
  const index = outputIndex(item);
  if (index !== null && state.outputIndex !== null && index !== state.outputIndex) {
    failProjection("OpenRouter tool call changed output identity.");
  }
  if (index !== null) state.outputIndex = index;
};

const rememberArguments = (state: CallState, value: unknown, complete: boolean): void => {
  if (typeof value !== "string") return failProjection("OpenRouter tool call arguments were not a string.");
  const argumentsText = value;
  if (complete) {
    if (state.deltaArguments && state.deltaArguments !== argumentsText) {
      failProjection("OpenRouter tool call arguments changed after deltas.");
    }
    if (state.completeArguments !== null && state.completeArguments !== argumentsText) {
      failProjection("OpenRouter tool call arguments changed after finalization.");
    }
    state.completeArguments = argumentsText;
    return;
  }
  if (state.completeArguments !== null) failProjection("OpenRouter tool call received arguments after finalization.");
  state.deltaArguments += argumentsText;
};

const decodeLocalShell = (value: unknown): Record<string, unknown> => {
  if (!isRecord(value) || Array.isArray(value) || !Array.isArray(value.command) || !value.command.length) {
    return failProjection("OpenRouter local-shell arguments must contain command.");
  }
  const objectValue = value;
  const command = objectValue.command;
  if (!Array.isArray(command)) return failProjection("OpenRouter local-shell command is invalid.");
  if (command.some((part) => typeof part !== "string")) {
    return failProjection("OpenRouter local-shell command is invalid.");
  }
  const action: Record<string, unknown> = { type: "exec", command: copyJson(command) };
  if (typeof objectValue.workdir === "string") action.working_directory = objectValue.workdir;
  if (objectValue.timeout_ms !== undefined) action.timeout_ms = objectValue.timeout_ms;
  if (objectValue.env !== undefined) action.env = copyJson(objectValue.env);
  if (objectValue.user !== undefined) action.user = objectValue.user;
  if (objectValue.with_escalated_permissions !== undefined) {
    action.with_escalated_permissions = objectValue.with_escalated_permissions;
  }
  if (objectValue.justification !== undefined) action.justification = objectValue.justification;
  return action;
};

const reverseArguments = (state: CallState, argumentsText: string): Record<string, unknown> => {
  if (!state.entry) return failProjection("OpenRouter tool call name was not advertised.");
  const entry = state.entry;
  const id = state.callId;
  if (!id) return failProjection("OpenRouter tool call omitted call_id.");
  if (entry.originalType === "function") {
    return {
      type: "function_call",
      ...(state.itemId ? { id: state.itemId } : {}),
      status: "completed",
      name: entry.originalName,
      arguments: argumentsText,
      call_id: id,
    };
  }
  const decoded = parseArguments(argumentsText, entry, "response.output_item");
  if (entry.originalType === "custom") {
    if (!isRecord(decoded) || typeof decoded.input !== "string") {
      return failProjection("OpenRouter custom-tool wrapper input is invalid.");
    }
    const input = decoded.input;
    return {
      type: "custom_tool_call",
      ...(state.itemId ? { id: state.itemId } : {}),
      status: "completed",
      call_id: id,
      name: entry.originalName,
      input,
    };
  }
  return {
    type: "local_shell_call",
    ...(state.itemId ? { id: state.itemId } : {}),
    call_id: id,
    status: "completed",
    action: decodeLocalShell(decoded),
  };
};

const reverseDirectNativeItem = (
  item: Record<string, unknown>,
  entry: OpenRouterToolProjection,
): Record<string, unknown> => {
  const id = getString(item.call_id)?.trim();
  if (!id) return failProjection("OpenRouter native tool call omitted call_id.");
  if (entry.originalType === "custom") {
    if (item.type !== "custom_tool_call" || item.name !== entry.originalName || typeof item.input !== "string") {
      failProjection("OpenRouter native custom-tool call does not match the advertised tool.");
    }
    return copyJson(item);
  }
  if (entry.originalType === "function") {
    if (item.type !== "function_call" || item.name !== entry.originalName) {
      failProjection("OpenRouter native function call does not match the advertised tool.");
    }
    return copyJson(item);
  }
  return failProjection("OpenRouter emitted an unsupported native local-shell call.");
};

const itemFromState = (state: CallState, item?: Record<string, unknown>): Record<string, unknown> => {
  if (state.entry && getString(item?.type) === "custom_tool_call" && state.entry.originalType === "custom") {
    return reverseDirectNativeItem(item!, state.entry);
  }
  if (state.entry && getString(item?.type) === "local_shell_call" && state.entry.originalType === "local_shell") {
    return reverseDirectNativeItem(item!, state.entry);
  }
  const argumentsText = state.completeArguments ?? (state.deltaArguments || null);
  if (!argumentsText) return failProjection("OpenRouter tool call ended without complete arguments.");
  const itemValue = reverseArguments(state, argumentsText);
  if (item && typeof item.status === "string") {
    itemValue.status = item.status === "in_progress" ? "in_progress" : "completed";
  }
  return itemValue;
};

const removeWarningEchoFromOutput = (output: unknown): unknown[] | null => {
  if (!Array.isArray(output)) return null;
  return output.filter((item) =>
    !(isRecord(item) && !Array.isArray(item) && item.type === "message" && item.role === "assistant" &&
      isFailoverWarningText(messageText(item)))
  );
};

type WarningEchoCandidate = {
  aliases: Set<string>;
  buffered: ResponsesStreamEvent[];
  text: string;
};

type WarningEchoFilterState = {
  aliases: Map<string, WarningEchoCandidate>;
  pending: Set<WarningEchoCandidate>;
  suppressed: Set<string>;
};

const warningEchoKeys = (
  value: Record<string, unknown>,
  item?: Record<string, unknown>,
): string[] => {
  const keys: string[] = [];
  const itemId = getString(item?.id)?.trim() || getString(value.item_id)?.trim();
  if (itemId) keys.push(`item:${itemId}`);
  const index = outputIndex(value);
  if (index !== null) keys.push(`index:${index}`);
  return keys;
};

const warningCandidateFor = (
  state: WarningEchoFilterState,
  keys: readonly string[],
): WarningEchoCandidate | null => {
  for (const key of keys) {
    const candidate = state.aliases.get(key);
    if (candidate) return candidate;
  }
  return null;
};

const bindWarningEchoAliases = (
  state: WarningEchoFilterState,
  candidate: WarningEchoCandidate,
  keys: readonly string[],
): void => {
  for (const key of keys) {
    candidate.aliases.add(key);
    state.aliases.set(key, candidate);
  }
};

const createWarningEchoCandidate = (
  state: WarningEchoFilterState,
  keys: readonly string[],
): WarningEchoCandidate => {
  const candidate: WarningEchoCandidate = { aliases: new Set(), buffered: [], text: "" };
  state.pending.add(candidate);
  bindWarningEchoAliases(state, candidate, keys);
  return candidate;
};

const discardWarningEchoCandidate = (
  state: WarningEchoFilterState,
  candidate: WarningEchoCandidate,
): void => {
  state.pending.delete(candidate);
  for (const key of candidate.aliases) {
    state.suppressed.add(key);
    if (state.aliases.get(key) === candidate) state.aliases.delete(key);
  }
};

const flushWarningEchoCandidate = (
  state: WarningEchoFilterState,
  candidate: WarningEchoCandidate,
): ResponsesStreamEvent[] => {
  state.pending.delete(candidate);
  for (const key of candidate.aliases) {
    if (state.aliases.get(key) === candidate) state.aliases.delete(key);
  }
  return candidate.buffered;
};

const warningTextUpdate = (
  event: ResponsesStreamEvent,
): Readonly<{ text: string; replace: boolean }> | null => {
  if (event.type === "response.output_text.delta" && typeof event.value.delta === "string") {
    return { text: event.value.delta, replace: false };
  }
  if (event.type === "response.output_text.done" && typeof event.value.text === "string") {
    return { text: event.value.text, replace: true };
  }
  if (event.type === "response.content_part.done" && isRecord(event.value.part)) {
    const text = getString(event.value.part.text);
    if (text !== null) return { text, replace: true };
  }
  return null;
};

const applyWarningTextUpdate = (
  candidate: WarningEchoCandidate,
  update: Readonly<{ text: string; replace: boolean }> | null,
): void => {
  if (!update) return;
  candidate.text = update.replace ? update.text : candidate.text + update.text;
};

const isAssistantMessageItem = (value: unknown): value is Record<string, unknown> =>
  isRecord(value) && !Array.isArray(value) && value.type === "message" && value.role === "assistant";

const filterWarningEchoes = (iterator: ResponsesStreamIterator): ResponsesStreamIterator =>
  (async function* () {
    const state: WarningEchoFilterState = { aliases: new Map(), pending: new Set(), suppressed: new Set() };
    const suppressKeys = (keys: readonly string[]): void => {
      for (const key of keys) state.suppressed.add(key);
    };

    const resolveOutput = (output: unknown): ResponsesStreamEvent[] => {
      if (!Array.isArray(output)) return [];
      const flushed: ResponsesStreamEvent[] = [];
      for (const [index, raw] of output.entries()) {
        if (!isRecord(raw) || Array.isArray(raw)) continue;
        const item = raw;
        const keys = warningEchoKeys({ output_index: index }, item);
        const candidate = warningCandidateFor(state, keys);
        if (isGatewayFailoverWarningItem(item)) {
          if (candidate) discardWarningEchoCandidate(state, candidate);
          else suppressKeys(keys);
          continue;
        }
        if (!candidate) continue;
        bindWarningEchoAliases(state, candidate, keys);
        const text = messageText(item);
        if (text !== null) candidate.text = text;
        if (isFailoverWarningText(candidate.text)) discardWarningEchoCandidate(state, candidate);
        else flushed.push(...flushWarningEchoCandidate(state, candidate));
      }
      return flushed;
    };

    const resolvePendingAtTerminal = (): ResponsesStreamEvent[] => {
      const flushed: ResponsesStreamEvent[] = [];
      for (const candidate of [...state.pending]) {
        if (isFailoverWarningText(candidate.text)) discardWarningEchoCandidate(state, candidate);
        else flushed.push(...flushWarningEchoCandidate(state, candidate));
      }
      return flushed;
    };

    for await (const sourceEvent of iterator) {
      const value = sourceEvent.value;
      const item = isRecord(value.item) && !Array.isArray(value.item) ? value.item : undefined;
      const keys = warningEchoKeys(value, item);
      if (keys.some((key) => state.suppressed.has(key))) continue;

      if (sourceEvent.type === "response.output_item.added" && isAssistantMessageItem(item)) {
        if (isGatewayFailoverWarningItem(item)) {
          suppressKeys(keys);
          continue;
        }
        const candidate = warningCandidateFor(state, keys) ?? createWarningEchoCandidate(state, keys);
        candidate.buffered.push(sourceEvent);
        const text = messageText(item);
        if (text !== null) candidate.text = text;
        if (isFailoverWarningText(candidate.text)) discardWarningEchoCandidate(state, candidate);
        else if (text !== null) {
          for (const buffered of flushWarningEchoCandidate(state, candidate)) yield buffered;
        }
        continue;
      }

      if (sourceEvent.type === "response.content_part.added") {
        const candidate = warningCandidateFor(state, keys);
        if (candidate) {
          candidate.buffered.push(sourceEvent);
          continue;
        }
        if (keys.length) {
          createWarningEchoCandidate(state, keys).buffered.push(sourceEvent);
          continue;
        }
      }

      const textUpdate = warningTextUpdate(sourceEvent);
      if (
        sourceEvent.type === "response.output_text.delta" || sourceEvent.type === "response.output_text.done" ||
        sourceEvent.type === "response.content_part.done"
      ) {
        const candidate = warningCandidateFor(state, keys);
        if (candidate) {
          candidate.buffered.push(sourceEvent);
          applyWarningTextUpdate(candidate, textUpdate);
          if (isFailoverWarningText(candidate.text)) discardWarningEchoCandidate(state, candidate);
          else if (textUpdate?.replace || !isFailoverWarningPrefix(candidate.text)) {
            for (const buffered of flushWarningEchoCandidate(state, candidate)) yield buffered;
          }
          continue;
        }
        if (textUpdate && isFailoverWarningText(textUpdate.text)) {
          suppressKeys(keys);
          continue;
        }
      }

      if (sourceEvent.type === "response.output_item.done" && isAssistantMessageItem(item)) {
        const candidate = warningCandidateFor(state, keys);
        if (candidate) {
          candidate.buffered.push(sourceEvent);
          const text = messageText(item);
          if (text !== null) candidate.text = text;
          if (isFailoverWarningText(candidate.text)) discardWarningEchoCandidate(state, candidate);
          else {
            for (const buffered of flushWarningEchoCandidate(state, candidate)) yield buffered;
          }
          continue;
        }
        if (isGatewayFailoverWarningItem(item)) {
          suppressKeys(keys);
          continue;
        }
      }

      if (sourceEvent.type === "response.output" || sourceEvent.terminal) {
        const flushed = [
          ...resolveOutput(value.output),
          ...(isRecord(value.response) && !Array.isArray(value.response) ? resolveOutput(value.response.output) : []),
          ...(sourceEvent.terminal ? resolvePendingAtTerminal() : []),
        ];
        for (const buffered of flushed) yield buffered;
        const next: Record<string, unknown> = { ...value };
        const output = removeWarningEchoFromOutput(value.output);
        if (output) next.output = output;
        if (isRecord(value.response) && !Array.isArray(value.response)) {
          const response = { ...value.response };
          const responseOutput = removeWarningEchoFromOutput(response.output);
          if (responseOutput) response.output = responseOutput;
          next.response = response;
        }
        yield eventFromValue(next);
        continue;
      }

      yield sourceEvent;
    }

    for (const candidate of resolvePendingAtTerminal()) {
      yield candidate;
    }
  })() as ResponsesStreamIterator;

export const projectOpenRouterResponsesIterator = (
  iterator: ResponsesStreamIterator,
  registry: OpenRouterProjectionRegistry,
): ResponsesStreamIterator =>
  (async function* () {
    const states = new Map<string, CallState>();
    const aliases = new Map<string, CallState>();
    const getState = (value: Record<string, unknown>, item?: Record<string, unknown>): CallState => {
      const source = item ?? value;
      const key = callStateKey({
        ...value,
        ...source,
        ...(item?.id !== undefined ? { item_id: item.id } : {}),
        ...(item?.call_id !== undefined ? { call_id: item.call_id } : {}),
      });
      if (!key) return failProjection("OpenRouter tool event omitted a stable identity.");
      let state = aliases.get(key) ?? states.get(key);
      if (!state) {
        state = {
          entry: null,
          callId: null,
          itemId: null,
          outputIndex: outputIndex(value) ?? outputIndex(item ?? {}),
          projectedName: null,
          deltaArguments: "",
          completeArguments: null,
          nativeDoneEmitted: false,
          nativeItem: null,
        };
        states.set(key, state);
      }
      if (item) {
        const itemId = getString(item.id)?.trim();
        const callIdValue = getString(item.call_id)?.trim();
        setCallIdentity(state, item);
        if (itemId) aliases.set(`item:${itemId}`, state);
        if (callIdValue) aliases.set(`call:${callIdValue}`, state);
        if (state.outputIndex !== null) aliases.set(`index:${state.outputIndex}`, state);
      }
      const callIdValue = getString(value.call_id)?.trim();
      if (callIdValue) {
        if (state.callId && state.callId !== callIdValue) {
          failProjection("OpenRouter tool event changed call identity.");
        }
        state.callId = callIdValue;
        aliases.set(`call:${callIdValue}`, state);
      }
      const itemIdValue = getString(value.item_id)?.trim();
      if (itemIdValue) {
        if (state.itemId && state.itemId !== itemIdValue) {
          failProjection("OpenRouter tool event changed item identity.");
        }
        state.itemId = itemIdValue;
        aliases.set(`item:${itemIdValue}`, state);
      }
      const index = outputIndex(value);
      if (index !== null) {
        if (state.outputIndex !== null && state.outputIndex !== index) {
          failProjection(
            "OpenRouter tool event changed output identity.",
          );
        }
        state.outputIndex = index;
        aliases.set(`index:${index}`, state);
      }
      return state;
    };

    const setEntry = (state: CallState, item: Record<string, unknown>): void => {
      const type = getString(item.type);
      const entry = providerEntryForItem(item, registry);
      if (!entry) {
        return failProjection(
          `OpenRouter emitted an unknown or unadvertised tool: ${String(item.name ?? type)}`,
        );
      }
      if (state.entry && state.entry !== entry) failProjection("OpenRouter tool event changed tool identity.");
      state.entry = entry;
      state.projectedName = getString(item.name)?.trim() ?? state.projectedName;
      setCallIdentity(state, item);
    };

    const ingestItem = (
      item: Record<string, unknown>,
      value: Record<string, unknown>,
      allowIncomplete = false,
    ): { state: CallState; native: Record<string, unknown> } | null => {
      const type = getString(item.type);
      if (type !== "function_call" && type !== "custom_tool_call" && type !== "local_shell_call") return null;
      const state = getState(value, item);
      setEntry(state, item);
      if (type === "function_call") {
        if (item.arguments !== undefined) {
          rememberArguments(state, item.arguments, !allowIncomplete && item.status !== "in_progress");
        }
        if (state.entry?.originalType === "custom" || state.entry?.originalType === "local_shell") {
          // A projected function call is decoded below. A provider-native call is
          // accepted only when its item kind already matches the advertised kind.
          if (getString(item.name) === state.entry.originalName) {
            failProjection("A projected custom or local-shell tool was emitted as a generic native function.");
          }
        }
      } else if (type === "custom_tool_call") {
        if (state.entry?.originalType !== "custom") {
          failProjection(
            "OpenRouter custom-tool item kind does not match the registry.",
          );
        }
      } else if (state.entry?.originalType !== "local_shell") {
        failProjection("OpenRouter local-shell item kind does not match the registry.");
      }
      const native = type === "function_call"
        ? allowIncomplete && state.completeArguments === null
          ? state.entry?.originalType === "custom"
            ? {
              type: "custom_tool_call",
              ...(state.itemId ? { id: state.itemId } : {}),
              status: "in_progress",
              call_id: nativeCallId(item, state),
              name: state.entry.originalName,
              input: "",
            }
            : {
              type: "function_call",
              ...(state.itemId ? { id: state.itemId } : {}),
              status: "in_progress",
              name: state.entry?.originalName ?? getString(item.name) ?? "",
              arguments: state.deltaArguments,
              call_id: nativeCallId(item, state),
            }
          : itemFromState(state, item)
        : reverseDirectNativeItem(item, state.entry!);
      state.nativeItem = native;
      return { state, native };
    };

    type RewrittenOutput = Readonly<{
      output: unknown[];
      completed: readonly { native: Record<string, unknown>; state: CallState; outputIndex: number }[];
    }>;

    const rewriteOutput = (output: unknown, value: Record<string, unknown>): RewrittenOutput => {
      if (!Array.isArray(output)) return { output: [], completed: [] };
      const completed: { native: Record<string, unknown>; state: CallState; outputIndex: number }[] = [];
      const rewritten = output.map((raw, index) => {
        if (!isRecord(raw) || Array.isArray(raw)) return raw;
        const itemType = getString(raw.type);
        if (
          itemType !== "function_call" && itemType !== "custom_tool_call" && itemType !== "local_shell_call"
        ) return copyJson(raw);
        const currentOutputIndex = typeof raw.output_index === "number" && Number.isSafeInteger(raw.output_index)
          ? raw.output_index
          : index;
        const result = ingestItem(raw, { ...value, output_index: currentOutputIndex });
        if (result && getString(raw.status) !== "in_progress" && !result.state.nativeDoneEmitted) {
          result.state.nativeDoneEmitted = true;
          completed.push({ native: result.native, state: result.state, outputIndex: currentOutputIndex });
        }
        return result?.native ?? raw;
      });
      return { output: rewritten, completed };
    };

    const syntheticDoneEvents = (
      value: Record<string, unknown>,
      completed: readonly { native: Record<string, unknown>; state: CallState; outputIndex: number }[],
    ): ResponsesStreamEvent[] => {
      const responseId = getString(value.response_id)?.trim() ??
        (isRecord(value.response) && !Array.isArray(value.response) ? getString(value.response.id)?.trim() : null);
      return completed.flatMap(({ native, state, outputIndex }) => {
        const common: Record<string, unknown> = {
          ...(responseId ? { response_id: responseId } : {}),
          ...(state.itemId ? { item_id: state.itemId } : {}),
          output_index: outputIndex,
        };
        const events: ResponsesStreamEvent[] = [];
        if (state.entry?.originalType === "custom" && typeof native.input === "string") {
          events.push(eventFromValue({
            ...common,
            type: "response.custom_tool_call_input.done",
            input: native.input,
          }));
        }
        events.push(eventFromValue({ ...common, type: "response.output_item.done", item: native }));
        return events;
      });
    };

    for await (const event of filterWarningEchoes(iterator)) {
      const value = event.value;
      if (event.type === "response.output_item.added" && isRecord(value.item) && !Array.isArray(value.item)) {
        const item = value.item;
        const type = getString(item.type);
        if (type === "function_call" || type === "custom_tool_call" || type === "local_shell_call") {
          const result = ingestItem(item, value, true);
          if (!result) continue;
          if (type === "local_shell_call") continue;
          yield eventFromValue({ ...value, item: result.native });
          continue;
        }
      }
      if (event.type === "response.function_call_arguments.delta") {
        const state = getState(value);
        rememberArguments(state, value.delta, false);
        if (!state.entry) continue;
        if (state.entry.originalType === "function") yield event;
        continue;
      }
      if (event.type === "response.function_call_arguments.done") {
        const state = getState(value);
        rememberArguments(state, value.arguments, true);
        if (!state.entry) continue;
        if (state.entry.originalType !== "function") {
          parseArguments(state.completeArguments!, state.entry, "response.function_call_arguments.done");
        }
        if (state.entry.originalType === "function") {
          yield event;
        } else if (state.entry.originalType === "custom") {
          const native = itemFromState(state);
          yield eventFromValue({
            ...value,
            type: "response.custom_tool_call_input.done",
            input: native.input,
            arguments: undefined,
          });
          if (!state.nativeDoneEmitted) {
            state.nativeDoneEmitted = true;
            yield eventFromValue({
              ...value,
              type: "response.output_item.done",
              item: native,
            });
          }
        } else if (!state.nativeDoneEmitted) {
          state.nativeDoneEmitted = true;
          yield eventFromValue({ ...value, type: "response.output_item.done", item: itemFromState(state) });
        }
        continue;
      }
      if (event.type === "response.output_item.done" && isRecord(value.item) && !Array.isArray(value.item)) {
        const item = value.item;
        const type = getString(item.type);
        if (type === "function_call" || type === "custom_tool_call" || type === "local_shell_call") {
          const result = ingestItem(item, value);
          if (!result) continue;
          if (result.state.nativeDoneEmitted) continue;
          result.state.nativeDoneEmitted = true;
          yield eventFromValue({ ...value, item: result.native });
          continue;
        }
      }
      if (event.type === "response.output" && value.output !== undefined) {
        const rewritten = rewriteOutput(value.output, value);
        yield eventFromValue({ ...value, output: rewritten.output });
        for (const synthetic of syntheticDoneEvents(value, rewritten.completed)) yield synthetic;
        continue;
      }
      if (event.terminal && isRecord(value.response) && !Array.isArray(value.response)) {
        const response = { ...value.response };
        const rewritten = response.output === undefined
          ? { output: undefined, completed: [] }
          : rewriteOutput(response.output, value);
        if (response.output !== undefined) response.output = rewritten.output;
        for (const synthetic of syntheticDoneEvents(value, rewritten.completed)) yield synthetic;
        yield eventFromValue({ ...value, response });
        continue;
      }
      yield event;
    }
  })() as ResponsesStreamIterator;
