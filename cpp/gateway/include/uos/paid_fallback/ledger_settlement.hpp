#pragma once
// Port of src/paid-fallback/ledger-settlement.ts.
// Paid-fallback ledger v3: settlement layer, split out of src/paid_fallback_ledger.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>. The five functions of the module's closing
// `export { ... }` list are declared where the TS defines them.
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>

namespace uos::paid_fallback::ledger_settlement {
  // `PaidFallbackReconciliationLeaseV3 | null`: std::nullopt is null.
  rt::Task<std::optional<authority::native_paid_contracts::PaidFallbackReconciliationLeaseV3>> acquireReconciliationLease(std::shared_ptr<rt::Kv> kv, std::string keyId,
                                                                                                                          double now);

  rt::Task<void> releaseReconciliationLease(std::shared_ptr<rt::Kv> kv, std::string keyId, authority::native_paid_contracts::PaidFallbackReconciliationLeaseV3 lease);

  // Throws rt::JsError "Paid fallback reconciliation scheduling changed concurrently." as the TS does.
  rt::Task<void> _expeditePaidFallbackReconciliationV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation, double now);

  /**
   * Records bounded backoff after a provider log is unavailable or invalid.
   * This writes durable state only: the event-driven policy does not arm a timer
   * or otherwise deliver a future reconciliation to an idle Mac.
   */
  // `number | null`: std::nullopt is null.
  rt::Task<std::optional<double>> deferPaidFallbackReconciliationV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId, double now);

  // The value settlePaidFallbackRequestV3 resolves to: `Readonly<{ settled: boolean; retry_delay_ms: number | null }>`.
  struct SettlePaidFallbackRequestV3Result {
    bool settled;
    std::optional<double> retry_delay_ms;
    friend bool operator==(const SettlePaidFallbackRequestV3Result&, const SettlePaidFallbackRequestV3Result&) = default;
  };

  // `providerLog: MeteredTokenLogEntry` (provider/metered.hpp aliases it to this record);
  // `correlation: "provider_request_id" | "surplus_reservation" = "provider_request_id"`;
  // `surplusBuyerCostMicro?: number` (std::nullopt is undefined). Throws rt::JsError "Paid fallback
  // settlement changed concurrently." after MAX_CAS_ATTEMPTS lost commits.
  rt::Task<SettlePaidFallbackRequestV3Result> settlePaidFallbackRequestV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId,
                                                                          authority::native_paid_contracts::NativePaidProviderLog providerLog, double now);
  rt::Task<SettlePaidFallbackRequestV3Result> settlePaidFallbackRequestV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId,
                                                                          authority::native_paid_contracts::NativePaidProviderLog providerLog, double now,
                                                                          authority::native_paid_contracts::NativePaidCorrelation correlation);
  rt::Task<SettlePaidFallbackRequestV3Result> settlePaidFallbackRequestV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId,
                                                                          authority::native_paid_contracts::NativePaidProviderLog providerLog, double now,
                                                                          authority::native_paid_contracts::NativePaidCorrelation correlation,
                                                                          std::optional<double> surplusBuyerCostMicro);

  struct PaidFallbackUsageSettlementV3 {
    /** Internal direct-settlement key; never exposed as an upstream request ID. */
    std::string settlement_request_id;
    double provider_quota;
    double input_tokens;
    rt::Nullable<double> cached_input_tokens{};
    double output_tokens;
    std::string model;
    double created_at_ms;
    /** Surplus-only exact provider amount; quota conversion can round odd microcredits. */
    std::optional<double> buyer_cost_micro{};
    friend bool operator==(const PaidFallbackUsageSettlementV3&, const PaidFallbackUsageSettlementV3&) = default;
  };

  /**
   * Settles a provider whose response contains authoritative usage instead of
   * exposing the OpenLux token-log endpoint. The same CAS path is used as the
   * asynchronous OpenLux reconciliation, so duplicate terminal observations
   * remain idempotent.
   */
  rt::Task<bool> settlePaidFallbackUsageV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation, PaidFallbackUsageSettlementV3 usage);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const SettlePaidFallbackRequestV3Result& value);
  SettlePaidFallbackRequestV3Result tag_invoke(const json::value_to_tag<SettlePaidFallbackRequestV3Result>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackUsageSettlementV3& value);
  PaidFallbackUsageSettlementV3 tag_invoke(const json::value_to_tag<PaidFallbackUsageSettlementV3>&, const json::value& value);
}
