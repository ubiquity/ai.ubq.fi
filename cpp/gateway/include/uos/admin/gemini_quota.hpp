#pragma once
// Reads and refreshes the owner-private Antigravity subscription quota snapshot.
// Quota is obtained from the documented CLI `/usage` command, not synthetic or
// paid inference, and the OAuth credential remains inside `agy`.

#include "uos/provider/gemini.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::admin::gemini_quota {
  inline constexpr std::string_view GEMINI_QUOTA_FILE = "antigravity-quota.json";
  inline constexpr double GEMINI_QUOTA_STALE_AFTER_MS = 5 * 60'000;
  inline constexpr double GEMINI_QUOTA_REFRESH_FRESH_MS = 10'000;
  inline constexpr double GEMINI_QUOTA_REFRESH_DEADLINE_MS = 15'000;

  struct Bucket {
    std::optional<double> remaining_fraction{};
    std::optional<double> remaining_amount{};
    std::optional<std::string> reset_time{};
    std::optional<double> reset_in_seconds{};
    std::optional<bool> disabled{};
  };

  struct Snapshot {
    double observed_at_ms = 0;
    std::string status;  // observed | signed_out | invalid_event
    std::optional<std::string> plan_tier{};
    rt::OrderedMap<std::string, Bucket> quota{};
  };

  struct GeminiQuotaOptions {
    provider::gemini::GeminiCliRunner runner{};
    std::optional<rt::AbortSignal> signal{};
    bool force_refresh = false;
  };

  rt::Task<rt::Response> handleAdminGeminiQuota();
  rt::Task<rt::Response> handleAdminGeminiQuotaRefresh(rt::Request req);
  rt::Task<rt::Response> handleAdminGeminiQuotaRefresh(rt::Request req, GeminiQuotaOptions options);

  // Test seams keep the same filesystem parser while selecting an isolated `.data` directory.
  rt::Task<Snapshot> readSnapshot(std::string directory, double now_ms);
  rt::Task<Snapshot> refreshSnapshot(std::string directory, GeminiQuotaOptions options, double now_ms);

  std::optional<Snapshot> projectUsageCommand(const json::value& result, double observed_at_ms);
  json::value snapshotJson(const Snapshot& snapshot, double now_ms);
}
