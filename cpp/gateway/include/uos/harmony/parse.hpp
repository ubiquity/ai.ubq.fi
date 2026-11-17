#pragma once
// Port of src/harmony/parse.ts.
/**
 * Harmony assistant-output parser (plan m01).
 *
 * Tolerant scanner for the Harmony special-token grammar emitted by gpt-oss
 * models.  It accepts both documented header orders ("recipient in the role
 * section" and "recipient in the channel section"), missing
 * `<|start|>assistant` prefixes (a completion continues the prompt's open
 * assistant message), omitted `<|end|>` before `<|call|>`/`<|return|>`, and
 * truncated tail text.
 *
 * Grammar reference:
 * https://github.com/openai/harmony/blob/main/docs/format.md
 */
#include "uos/harmony/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace uos::harmony::parse {
  /**
   * Strips the `functions.` namespace prefix from a recipient to recover the
   * model-facing tool name (mirrors vLLM's `extract_function_from_recipient`).
   * Recipients outside the `functions.` namespace (e.g. built-in browser
   * tools) are returned unchanged.
   */
  std::string functionNameFromRecipient(std::string_view recipient);

  /**
   * Normalizes tool arguments: valid JSON is re-serialized compactly so replay
   * and instrumentation are deterministic; invalid JSON stays verbatim (the
   * shared application pipeline rejects it later).
   */
  std::string normalizeToolArguments(std::string_view argumentsText);

  /** Normalizes raw Harmony messages into adapter turns. */
  std::vector<types::HarmonyTurn> harmonyTurnsFromMessages(const std::vector<types::HarmonyRawMessage>& messages);

  /**
   * Scans raw completion text into Harmony messages.  The parser is optimistic:
   * any non-empty content is retained, and only unambiguous structure is
   * interpreted.
   */
  types::HarmonyParseResult parseHarmonyOutput(std::string_view text);
}
