#pragma once
// Port of src/harmony/tools/router.ts.
#include "uos/harmony/tools/backend.hpp"
#include "uos/harmony/tools/result.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::harmony::tools::router {
  /** Per-dispatch options; all bounds have stable defaults. */
  struct RunToolOptions {
    /** Whole-run abort signal (also aborts in-flight shell commands). */
    std::optional<rt::AbortSignal> signal{};
    /** Shell command time bound; default 20s. */
    std::optional<double> shellTimeoutMs{};
    /** Output limit per envelope field; default 8000 chars. */
    std::optional<double> outputLimit{};
    /** Search/find line limit; default 200 lines. */
    std::optional<double> searchLineLimit{};
  };

  /**
   * Routes one canonical tool call to its backend and returns a machine-readable
   * result envelope. Never throws for tool-level failures; unexpected backend
   * exceptions become `internal` envelopes.
   */
  [[nodiscard]] rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, json::value args);
  [[nodiscard]] rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, json::value args, RunToolOptions opts);
  // nullptr is undefined; a json::value containing nullptr is null.
  [[nodiscard]] rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, const json::value* args);
  [[nodiscard]] rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, const json::value* args, RunToolOptions opts);
}
