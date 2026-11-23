#pragma once
// Port of src/harmony/render.ts.
/**
 * Harmony prompt rendering (plan m01).
 *
 * Renders the system and developer message *content* the way the OpenAI
 * Harmony library does, so a `gpt-oss-120b` request can carry a stable
 * model-facing contract without relying on the provider's own tool-schema
 * translation.  See https://github.com/openai/harmony/blob/main/docs/format.md
 */
#include "uos/harmony/types.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::harmony::render {
  struct SystemRenderOptions {
    /** Date rendered as `Current date: <value>`. The caller passes an ISO day. */
    std::string currentDate;
    types::HarmonyReasoningEffort reasoningEffort;
    std::optional<std::string> identity{};
    std::optional<std::string> knowledgeCutoff{};
    std::optional<std::vector<types::HarmonyChannel>> channels{};
    /**
     * Namespace that receives user function calls.  Pass `null` to omit the
     * "Calls to these tools must go to the commentary channel" note (classifier
     * requests expose no tools).
     */
    rt::Nullable<std::string> toolNamespace{};
  };

  struct DeveloperRenderOptions {
    std::string instructions;
    std::optional<std::vector<types::ToolDefinition>> tools{};
    std::optional<types::NativeResponseFormat> responseFormat{};
    /** TS namespace wrapping the rendered functions (default `functions`). */
    std::optional<std::string> namespace_{};
  };

  inline constexpr std::array<std::string_view, 3> HARMONY_ASSISTANT_CHANNELS{"analysis", "commentary", "final"};

  /**
   * The documented system message: identity, meta dates, reasoning effort,
   * valid channels and the function-call channel note.
   */
  std::string renderSystemMessage(const SystemRenderOptions& options);

  /**
   * Escapes a string for a Harmony TS literal (double quotes, matching the
   * renderer's JSON-literal style for string enum values).
   */
  std::string quoteString(std::string_view value);

  /**
   * Renders a JSON Schema subset to the Harmony TypeScript-like type syntax
   * documented for function definitions.  Unknown constructs degrade to `any`.
   */
  // `schemaValue: unknown`: nullptr is undefined.
  std::string harmonyTypeFromJsonSchema(const json::value& schemaValue);
  std::string harmonyTypeFromJsonSchema(const json::value* schemaValue);

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
  std::string renderToolDefinition(const types::ToolDefinition& tool);

  /**
   * Renders the `# Tools` section of the developer message.  The provider's
   * mixed-strictness constraint does not apply here: the native style renders
   * schemas as Harmony types and exposes no `tools` field.
   */
  std::string renderDeveloperMessage(const DeveloperRenderOptions& options);
}
