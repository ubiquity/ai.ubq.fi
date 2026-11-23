#pragma once
// Port of src/codex/responses-operations.ts.
// The Codex Responses operations installed onto one request's context.
#include "uos/codex/responses_state.hpp"

#include <memory>

namespace uos::codex::responses_operations {
  // `(ctx: CodexResponseContext): void`: assigns every CodexResponseOperations member of `ctx`. The
  // installed closures must not own `ctx` (see responses_state.hpp).
  void installCodexResponseOperations(std::shared_ptr<responses_state::CodexResponseContext> ctx);
}
