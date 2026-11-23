#pragma once
// Port of src/provider/capacity-parse.ts.
// Provider capacity parsing and stored-snapshot decoding, split out of src/provider_capacity.ts.
//
// The module declares its helpers module-private and exports them in one `export { ... }` list;
// they are declared here in source order. This header includes uos/provider/capacity_contract.hpp
// for the record types those exported signatures use (the plan's header DAG lists no edge because
// it reads only `export const` declarations; capacity_contract.hpp includes no capacity_parse or
// capacity_history header, so there is no cycle). Readers of stored values take rt::KvValue (a
// json::value converts implicitly; a default-constructed KvValue is undefined); the parsers of the
// upstream usage payload take json::value plus a pointer overload (nullptr is undefined).
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::capacity_parse {
  // `email: string | null = null` (null and undefined alike).
  std::string codexAccountLabel(double slot, const std::optional<std::string>& email = std::nullopt);

  // `slot: 1 | 2`
  rt::KvKey providerCapacityLastAvailableKey(double slot);

  // `now: () => number`
  double safeNow(const std::function<double()>& now);

  // `(value: unknown): value is number`
  bool isSafeTimestamp(const rt::KvValue& value);

  std::string codexUsageUrl();

  std::vector<capacity_contract::ProviderCapacityAdditionalRateLimit> additionalRateLimitsForRouting(
    const std::vector<capacity_contract::ProviderCapacityAdditionalRateLimit>& limits, double snapshotAtMs);

  /**
   * Admin diagnostics only: the upstream overage balance and limit flag exactly
   * as returned. The balance stays the upstream decimal string; a missing or
   * malformed pair means the sample is unavailable rather than zero.
   */
  struct ProviderCapacityOverageUsage {
    std::optional<std::string> balance;             // string | null
    std::optional<bool> overage_limit_reached;  // boolean | null
    friend bool operator==(const ProviderCapacityOverageUsage&, const ProviderCapacityOverageUsage&) = default;
  };

  // `(value: unknown): ProviderCapacityOverageUsage | null`; std::nullopt is null.
  std::optional<ProviderCapacityOverageUsage> parseCodexOverageUsage(const json::value& value);
  std::optional<ProviderCapacityOverageUsage> parseCodexOverageUsage(const json::value* value);

  // `Readonly<{ primary: ProviderCapacityWindow | null; secondary: ProviderCapacityWindow | null;
  // additional_rate_limits: readonly ProviderCapacityAdditionalRateLimit[] }>`, parseCodexUsage's result.
  struct ParseCodexUsageResult {
    std::optional<capacity_contract::ProviderCapacityWindow> primary;    // ... | null
    std::optional<capacity_contract::ProviderCapacityWindow> secondary;  // ... | null
    std::vector<capacity_contract::ProviderCapacityAdditionalRateLimit> additional_rate_limits;
    friend bool operator==(const ParseCodexUsageResult&, const ParseCodexUsageResult&) = default;
  };

  // `(value: unknown)`; std::nullopt is null.
  std::optional<ParseCodexUsageResult> parseCodexUsage(const json::value& value);
  std::optional<ParseCodexUsageResult> parseCodexUsage(const json::value* value);

  // `(slot: 1 | 2, snapshotAtMs, failureKind = "not_configured", failureStatus: number | null = null,
  // observed = false, label = codexAccountLabel(slot), accountCohortId: string | null = null)`:
  // a std::nullopt `label` is the default label.
  capacity_contract::ProviderCapacityCodexSource unavailableCodexSource(double slot, double snapshotAtMs,
                                                                        capacity_contract::ProviderCapacityFailureKind failureKind = "not_configured",
                                                                        std::optional<double> failureStatus = std::nullopt, bool observed = false,
                                                                        std::optional<std::string> label = std::nullopt,
                                                                        std::optional<std::string> accountCohortId = std::nullopt);

  capacity_contract::ProviderCapacityMeteredSource unavailableMeteredSource(double snapshotAtMs);

  // `(value: unknown, fallbackSnapshotAtMs: number)`; std::nullopt is null.
  std::optional<capacity_contract::ProviderCapacityCodexSource> readStoredCodexSource(const rt::KvValue& value, double fallbackSnapshotAtMs);

  // `(value: unknown)`; std::nullopt is null.
  std::optional<capacity_contract::ProviderCapacitySnapshot> readStoredSnapshot(const rt::KvValue& value);

  // `kv: Kv` (non-null); std::nullopt is null (also for a failed read).
  rt::Task<std::optional<capacity_contract::ProviderCapacitySnapshot>> readCapacitySnapshot(std::shared_ptr<rt::Kv> kv);

  // `(value: unknown)`; std::nullopt is null.
  std::optional<capacity_contract::ProviderCapacityHistoryPoint> readStoredHistoryPoint(const rt::KvValue& value);

  // ---- JSON conversion (members in declaration order) --------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityOverageUsage& value);
  ProviderCapacityOverageUsage tag_invoke(const json::value_to_tag<ProviderCapacityOverageUsage>&, const json::value& value);
}
