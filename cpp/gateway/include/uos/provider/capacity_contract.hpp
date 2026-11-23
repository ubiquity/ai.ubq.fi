#pragma once
// Port of src/provider/capacity-contract.ts.
//
// Literal-typed members are plain members with their literal as default (`v: 1`, `label:
// "Metered fallback"`); the `source` discriminant of the ProviderCapacitySource arms is a static
// member, as for every variant alternative. `1 | 2` slots are double. Fixed-length tuples are
// std::array (homogeneous) or std::tuple (ProviderCapacityHistoryPoint::sources).
#include "uos/metered_quota.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage_schema.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/regex.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace uos::provider::capacity_contract {
  /**
   * Shared provider-capacity storage keys. Keeping the key outside the sampler
   * avoids a routing-to-sampler import cycle while allowing routing to reconcile
   * snapshots written by an earlier sampler run.
   */
  inline const rt::KvKey PROVIDER_CAPACITY_SNAPSHOT_KEY{"uos_ai", "provider_capacity", "v1", "snapshot"};

  /**
   * Width of one capacity-history bucket: also the debounce for event-driven
   * sampling, so the first observation in a bucket samples and later ones do not.
   */
  inline constexpr double PROVIDER_CAPACITY_HISTORY_BUCKET_MS = 15 * 60'000;

  inline const rt::KvKey PROVIDER_CAPACITY_LEASE_KEY{"uos_ai", "provider_capacity", "v1", "lease"};
  inline const rt::KvKey PROVIDER_CAPACITY_HISTORY_KEY_PREFIX{"uos_ai", "provider_capacity", "v1", "history"};
  inline const rt::KvKey PROVIDER_CAPACITY_LAST_AVAILABLE_KEY_PREFIX{"uos_ai", "provider_capacity", "v1", "last_available"};
  inline constexpr double PROVIDER_CAPACITY_HISTORY_RETENTION_MS = 7 * 24 * 60 * 60'000;
  // Keep this name for callers that used the old snapshot retention constant.
  inline constexpr double PROVIDER_CAPACITY_LEASE_MS = 10'000;
  inline constexpr double PROVIDER_CAPACITY_COLD_WAIT_MS = 2'000;
  inline constexpr double PROVIDER_CAPACITY_SOURCE_STALE_MS = 30 * 60'000;
  inline constexpr double PROVIDER_CAPACITY_CODEX_TIMEOUT_MS = 8'000;
  inline constexpr double PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS = 25;

  inline constexpr double ADDITIONAL_WINDOW_UNANCHORED_TOLERANCE_MS = 60'000;
  // `/^[a-f0-9]{64}$/` (no `g`/`y`: use the const overloads).
  inline const rt::Regex SHA256_HEX{"^[a-f0-9]{64}$"};

  // "available" | "stale" | "unavailable"
  using CapacityState = std::string;
  // "live" | "persisted" | "stale" | "unavailable"
  using ProviderCapacityViewState = std::string;
  // "not_configured" | "http_error" | "upstream_error" | "unreachable" | "invalid_response"
  using ProviderCapacityFailureKind = std::string;

  struct ProviderCapacityWindow {
    std::optional<double> limit_window_seconds;  // number | null
    std::optional<double> used_percent;          // number | null
    std::optional<double> reset_at_ms;           // number | null
    friend bool operator==(const ProviderCapacityWindow&, const ProviderCapacityWindow&) = default;
  };

  struct ProviderCapacityAdditionalRateLimit {
    // `Readonly<{ primary: ProviderCapacityWindow | null; secondary: ProviderCapacityWindow | null }>`
    // (the Codex source's `windows` has the same shape and type).
    struct Windows {
      std::optional<ProviderCapacityWindow> primary;    // ... | null
      std::optional<ProviderCapacityWindow> secondary;  // ... | null
      friend bool operator==(const Windows&, const Windows&) = default;
    };

    std::string limit_name;
    std::optional<std::string> metered_feature;  // string | null
    Windows windows;
    friend bool operator==(const ProviderCapacityAdditionalRateLimit&, const ProviderCapacityAdditionalRateLimit&) = default;
  };

  // `export type ProviderCapacitySource = Readonly<{ source: "codex"; ... }> | Readonly<{ source:
  // "metered"; ... }>`. The TS names the two arms further down
  // (`ProviderCapacityCodexSource = Extract<ProviderCapacitySource, { source: "codex" }>`,
  // `ProviderCapacityMeteredSource = Extract<..., { source: "metered" }>`); C++ defines them here as
  // the variant's alternatives.
  struct ProviderCapacityCodexSource {
    using Windows = ProviderCapacityAdditionalRateLimit::Windows;

    static constexpr std::string_view source = "codex";
    std::string label;
    double slot;                                  // `1 | 2`
    std::optional<std::string> account_cohort_id;  // string | null
    CapacityState state;
    std::optional<double> source_observed_at_ms;  // number | null
    double snapshot_at_ms;
    std::optional<ProviderCapacityFailureKind> failure_kind;  // ... | null
    std::optional<double> failure_status;                     // number | null
    Windows windows;
    std::vector<ProviderCapacityAdditionalRateLimit> additional_rate_limits;
    friend bool operator==(const ProviderCapacityCodexSource&, const ProviderCapacityCodexSource&) = default;
  };

  struct ProviderCapacityMeteredSource {
    struct Wallet {
      std::optional<double> balance_credits;                 // number | null
      std::optional<double> baseline_credits;                // number | null
      std::optional<double> refill_cycle_remaining_percent;  // number | null
      std::optional<double> refill_cycle_used_percent;       // number | null
      std::optional<bool> unlimited_quota;                   // boolean | null
      std::optional<double> total_available;                 // number | null
      std::optional<double> total_granted;                   // number | null
      std::optional<double> total_used;                      // number | null
      std::optional<double> cycle_started_at_ms;             // number | null
      std::optional<double> last_credit_at_ms;               // number | null
      // `MeteredQuotaSnapshot["state"]["confidence"] | null`
      std::optional<metered_quota::MeteredQuotaConfidence> confidence;
      // `MeteredQuotaSnapshot["cache_state"] | null`
      std::optional<metered_quota::MeteredQuotaCacheState> cache_state;
      std::nullptr_t reset_at_ms = nullptr;  // `null`
      friend bool operator==(const Wallet&, const Wallet&) = default;
    };

    static constexpr std::string_view source = "metered";
    std::string label = "Metered fallback";  // `"Metered fallback"`
    CapacityState state;
    std::optional<double> source_observed_at_ms;  // number | null
    double snapshot_at_ms;
    Wallet wallet;
    friend bool operator==(const ProviderCapacityMeteredSource&, const ProviderCapacityMeteredSource&) = default;
  };

  using ProviderCapacitySource = std::variant<ProviderCapacityCodexSource, ProviderCapacityMeteredSource>;

  /**
   * The Claude subscription analytics row. It is computed live for the admin
   * read from the tier's own credential, workspace, guard, and quota state, so
   * it never depends on the stored Codex/metered snapshot record.
   */
  struct ProviderCapacityClaudeWindow {
    std::optional<std::string> status;  // string | null
    /** Percentage of the window consumed, normalized from the upstream fraction. */
    std::optional<double> used_percent;    // number | null
    std::optional<double> reset_at_ms;     // number | null
    std::optional<double> observed_at_ms;  // number | null
    friend bool operator==(const ProviderCapacityClaudeWindow&, const ProviderCapacityClaudeWindow&) = default;
  };

  struct ProviderCapacityClaudeStandardLimit {
    std::string family;
    std::optional<double> limit;        // number | null
    std::optional<double> remaining;    // number | null
    std::optional<double> reset_at_ms;  // number | null
    friend bool operator==(const ProviderCapacityClaudeStandardLimit&, const ProviderCapacityClaudeStandardLimit&) = default;
  };

  struct ProviderCapacityClaudeError {
    std::string kind;
    std::optional<double> status;        // number | null
    std::optional<std::string> message;  // string | null
    double at_ms;
    friend bool operator==(const ProviderCapacityClaudeError&, const ProviderCapacityClaudeError&) = default;
  };

  struct ProviderCapacityClaudeSource {
    // `Readonly<{ five_hour: ProviderCapacityClaudeWindow | null; seven_day: ProviderCapacityClaudeWindow | null }>`
    // (ClaudeCapacityHistoryPoint's `windows` has the same shape and type).
    struct Windows {
      std::optional<ProviderCapacityClaudeWindow> five_hour;  // ... | null
      std::optional<ProviderCapacityClaudeWindow> seven_day;  // ... | null
      friend bool operator==(const Windows&, const Windows&) = default;
    };

    std::optional<claude_code_usage_schema::ClaudeCodeUsageSnapshot> usage_snapshot{};
    std::optional<bool> usage_stale{};
    std::string source = "claude_code";         // `"claude_code"`
    std::string label = "Claude subscription";  // `"Claude subscription"`
    // `?: string | null`: the active-account lookup compares it with a nullable id, so undefined
    // (the unavailable row) and null (an unbound row) differ.
    rt::Nullable<std::string> account_id{};
    rt::Nullable<std::string> account_cohort_id{};
    CapacityState state;
    std::optional<double> source_observed_at_ms;  // number | null
    double snapshot_at_ms;
    std::optional<ProviderCapacityFailureKind> failure_kind;  // ... | null
    std::optional<double> failure_status;                     // number | null
    /** Resolved credential direction, never the secret itself. */
    std::optional<std::string> credential_kind;  // "api_key" | "oauth" | null
    /** The workspace the tier would name, from KV, environment, or discovery. */
    std::optional<std::string> workspace_id;  // string | null
    Windows windows;
    std::vector<ProviderCapacityClaudeStandardLimit> standard;
    std::optional<claude_code_quota::ClaudeCodeUsageState> usage;  // ... | null
    std::optional<double> blocked_until_ms;                         // number | null
    std::optional<double> cooldown_until_ms;                        // number | null
    std::optional<ProviderCapacityClaudeError> last_error;          // ... | null
    friend bool operator==(const ProviderCapacityClaudeSource&, const ProviderCapacityClaudeSource&) = default;
  };

  // `export type ProviderCapacityCodexSource = Extract<ProviderCapacitySource, { source: "codex" }>`
  // and `ProviderCapacityMeteredSource`: the structs defined above.

  struct ProviderCapacitySnapshot {
    double snapshot_at_ms;
    double stale_after_ms;
    // `readonly [ProviderCapacitySource, ProviderCapacitySource, ProviderCapacitySource]`
    std::array<ProviderCapacitySource, 3> sources;
    friend bool operator==(const ProviderCapacitySnapshot&, const ProviderCapacitySnapshot&) = default;
  };

  struct ProviderCapacityHistoryPoint {
    double bucket_start_at_ms;
    double sampled_at_ms;
    // `readonly [ProviderCapacityCodexSource, ProviderCapacityCodexSource, ProviderCapacityMeteredSource]`
    std::tuple<ProviderCapacityCodexSource, ProviderCapacityCodexSource, ProviderCapacityMeteredSource> sources;
    friend bool operator==(const ProviderCapacityHistoryPoint&, const ProviderCapacityHistoryPoint&) = default;
  };

  // ClaudeCapacityHistoryPoint and ClaudeCapacityHistoryView follow ProviderCapacityView in the TS;
  // C++ needs them complete before the view's members.

  /** Additive Claude evidence; never widen the persisted Codex/metered tuple. */
  // Members in the order the TS builds a point (claude-capacity-history's pointForObservation,
  // whose result is stored and served as is): the optional `source` and `usage_snapshot` sit where
  // that object literal spreads them.
  struct ClaudeCapacityHistoryPoint {
    // `Readonly<ProviderCapacityClaudeStandardLimit & { observed_at_ms: number | null }>`
    struct StandardLimit : ProviderCapacityClaudeStandardLimit {
      std::optional<double> observed_at_ms;  // number | null
      friend bool operator==(const StandardLimit&, const StandardLimit&) = default;
    };

    double v = 1;  // `1`
    std::string point_id;
    double bucket_start_at_ms;
    double sampled_at_ms;
    std::optional<double> source_observed_at_ms;  // number | null
    std::string account_id;
    std::string account_cohort_id;
    std::string credential_kind;              // "oauth" | "api_key"
    std::optional<std::string> workspace_id;  // string | null
    std::string state;                        // "available" | "unavailable"
    std::optional<double> response_status;    // number | null
    std::optional<std::string> source{};      // `"response_headers"`
    ProviderCapacityClaudeSource::Windows windows;
    std::optional<claude_code_usage_schema::ClaudeCodeUsageSnapshot> usage_snapshot{};
    std::vector<StandardLimit> standard;
    friend bool operator==(const ClaudeCapacityHistoryPoint&, const ClaudeCapacityHistoryPoint&) = default;
  };

  // Members in the order claude-capacity-history's readClaudeCapacityHistory builds the view
  // (`{ ...base, scan, truncated, points }`).
  struct ClaudeCapacityHistoryView {
    double v = 1;  // `1`
    double bucket_ms;
    double retention_ms;
    double stale_after_ms;
    std::string scan;  // "ok" | "unavailable"
    bool truncated;
    std::vector<ClaudeCapacityHistoryPoint> points;
    friend bool operator==(const ClaudeCapacityHistoryView&, const ClaudeCapacityHistoryView&) = default;
  };

  // `Readonly<ProviderCapacitySnapshot & { claude?; claude_accounts?; claude_history?; cache_state;
  // history; reset_events; rate_limit_reset_events; downtime_events }>`. Members in the order
  // capacity.ts builds a view (the snapshot spread, then the projection, then the Claude rows that
  // the admin read appends).
  struct ProviderCapacityView : ProviderCapacitySnapshot {
    ProviderCapacityViewState cache_state;
    std::vector<ProviderCapacityHistoryPoint> history;
    std::vector<capacity_events::ProviderCapacityResetEvent> reset_events;
    std::vector<capacity_events::ProviderCapacityRateLimitResetEvent> rate_limit_reset_events;
    std::vector<capacity_events::ProviderCapacityDowntimeEvent> downtime_events;
    /**
     * Claude subscription analytics row. Present on admin reads; absent from
     * internal callers that only need the Codex/metered snapshot.
     */
    rt::Nullable<ProviderCapacityClaudeSource> claude{};
    /** Validated configured roster; `claude` remains the exact active projection. */
    std::optional<std::vector<ProviderCapacityClaudeSource>> claude_accounts{};
    std::optional<ClaudeCapacityHistoryView> claude_history{};
    friend bool operator==(const ProviderCapacityView&, const ProviderCapacityView&) = default;
  };

  // `export type ProviderCapacityFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  using ProviderCapacityFetch = rt::Fetch;

  struct ProviderCapacitySnapshotOptions {
    // `kv?: Kv | null`: std::nullopt is undefined (getKv()); a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    ProviderCapacityFetch fetcher{};  // empty: undefined
    std::function<double()> now{};    // empty: Date.now
    std::optional<rt::AbortSignal> signal{};
    std::function<std::string()> createLeaseOwner{};  // empty: crypto.randomUUID
  };

  struct CapacityLease {
    std::string owner;
    double lease_until_ms;
    friend bool operator==(const CapacityLease&, const CapacityLease&) = default;
  };

  struct StoredRateLimitObservation {
    double sampled_at_ms;
    ProviderCapacityCodexSource source;
    friend bool operator==(const StoredRateLimitObservation&, const StoredRateLimitObservation&) = default;
  };

  // ---- JSON conversions of the stored / served records (members in declaration order; a static
  // `source` discriminant is written first; the metered wallet's `reset_at_ms` as null). The
  // fixed-length `sources` tuples are arrays. -------------------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityWindow& value);
  ProviderCapacityWindow tag_invoke(const json::value_to_tag<ProviderCapacityWindow>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityAdditionalRateLimit::Windows& value);
  ProviderCapacityAdditionalRateLimit::Windows tag_invoke(const json::value_to_tag<ProviderCapacityAdditionalRateLimit::Windows>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityAdditionalRateLimit& value);
  ProviderCapacityAdditionalRateLimit tag_invoke(const json::value_to_tag<ProviderCapacityAdditionalRateLimit>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityCodexSource& value);
  ProviderCapacityCodexSource tag_invoke(const json::value_to_tag<ProviderCapacityCodexSource>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityMeteredSource::Wallet& value);
  ProviderCapacityMeteredSource::Wallet tag_invoke(const json::value_to_tag<ProviderCapacityMeteredSource::Wallet>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityMeteredSource& value);
  ProviderCapacityMeteredSource tag_invoke(const json::value_to_tag<ProviderCapacityMeteredSource>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacitySource& value);
  ProviderCapacitySource tag_invoke(const json::value_to_tag<ProviderCapacitySource>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeWindow& value);
  ProviderCapacityClaudeWindow tag_invoke(const json::value_to_tag<ProviderCapacityClaudeWindow>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeStandardLimit& value);
  ProviderCapacityClaudeStandardLimit tag_invoke(const json::value_to_tag<ProviderCapacityClaudeStandardLimit>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeError& value);
  ProviderCapacityClaudeError tag_invoke(const json::value_to_tag<ProviderCapacityClaudeError>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeSource::Windows& value);
  ProviderCapacityClaudeSource::Windows tag_invoke(const json::value_to_tag<ProviderCapacityClaudeSource::Windows>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeSource& value);
  ProviderCapacityClaudeSource tag_invoke(const json::value_to_tag<ProviderCapacityClaudeSource>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacitySnapshot& value);
  ProviderCapacitySnapshot tag_invoke(const json::value_to_tag<ProviderCapacitySnapshot>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityHistoryPoint& value);
  ProviderCapacityHistoryPoint tag_invoke(const json::value_to_tag<ProviderCapacityHistoryPoint>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityHistoryPoint::StandardLimit& value);
  ClaudeCapacityHistoryPoint::StandardLimit tag_invoke(const json::value_to_tag<ClaudeCapacityHistoryPoint::StandardLimit>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityHistoryPoint& value);
  ClaudeCapacityHistoryPoint tag_invoke(const json::value_to_tag<ClaudeCapacityHistoryPoint>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityHistoryView& value);
  ClaudeCapacityHistoryView tag_invoke(const json::value_to_tag<ClaudeCapacityHistoryView>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityView& value);
  ProviderCapacityView tag_invoke(const json::value_to_tag<ProviderCapacityView>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CapacityLease& value);
  CapacityLease tag_invoke(const json::value_to_tag<CapacityLease>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const StoredRateLimitObservation& value);
  StoredRateLimitObservation tag_invoke(const json::value_to_tag<StoredRateLimitObservation>&, const json::value& value);
}
