#pragma once
// Port of src/codex/responses-runners.ts.
// The Codex Responses attempt runners installed onto one request's context.
#include "uos/codex/responses_state.hpp"

#include <memory>

namespace uos::codex::responses_runners {
  // `(ctx: CodexResponseContext): void`: assigns every CodexResponseRunners member of `ctx`. The
  // installed closures must not own `ctx` (see responses_state.hpp).
  void installCodexResponseRunners(std::shared_ptr<responses_state::CodexResponseContext> ctx);
}
