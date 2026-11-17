#pragma once
// Port of src/provider/claude-code-usage-schema.ts.
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace uos::provider::claude_code_usage_schema {
  /** Native Claude Code 2.1.310 usage contract; these are meters, never routing guards. */
  inline constexpr std::array<std::string_view, 5> CLAUDE_USAGE_LEGACY_WINDOWS = {"five_hour", "seven_day", "seven_day_oauth_apps", "seven_day_opus", "seven_day_sonnet"};

  // The payload, meters and snapshots are stored and read back as wire JSON, so they stay
  // json::object views: a validated payload keeps every member it arrived with.
  using ClaudeCodeUsagePayload = json::object;
  // { id, label, kind, group, scope, severity, is_active, used_percent, resets_at, reset_at_ms, observed_at_ms }
  using ClaudeCodeUsageMeter = json::object;
  // { v: 1, observed_at_ms, payload, meters }
  using ClaudeCodeUsageSnapshot = json::object;

  /** Successful snapshots replace the complete prior shape; missing is not zero or last-known data. */
  // `payload: unknown`: nullptr is undefined.
  std::optional<ClaudeCodeUsageSnapshot> parseClaudeCodeUsageSnapshot(const json::value* payload, double observedAtMs);
  std::optional<ClaudeCodeUsageSnapshot> parseClaudeCodeUsageSnapshot(const json::value& payload, double observedAtMs);
  /** Recompute projections from validated provenance rather than trusting stored derived fields. */
  std::optional<ClaudeCodeUsageSnapshot> readClaudeCodeUsageSnapshot(const json::value* value);
  std::optional<ClaudeCodeUsageSnapshot> readClaudeCodeUsageSnapshot(const json::value& value);

  // Returns the validated value itself (every member it carries).
  std::optional<ClaudeCodeUsageMeter> readClaudeCodeUsageMeter(const json::value* value);
  std::optional<ClaudeCodeUsageMeter> readClaudeCodeUsageMeter(const json::value& value);
}
