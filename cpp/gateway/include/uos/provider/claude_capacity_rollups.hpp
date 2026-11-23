#pragma once
// Port of src/provider/claude-capacity-rollups.ts.
//
// `kv: Kv | null` is std::shared_ptr<rt::Kv> (a null pointer is null). The guards and readers
// validate stored values, so they take rt::KvValue (a json::value or a double converts implicitly;
// a default-constructed KvValue is undefined). The readers return the declared members of a
// value they accept (the TS returns the value itself).
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/claude_code_usage_schema.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::provider::claude_capacity_rollups {
  inline const rt::KvKey CLAUDE_CAPACITY_ROLLUP_PREFIX{"uos_ai", "provider_capacity", "claude", "v1", "rollup"};
  inline constexpr double CLAUDE_CAPACITY_READ_LIMIT = 20'000;
  // "five_hour" | "seven_day"
  using ClaudeCapacityWindowId = std::string;

  struct ClaudeCapacityRollupPoint {
    double v = 1;  // `1`
    double bucket_start_at_ms;
    std::string account_id;
    std::string account_cohort_id;
    std::string credential_kind;              // "oauth" | "api_key"
    std::optional<std::string> workspace_id;  // string | null
    ClaudeCapacityWindowId window;
    std::optional<double> reset_at_ms;  // number | null
    double samples;
    double available_sample_count;
    double unavailable_sample_count;
    std::optional<double> first_source_observed_at_ms;  // number | null
    std::optional<double> last_source_observed_at_ms;   // number | null
    double first_sampled_at_ms;
    double last_sampled_at_ms;
    std::optional<double> min_used_percent;   // number | null
    std::optional<double> max_used_percent;   // number | null
    std::optional<double> last_used_percent;  // number | null
    std::optional<std::string> last_status;   // string | null
    friend bool operator==(const ClaudeCapacityRollupPoint&, const ClaudeCapacityRollupPoint&) = default;
  };

  // `(value: unknown): value is number`
  bool claudeCapacityTimestamp(const rt::KvValue& value);
  // `(value: unknown): value is number | null`
  bool claudeCapacityOptionalTimestamp(const rt::KvValue& value);
  // `(value: unknown): value is number | null`
  bool claudeCapacityPercent(const rt::KvValue& value);
  // `(value: unknown): value is string`
  bool claudeCapacityIdentity(const rt::KvValue& value);

  // `(value: unknown): ClaudeCapacityRollupPoint | null`; std::nullopt is null.
  std::optional<ClaudeCapacityRollupPoint> readClaudeCapacityRollup(const rt::KvValue& value);

  // `window: ClaudeCapacityWindowId`
  rt::KvKey claudeCapacityRollupKey(const capacity_contract::ClaudeCapacityHistoryPoint& point, std::string_view window);

  /** Counts retained observations, not requests or inferred subscription entitlement. */
  // `previous: ClaudeCapacityRollupPoint | null`; throws Error("Claude rollup binding mismatch").
  ClaudeCapacityRollupPoint mergeClaudeCapacityRollup(const std::optional<ClaudeCapacityRollupPoint>& previous,
                                                      const capacity_contract::ClaudeCapacityHistoryPoint& point, std::string_view window);

  // `Readonly<{ scan: "ok" | "unavailable"; truncated: boolean; points: readonly ClaudeCapacityRollupPoint[] }>`
  struct ListClaudeCapacityRollupsResult {
    std::string scan;  // "ok" | "unavailable"
    bool truncated;
    std::vector<ClaudeCapacityRollupPoint> points;
    friend bool operator==(const ListClaudeCapacityRollupsResult&, const ListClaudeCapacityRollupsResult&) = default;
  };

  rt::Task<ListClaudeCapacityRollupsResult> listClaudeCapacityRollups(std::shared_ptr<rt::Kv> kv, double sinceMs, double nowMs);

  /** Direct meters have a separate namespace; no dynamic meter enters canonical quota guards. */
  inline const rt::KvKey CLAUDE_USAGE_ROLLUP_PREFIX{"uos_ai", "provider_capacity", "claude", "v1", "usage_rollup"};

  struct ClaudeUsageRollup {
    double v = 1;  // `1`
    double bucket_start_at_ms;
    std::string account_id;
    std::string account_cohort_id;
    std::string credential_kind;              // "oauth" | "api_key"
    std::optional<std::string> workspace_id;  // string | null
    claude_code_usage_schema::ClaudeCodeUsageMeter meter;
    double samples;
    double first_observed_at_ms;
    double last_observed_at_ms;
    std::optional<double> min_used_percent;  // number | null
    std::optional<double> max_used_percent;  // number | null
    friend bool operator==(const ClaudeUsageRollup&, const ClaudeUsageRollup&) = default;
  };

  // `point: Pick<ClaudeCapacityHistoryPoint, "account_id" | "account_cohort_id">` of claudeUsageRollupKey.
  struct ClaudeUsageRollupKeyPoint {
    std::string account_id;
    std::string account_cohort_id;
  };

  rt::Task<rt::KvKey> claudeUsageRollupKey(ClaudeUsageRollupKeyPoint point, claude_code_usage_schema::ClaudeCodeUsageMeter meter);

  // `(value: unknown): ClaudeUsageRollup | null`; std::nullopt is null.
  std::optional<ClaudeUsageRollup> readClaudeUsageRollup(const rt::KvValue& value);

  // `previous: ClaudeUsageRollup | null`; throws Error("Claude usage rollup binding mismatch").
  ClaudeUsageRollup mergeClaudeUsageRollup(const std::optional<ClaudeUsageRollup>& previous, const capacity_contract::ClaudeCapacityHistoryPoint& point,
                                           const claude_code_usage_schema::ClaudeCodeUsageMeter& meter);

  // `Readonly<{ scan: "ok" | "unavailable"; truncated: boolean; points: readonly ClaudeUsageRollup[] }>`
  struct ListClaudeUsageRollupsResult {
    std::string scan;  // "ok" | "unavailable"
    bool truncated;
    std::vector<ClaudeUsageRollup> points;
    friend bool operator==(const ListClaudeUsageRollupsResult&, const ListClaudeUsageRollupsResult&) = default;
  };

  rt::Task<ListClaudeUsageRollupsResult> listClaudeUsageRollups(std::shared_ptr<rt::Kv> kv, double sinceMs, double nowMs);

  // ---- JSON conversions (members in declaration order) -------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityRollupPoint& value);
  ClaudeCapacityRollupPoint tag_invoke(const json::value_to_tag<ClaudeCapacityRollupPoint>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ListClaudeCapacityRollupsResult& value);
  ListClaudeCapacityRollupsResult tag_invoke(const json::value_to_tag<ListClaudeCapacityRollupsResult>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeUsageRollup& value);
  ClaudeUsageRollup tag_invoke(const json::value_to_tag<ClaudeUsageRollup>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ListClaudeUsageRollupsResult& value);
  ListClaudeUsageRollupsResult tag_invoke(const json::value_to_tag<ListClaudeUsageRollupsResult>&, const json::value& value);
}
