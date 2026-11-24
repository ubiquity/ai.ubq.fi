#pragma once
// Port of src/codex/responses-operations.ts.
// The Codex Responses operations installed onto one request's context.
#include "uos/codex/responses_state.hpp"

#include <memory>

namespace uos::codex::responses_operations {
  // `(ctx: CodexResponseContext): void`: assigns every CodexResponseOperations member of `ctx`. The
  // installed closures must not own `ctx` (see responses_state.hpp).
  void installCodexResponseOperations(std::shared_ptr<responses_state::CodexResponseContext> ctx);

  struct BlockedSubscriptionRequest {
    std::string url;
    std::string body;
    rt::Headers headers;
    std::optional<std::string> model;
    std::optional<rt::AbortSignal> signal;
    rt::OrderedSet<std::string> attemptedAccountIds;
  };

  struct BlockedSubscriptionAttempt {
    routing_state::RoutingAccount routing;
    rt::Response response;
  };

  // Recover a blocked subscription using the caller's actual upstream transport.
  rt::Task<std::optional<BlockedSubscriptionAttempt>> recoverBlockedSubscription(BlockedSubscriptionRequest request);
}
