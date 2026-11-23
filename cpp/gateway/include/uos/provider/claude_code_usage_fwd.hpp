#pragma once
// Forward header of src/provider/claude-code-usage.ts for the w04c1 import cycle:
// claude-code-quota stores ClaudeCodeUsageErrorKind in ClaudeCodeUsageState, while
// claude-code-usage needs claude-code-quota's observation type. The alias is defined here, in
// claude_code_usage's own namespace; uos/provider/claude_code_usage.hpp includes this header.
#include <string>

namespace uos::provider::claude_code_usage {
  // "scope_missing" | "auth_invalid" | "rate_limited" | "unsupported_credential" | "unreachable" |
  // "upstream_error" | "http_error" | "invalid_response"
  using ClaudeCodeUsageErrorKind = std::string;
}
