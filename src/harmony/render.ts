/**
 * Harmony prompt rendering (plan m01).
 *
 * Renders the system and developer message *content* the way the OpenAI
 * Harmony library does, so a `gpt-oss-120b` request can carry a stable
 * model-facing contract without relying on the provider's own tool-schema
 * translation.  See https://github.com/openai/harmony/blob/main/docs/format.md
 */

import type { HarmonyChannel, HarmonyReasoningEffort, NativeResponseFormat, ToolDefinition } from "./types.ts";

export type SystemRenderOptions = Readonly<{
  /** Date rendered as `Current date: <value>`. The caller passes an ISO day. */
  currentDate: string;
  reasoningEffort: HarmonyReasoningEffort;
  identity?: string;
  knowledgeCutoff?: string;
  channels?: readonly HarmonyChannel[];
  /**
   * Namespace that receives user function calls.  Pass `null` to omit the
   * "Calls to these tools must go to the commentary channel" note (classifier
   * requests expose no tools).
   */
  toolNamespace?: string | null;
}>;

export type DeveloperRenderOptions = Readonly<{
  instructions: string;
  tools?: readonly ToolDefinition[];
  responseFormat?: NativeResponseFormat;
  /** TS namespace wrapping the rendered functions (default `functions`). */
  namespace?: string;
}>;

export const HARMONY_ASSISTANT_CHANNELS = ["analysis", "commentary", "final"] as const;

const channelList = (channels: readonly HarmonyChannel[]): string => channels.join(", ");

/**
 * The documented system message: identity, meta dates, reasoning effort,
 * valid channels and the function-call channel note.
 */
export const renderSystemMessage = (options: SystemRenderOptions): string => {
  const identity = options.identity ?? "You are ChatGPT, a large language model trained by OpenAI.";
  const lines: string[] = [identity];
  lines.push(`Knowledge cutoff: ${options.knowledgeCutoff ?? "2024-06"}`);
  lines.push(`Current date: ${options.currentDate}`);
  lines.push("");
  lines.push(`Reasoning: ${options.reasoningEffort}`);
  lines.push("");
  lines.push(
    `# Valid channels: ${channelList(options.channels ?? HARMONY_ASSISTANT_CHANNELS)}. ` +
      "Channel must be included for every message.",
  );
  const toolNamespace = options.toolNamespace === undefined ? "functions" : options.toolNamespace;
  if (toolNamespace !== null) {
    lines.push(`Calls to these tools must go to the commentary channel: '${toolNamespace}'.`);
  }
  return lines.join("\n");
};

/**
 * Escapes a string for a Harmony TS literal (double quotes, matching the
 * renderer's JSON-literal style for string enum values).
 */
export const quoteString = (value: string): string =>
  `"${value.replaceAll("\\", "\\\\").replaceAll('"', '\\"').replaceAll("\n", "\\n")}"`;

const jsonLiteral = (value: unknown): string => {
  if (typeof value === "string") return quoteString(value);
  return JSON.stringify(value);
};

const indent = (text: string, level: number): string =>
  text.split("\n").map((line) => (line.length ? "  ".repeat(level) + line : line)).join("\n");

const commentPrefix = (text: string): string | null => {
  const comment = text.trim();
  return comment ? indent(`// ${comment}`, 1) : null;
};

/**
 * Renders a JSON Schema subset to the Harmony TypeScript-like type syntax
 * documented for function definitions.  Unknown constructs degrade to `any`.
 */
export const harmonyTypeFromJsonSchema = (schemaValue: unknown): string => {
  if (!schemaValue || typeof schemaValue !== "object") return "any";
  const schema = schemaValue as Record<string, unknown>;

  const constEnum = schema.enum;
  if (Array.isArray(constEnum) && constEnum.length > 0) {
    return constEnum.map((entry) => jsonLiteral(entry)).join(" | ");
  }

  const type = typeof schema.type === "string" ? schema.type : undefined;
  const anyOf = Array.isArray(schema.anyOf) ? schema.anyOf : Array.isArray(schema.oneOf) ? schema.oneOf : null;
  if (anyOf && anyOf.length > 0) return anyOf.map((entry) => harmonyTypeFromJsonSchema(entry)).join(" | ");

  if (type === "object" || (type === undefined && schema.properties)) {
    const properties = schema.properties;
    const required = new Set(
      Array.isArray(schema.required) ? schema.required.filter((name): name is string => typeof name === "string") : [],
    );
    if (!properties || typeof properties !== "object" || Array.isArray(properties)) return "any";
    const fields = Object.entries(properties as Record<string, unknown>);
    if (fields.length === 0) return "{}";
    const lines: string[] = [];
    for (const [name, field] of fields) {
      const fieldSchema = field && typeof field === "object" ? (field as Record<string, unknown>) : {};
      const description = commentPrefix(
        typeof fieldSchema.description === "string" ? fieldSchema.description : "",
      );
      if (description) lines.push(description);
      const optional = required.has(name) ? "" : "?";
      const defaultValue = fieldSchema.default === undefined ? null : fieldSchema.default;
      const suffix = defaultValue === null ? "," : `, // default: ${jsonLiteral(defaultValue)}`;
      lines.push(`${name}${optional}: ${harmonyTypeFromJsonSchema(field)}${suffix}`);
    }
    return `{\n${indent(lines.join("\n"), 1)}\n}`;
  }

  if (type === "array" || (type === undefined && schema.items)) {
    return `${harmonyTypeFromJsonSchema(schema.items)}[]`;
  }

  if (
    type === "string" || type === "number" || type === "integer" || type === "boolean" ||
    typeof type === "string"
  ) {
    return type;
  }
  return "any";
};

/**
 * Renders one tool definition in the exact documented shape:
 *
 *   // Gets the current weather in the provided location.
 *   type get_current_weather = (_: {
 *   // The city and state, e.g. San Francisco, CA
 *   location: string,
 *   format?: "celsius" | "fahrenheit", // default: celsius
 *   }) => any;
 */
export const renderToolDefinition = (tool: ToolDefinition): string => {
  const description = commentPrefix(tool.description);
  const name = tool.name;
  const parameters = tool.parameters ?? {};
  if (harmonyTypeFromJsonSchema(parameters) === "any") {
    return `${description ? description + "\n" : ""}type ${name} = () => any;`;
  }
  return `${description ? description + "\n" : ""}type ${name} = (_: ${harmonyTypeFromJsonSchema(parameters)}) => any;`;
};

/**
 * Renders the `# Tools` section of the developer message.  The provider's
 * mixed-strictness constraint does not apply here: the native style renders
 * schemas as Harmony types and exposes no `tools` field.
 */
export const renderDeveloperMessage = (options: DeveloperRenderOptions): string => {
  const lines: string[] = ["# Instructions", "", options.instructions];
  if (options.tools && options.tools.length > 0) {
    const namespace = options.namespace ?? "functions";
    lines.push("", "# Tools", "", `## ${namespace}`, "", `namespace ${namespace} {`, "");
    for (const tool of options.tools) {
      lines.push(renderToolDefinition(tool), "");
    }
    lines.push(`} // namespace ${namespace}`);
  }
  if (options.responseFormat) {
    lines.push("", "# Response Formats", "", `## ${options.responseFormat.formatName}`);
    if (options.responseFormat.description) lines.push("", `// ${options.responseFormat.description}`);
    lines.push("", JSON.stringify(options.responseFormat.schema));
  }
  return lines.join("\n");
};
