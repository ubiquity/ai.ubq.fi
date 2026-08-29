/**
 * Deterministic tool-argument validation feedback (plan m05).
 *
 * m04's `schemas.ts` validates against the canonical surface but reports only
 * the first problem.  The reliability layer needs **complete, deterministic,
 * machine-readable** feedback: every issue in one pass, a stable issue code
 * per problem class, and one concise corrective hint per issue so the model
 * can fix the call without a round trip.  This module is a pure read-only
 * view over the canonical schemas:
 *
 * - {@link validateToolArgumentsDetailed} checks every present argument
 *   (unknown keys rejected, required string parameters non-empty, arrays of
 *   strings), collects ALL issues, and never mutates the input.
 * - {@link renderValidationFeedback} renders one deterministic line used by
 *   the reliability harness as the model-facing tool error text; the closed
 *   {@link ToolErrorCode} `invalid_args` stays the envelope code, so run-level
 *   bookkeeping (m02 metrics) keeps working unchanged.
 *
 * This module performs no I/O and never executes tools.
 */

import { CANONICAL_TOOL_NAMES, TOOL_SCHEMAS } from "../tools/schemas.ts";

/** Stable issue classes emitted by the detailed validator. */
export type ArgumentIssueCode =
  | "unknown_tool"
  | "not_an_object"
  | "unexpected_argument"
  | "missing_required"
  | "wrong_type"
  | "non_empty"
  | "undefined_value"
  | "bad_array_item";

/** One argument-validation issue with a corrective hint. */
export interface ToolArgumentIssue {
  code: ArgumentIssueCode;
  /** Dotted property path, e.g. `arguments.path`. */
  location: string;
  /** Deterministic description. */
  message: string;
  /** Short corrective hint for the model. */
  hint: string;
}

export type DetailedValidationResult = Readonly<
  | { valid: true; arguments: Record<string, unknown>; issues: readonly ToolArgumentIssue[] }
  | { valid: false; arguments: Record<string, unknown>; issues: readonly ToolArgumentIssue[] }
>;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const typeLabel = (type: string): string => (type === "array" ? "an array of non-empty strings" : `a ${type}`);

const issue = (code: ArgumentIssueCode, location: string, message: string, hint: string): ToolArgumentIssue => ({
  code,
  location,
  message,
  hint,
});

/**
 * Validates a tool call against the canonical schema and reports EVERY
 * problem deterministically.  Unknown tools produce one `unknown_tool` issue;
 * any other malformed call produces one issue per problem, sorted by a stable
 * order (unknown keys first, then required-missing, then value problems in
 * declared-property order) so output is reproducible.
 */
export function validateToolArgumentsDetailed(tool: string, args: unknown): DetailedValidationResult {
  const schema = TOOL_SCHEMAS[tool as keyof typeof TOOL_SCHEMAS];
  if (schema === undefined) {
    const issues: ToolArgumentIssue[] = [{
      code: "unknown_tool",
      location: "tool",
      message: `unknown tool ${JSON.stringify(tool)}`,
      hint: `use one of: ${CANONICAL_TOOL_NAMES.join(", ")}`,
    }];
    return { valid: false, arguments: {}, issues };
  }
  if (!isRecord(args)) {
    return {
      valid: false,
      arguments: {},
      issues: [
        issue(
          "not_an_object",
          "arguments",
          "arguments must be a JSON object",
          "pass a JSON object with the tool's arguments",
        ),
      ],
    };
  }

  const issues: ToolArgumentIssue[] = [];
  const properties = schema.parameters.properties as Readonly<Record<string, Readonly<Record<string, unknown>>>>;
  const required = (schema.parameters.required as readonly string[]) ?? [];
  const ordered = Object.keys(properties);

  // 1. Unknown keys (stable order: input order, sorted for determinism).
  for (const key of Object.keys(args).sort()) {
    if (!(key in properties)) {
      issues.push(issue(
        "unexpected_argument",
        `arguments.${key}`,
        `unexpected argument ${JSON.stringify(key)}`,
        `remove ${JSON.stringify(key)}; allowed: ${ordered.join(", ") || "(none)"}`,
      ));
    }
  }
  // 2. Missing required arguments (declared order).
  for (const key of required) {
    if (!(key in args) || args[key] === undefined) {
      const param = properties[key] ?? {};
      issues.push(issue(
        "missing_required",
        `arguments.${key}`,
        `missing required argument ${JSON.stringify(key)}`,
        `provide ${JSON.stringify(key)} (${typeLabel(String(param.type ?? "string"))})`,
      ));
    }
  }
  // 3. Value problems (declared-property order).
  for (const key of ordered) {
    if (!(key in args) || args[key] === undefined) continue;
    const value = args[key];
    const param = properties[key] ?? {};
    const type = typeof param.type === "string" ? param.type : "string";
    const isNonEmpty = param.minLength === 1;
    if (value === undefined) {
      issues.push(issue(
        "undefined_value",
        `arguments.${key}`,
        `argument ${JSON.stringify(key)} must not be undefined`,
        `omit ${JSON.stringify(key)} or pass a ${typeLabel(type)}`,
      ));
    } else if (type === "string") {
      if (typeof value !== "string") {
        issues.push(issue(
          "wrong_type",
          `arguments.${key}`,
          `argument ${JSON.stringify(key)} must be a string, got ${typeof value}`,
          `pass a string for ${JSON.stringify(key)}`,
        ));
      } else if (isNonEmpty && value.length === 0) {
        issues.push(issue(
          "non_empty",
          `arguments.${key}`,
          `argument ${JSON.stringify(key)} must be a non-empty string`,
          `pass a non-empty string for ${JSON.stringify(key)}`,
        ));
      }
    } else if (type === "boolean") {
      if (typeof value !== "boolean") {
        issues.push(issue(
          "wrong_type",
          `arguments.${key}`,
          `argument ${JSON.stringify(key)} must be a boolean, got ${typeof value}`,
          `pass true or false for ${JSON.stringify(key)}`,
        ));
      }
    } else if (type === "array") {
      const items = Array.isArray(value) ? value : [];
      if (!Array.isArray(value)) {
        issues.push(issue(
          "wrong_type",
          `arguments.${key}`,
          `argument ${JSON.stringify(key)} must be an array of non-empty strings, got ${typeof value}`,
          `pass an array of non-empty strings for ${JSON.stringify(key)}`,
        ));
      } else {
        items.forEach((item, index) => {
          if (typeof item !== "string" || item.length === 0) {
            issues.push(issue(
              "bad_array_item",
              `arguments.${key}[${index}]`,
              `argument ${JSON.stringify(key)}[${index}] must be a non-empty string`,
              `pass an array of non-empty strings for ${JSON.stringify(key)}`,
            ));
          }
        });
      }
    }
  }

  return { valid: issues.length === 0, arguments: args, issues };
}

/**
 * Renders the complete feedback as ONE deterministic line.  Kept short so the
 * model-facing tool error stays within the m04 output budget while carrying
 * every issue with its code and corrective hint.
 */
export const renderValidationFeedback = (tool: string, result: DetailedValidationResult): string => {
  if (result.valid) return "";
  const parts = result.issues.map((i, index) => `#${index + 1}: ${i.code} ${i.location} — ${i.hint}`);
  return `invalid arguments (${result.issues.length} issue(s)) for ${JSON.stringify(tool)}: ${parts.join("; ")}`;
};

/** Short stable label for one invalid call (used in event bookkeeping). */
export const invalidCallLabel = (result: DetailedValidationResult): string => {
  const first = result.issues[0];
  return first === undefined ? "invalid_args" : `${first.code}:${first.location}`;
};
