#pragma once
// Port of src/harmony/tools/schemas.ts.
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
#include "uos/authority/contracts.hpp"
#include "uos/harmony/types.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::harmony::tools::schemas {
  /** Canonical tool names, in the order they are exposed to the model. */
  inline constexpr std::array<std::string_view, 9> CANONICAL_TOOL_NAMES{
    "filesystem.read",
    "filesystem.find",
    "filesystem.search",
    "browser.search",
    "browser.open",
    "browser.find",
    "shell.exec",
    "editor.apply_patch",
    "task.update_plan",
  };

  // (typeof CANONICAL_TOOL_NAMES)[number]
  using CanonicalToolName = std::string;

  /** The single strictness value all canonical definitions expose by default. */
  inline constexpr bool CANONICAL_TOOL_DEFAULT_STRICTNESS = false;

  /** One canonical tool: model-facing name, description and JSON Schema. */
  struct CanonicalToolSchema {
    CanonicalToolName name;
    std::string description;
    /**
     * JSON Schema for the `parameters` object (root `type: "object"`).
     * `additionalProperties: false`; string/boolean/array-of-string types only.
     */
    json::object parameters;
    friend bool operator==(const CanonicalToolSchema&, const CanonicalToolSchema&) = default;
  };

  namespace detail {
    // Builds TOOL_SCHEMAS from the module's private schema helpers (schemas.cpp).
    authority::contracts::Record<CanonicalToolSchema> buildToolSchemas();
  }

  /** Canonical tool registry, keyed by model-facing tool name. */
  // Readonly<Record<CanonicalToolName, CanonicalToolSchema>>: `TOOL_SCHEMAS.get(name)` reads an
  // own member only (see lookupToolSchema for the inherited Object.prototype names).
  inline const authority::contracts::Record<CanonicalToolSchema> TOOL_SCHEMAS = detail::buildToolSchemas();

  /** Returns the canonical schema for a tool name, or null when unknown. */
  // nullptr is null. For an Object.prototype name ("constructor", "toString", "__proto__", ...)
  // the TS plain-object lookup returns the inherited value, which is no schema: the C++ returns
  // nullptr, and validateToolArguments reproduces what the TS then does with that value.
  const CanonicalToolSchema* lookupToolSchema(std::string_view tool);

  // `{ valid: true; arguments: Record<string, unknown> }`
  struct ValidateToolArgumentsResultValid {
    static constexpr bool valid = true;
    json::object arguments;
    friend bool operator==(const ValidateToolArgumentsResultValid&, const ValidateToolArgumentsResultValid&) = default;
  };
  // `{ valid: false; reason: string }`
  struct ValidateToolArgumentsResultInvalid {
    static constexpr bool valid = false;
    std::string reason;
    friend bool operator==(const ValidateToolArgumentsResultInvalid&, const ValidateToolArgumentsResultInvalid&) = default;
  };
  using ValidateToolArgumentsResult = std::variant<ValidateToolArgumentsResultValid, ValidateToolArgumentsResultInvalid>;

  /**
   * Type-checks and normalizes tool arguments against the canonical schema.
   *
   * Every present argument is validated (not only required ones), unknown
   * arguments are rejected, and required string parameters must be non-empty.
   * The returned `arguments` object preserves the caller's values unchanged.
   */
  // `args: unknown`: nullptr is undefined. Throws rt::TypeError "undefined is not an object
  // (evaluating 'toolSchema.parameters.properties')" for an Object.prototype tool name with
  // object arguments, as the TS does.
  ValidateToolArgumentsResult validateToolArguments(std::string_view tool, const json::value& args);
  ValidateToolArgumentsResult validateToolArguments(std::string_view tool, const json::value* args);

  /** Parameter type view of a canonical schema, for tooling and compatibility shims. */
  // Values are "string" | "boolean" | "string[]".
  authority::contracts::Record<std::string> toolParameterTypes(const CanonicalToolSchema& toolSchema);

  // `opts: Readonly<{ strict?: boolean }> = {}` (the overload without it).
  struct ToolDefinitionsOptions {
    std::optional<bool> strict{};
  };

  /** Renders the whole canonical surface as m01 ToolDefinition entries. */
  std::vector<types::ToolDefinition> toolDefinitions();
  std::vector<types::ToolDefinition> toolDefinitions(const ToolDefinitionsOptions& opts);

  // `{ ok: true; names: readonly CanonicalToolName[] }`
  struct AssertCanonicalToolSchemasResultOk {
    static constexpr bool ok = true;
    std::vector<CanonicalToolName> names;
    friend bool operator==(const AssertCanonicalToolSchemasResultOk&, const AssertCanonicalToolSchemasResultOk&) = default;
  };
  // `{ ok: false; name: string; reason: string }`
  struct AssertCanonicalToolSchemasResultNotOk {
    static constexpr bool ok = false;
    std::string name;
    std::string reason;
    friend bool operator==(const AssertCanonicalToolSchemasResultNotOk&, const AssertCanonicalToolSchemasResultNotOk&) = default;
  };
  using AssertCanonicalToolSchemasResult = std::variant<AssertCanonicalToolSchemasResultOk, AssertCanonicalToolSchemasResultNotOk>;

  /**
   * Well-formedness proof for the canonical surface (used by focused tests).
   *
   * `strict: true` additionally requires every property to be listed in
   * `required` (the OpenAI strict-mode restriction). The canonical default is
   * `strict: false` because `editor.apply_patch` and `filesystem.find` keep
   * optional parameters by design.
   */
  AssertCanonicalToolSchemasResult assertCanonicalToolSchemas(bool strict);
}
