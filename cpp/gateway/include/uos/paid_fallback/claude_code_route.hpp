#pragma once
// Port of src/paid-fallback/claude-code-route.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::paid_fallback::claude_code_route {
  struct SubscriptionOptions {
    std::string model;
    std::optional<rt::AbortSignal> signal{};
    const openai_telemetry::UsageContext* usageContext = nullptr;
    provider::claude_code::ClaudeCodeEnvLookup env{};
    provider::claude_code::ClaudeCodeFetch fetcher{};
  };
  /** One durable active subscription; only exact-current full-pool proof authorizes paid continuation. */
  rt::Task<std::optional<openai_telemetry::RoutedResponsesUpstream>> attemptClaudeCodeSubscription(json::object body, SubscriptionOptions options);
}
