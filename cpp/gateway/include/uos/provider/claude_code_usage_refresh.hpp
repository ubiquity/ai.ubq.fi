#pragma once
// Port of src/provider/claude-code-usage-refresh.ts.
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage.hpp"
#include <memory>
#include <optional>
#include <string_view>
#include <variant>
namespace uos::provider::claude_code_usage_refresh {
  inline constexpr double CLAUDE_CODE_USAGE_FRESH_MS = 30'000;
  struct ClaudeCodeUsageRefreshChanged { static constexpr std::string_view kind = "changed"; };
  struct ClaudeCodeUsageRefreshBusy { static constexpr std::string_view kind = "busy"; };
  struct ClaudeCodeUsageRefreshFresh { static constexpr std::string_view kind = "fresh"; claude_code_quota::ClaudeCodeUsageState usage; };
  struct ClaudeCodeUsageRefreshCompleted { static constexpr std::string_view kind = "completed"; claude_code_usage::ClaudeCodeUsageResult result; claude_code_quota::ClaudeCodeUsageState usage; };
  using ClaudeCodeUsageRefresh = std::variant<ClaudeCodeUsageRefreshChanged, ClaudeCodeUsageRefreshBusy, ClaudeCodeUsageRefreshFresh, ClaudeCodeUsageRefreshCompleted>;
  bool claudeCodeUsageIsFresh(const std::optional<claude_code_quota::ClaudeCodeQuotaState>& quota, double nowMs);
  rt::Task<ClaudeCodeUsageRefresh> refreshClaudeCodeUsage(std::shared_ptr<rt::Kv> kv, claude_code_accounts::ClaudeCodeAccountBinding binding);
  rt::Task<ClaudeCodeUsageRefresh> refreshClaudeCodeUsage(std::shared_ptr<rt::Kv> kv, claude_code_accounts::ClaudeCodeAccountBinding binding, claude_code_usage::FetchClaudeCodeUsageOptions options);
}
