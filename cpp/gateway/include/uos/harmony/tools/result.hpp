#pragma once
/**
 * Canonical tool result envelope (plan m04).
 *
 * Every tool routed by `router.ts` returns one {@link ToolResult} — a
 * machine-readable envelope with a closed set of {@link ToolErrorCode}s, so
 * harnesses (m05 reliability detection, benchmark adapters) can classify
 * failures without parsing prose. The envelope is a structural superset of the
 * benchmark trajectory's `tool_result` shape: `ok`, `output`, `error`,
 * `error_code`, plus shell-specific `exit_code`/`stdout`/`stderr`.
 *
 * Limits keep every result bounded regardless of backend content.
 */
#include "uos/rt/error.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace uos::harmony::tools::result {
  /**
   * Closed set of machine-readable tool error codes:
   *   "invalid_args"  schema or tool-level argument rejection
   *   "path_escape"   path resolved outside the workspace root
   *   "write_scope"   write outside the task's allowed write scope
   *   "not_found"     file, directory or page does not exist
   *   "exec_failed"   shell command exited with a non-zero code
   *   "timeout"       shell command exceeded its time bound
   *   "patch_failed"  patch could not be applied (missing/ambiguous text, exists)
   *   "unavailable"   no backend is injected for this tool
   *   "internal"      unexpected backend failure
   */
  using ToolErrorCode = std::string;

  /** One canonical tool result. */
  struct ToolResult {
    bool ok = false;
    /** Model-facing text on success (clipped to the output limit). */
    std::optional<std::string> output{};
    /** Model-facing error text on failure (clipped to the output limit). */
    std::optional<std::string> error{};
    /** Machine-readable error code; present exactly when `ok` is false. */
    std::optional<ToolErrorCode> error_code{};
    /** shell.exec: the command's exit code (absent on timeout). */
    std::optional<double> exit_code{};
    /** shell.exec: captured standard output (clipped). */
    std::optional<std::string> stdout{};
    /** shell.exec: captured standard error (clipped). */
    std::optional<std::string> stderr{};

    friend bool operator==(const ToolResult&, const ToolResult&) = default;
  };

  /** Maximum characters of any single result output/error field. */
  inline constexpr double TOOL_OUTPUT_LIMIT = 8'000;
  /** Maximum lines returned by a single filesystem/browser search or find. */
  inline constexpr double SEARCH_LINE_LIMIT = 200;
  /** Default shell command time bound in milliseconds. */
  inline constexpr double SHELL_DEFAULT_TIMEOUT_MS = 20'000;

  /** Clips long tool text with a stable, indexable suffix. Lengths are UTF-16 code units. */
  std::string clipToolText(std::string_view text, double limit = TOOL_OUTPUT_LIMIT);

  /** Builds a failure envelope. */
  ToolResult toolFailure(ToolErrorCode errorCode, std::string error);

  /** Backend failure carrying a canonical, machine-readable code. */
  class ToolExecutionError : public rt::JsError {
  public:
    ToolErrorCode code;

    ToolExecutionError(ToolErrorCode code_, std::string message_);
  };
}
