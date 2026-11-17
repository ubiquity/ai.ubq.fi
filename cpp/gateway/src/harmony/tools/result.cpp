#include "uos/harmony/tools/result.hpp"

#include "uos/rt/string.hpp"
#include <utility>

namespace uos::harmony::tools::result {
  std::string clipToolText(std::string_view text, double limit) {
    return static_cast<double>(rt::length(text)) > limit ? rt::concat(rt::slice(text, 0, limit), "…[truncated]") : std::string(text);
  }

  ToolResult toolFailure(ToolErrorCode errorCode, std::string error) {
    return ToolResult{
      .ok = false,
      .error = std::move(error),
      .error_code = std::move(errorCode),
    };
  }

  ToolExecutionError::ToolExecutionError(ToolErrorCode code_, std::string message_) : rt::JsError(std::move(message_), "ToolExecutionError"), code(std::move(code_)) {}
}
