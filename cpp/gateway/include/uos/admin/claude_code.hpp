#pragma once
// Port of src/admin/claude-code.ts.
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

#include <functional>
#include <optional>

namespace uos::admin::claude_code {
  rt::Task<rt::Response> handleAdminClaudeCodeAuthGet();

  struct ClaudeCodeAuthOptions {
    provider::claude_code::ClaudeCodeEnvLookup env{};
  };
  rt::Task<rt::Response> handleAdminClaudeCodeAuthSet(rt::Request req);
  rt::Task<rt::Response> handleAdminClaudeCodeAuthSet(rt::Request req, ClaudeCodeAuthOptions options);

  struct ClaudeUsageRefreshOptions {
    provider::claude_code::ClaudeCodeEnvLookup env{};
    provider::claude_code::ClaudeCodeFetch fetcher{};
    std::function<double()> now{};
  };
  rt::Task<rt::Response> handleAdminClaudeCodeQuotaRefresh(rt::Request req);
  rt::Task<rt::Response> handleAdminClaudeCodeQuotaRefresh(rt::Request req, ClaudeUsageRefreshOptions options);

  rt::Task<rt::Response> handleAdminClaudeCodeAuthDelete();
  rt::Task<rt::Response> handleAdminClaudeCodeAuthDelete(std::optional<rt::Request> req);
  rt::Task<rt::Response> handleAdminClaudeCodeQuotaClear();
  rt::Task<rt::Response> handleAdminClaudeCodeQuotaClear(std::optional<rt::Request> req);
  rt::Task<rt::Response> handleAdminClaudeCodeAccess(rt::Request req);
  rt::Task<rt::Response> handleAdminClaudeCodeAccountActive(rt::Request req);
}
