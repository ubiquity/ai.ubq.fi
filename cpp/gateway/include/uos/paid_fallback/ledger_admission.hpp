#pragma once
// Port of src/paid-fallback/ledger-admission.ts.
// Paid-fallback ledger v3: admission layer, split out of src/paid_fallback_ledger.ts.
//
// `kvOverride?: Kv | null` is std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined,
// which reads getKv(); a null pointer is null), with an overload without it.
#include "uos/authority/native_paid.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::paid_fallback::ledger_admission {
  struct PaidFallbackDeletionV3Deleted {
    static constexpr std::string_view kind = "deleted";
    double deleted_requests;
    double deleted_windows;
    double deleted_pending;
    double deleted_leases;
    friend bool operator==(const PaidFallbackDeletionV3Deleted&, const PaidFallbackDeletionV3Deleted&) = default;
  };
  // `Readonly<{ kind: "blocked"; outstanding: PaidFallbackOutstandingV3 }>` and
  // `Readonly<{ kind: "unavailable" }>`: the TS returns planPaidFallbackDeletionV3's decision itself,
  // so these alternatives are that plan's structs.
  using PaidFallbackDeletionV3Blocked = authority::native_paid::PaidFallbackDeletionPlanV3Blocked;
  using PaidFallbackDeletionV3Unavailable = authority::native_paid::PaidFallbackDeletionPlanV3Unavailable;
  using PaidFallbackDeletionV3 = std::variant<PaidFallbackDeletionV3Deleted, PaidFallbackDeletionV3Blocked, PaidFallbackDeletionV3Unavailable>;

  // Throws rt::JsError "Paid fallback deletion guard changed concurrently." / "Paid fallback state
  // deletion changed concurrently." as the TS does.
  rt::Task<PaidFallbackDeletionV3> deletePaidFallbackStateV3(std::string keyId);
  rt::Task<PaidFallbackDeletionV3> deletePaidFallbackStateV3(std::string keyId, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  using PaidFallbackAdmissionBlockedReason = std::string;  // "limit_exceeded" | "invalid_policy" | "concurrent_update"

  // The value admitPaidFallbackV3 resolves to:
  // `Readonly<{ kind: "reserved"; reservation: PaidFallbackAdmissionV3 }> | Readonly<{ kind: "blocked"; reason: PaidFallbackAdmissionBlockedReason }>`.
  struct AdmitPaidFallbackV3ResultReserved {
    static constexpr std::string_view kind = "reserved";
    authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation;
    friend bool operator==(const AdmitPaidFallbackV3ResultReserved&, const AdmitPaidFallbackV3ResultReserved&) = default;
  };
  struct AdmitPaidFallbackV3ResultBlocked {
    static constexpr std::string_view kind = "blocked";
    PaidFallbackAdmissionBlockedReason reason;
    friend bool operator==(const AdmitPaidFallbackV3ResultBlocked&, const AdmitPaidFallbackV3ResultBlocked&) = default;
  };
  using AdmitPaidFallbackV3Result = std::variant<AdmitPaidFallbackV3ResultReserved, AdmitPaidFallbackV3ResultBlocked>;

  rt::Task<AdmitPaidFallbackV3Result> admitPaidFallbackV3(ledger_state::AdmissionInput input);

  // Throws rt::JsError "Paid fallback request changed concurrently." after MAX_CAS_ATTEMPTS lost commits.
  rt::Task<void> updatePaidFallbackRequestV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation,
                                             authority::native_paid_contracts::PaidFallbackRequestLifecyclePatchV3 patch);

  rt::Task<void> releaseUndispatchedPaidFallbackV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation);

  /**
   * Releases a reservation after a durable dispatch intent was written but
   * before provider fetch was invoked. This must never be called once provider
   * fetch can have started.
   */
  rt::Task<void> releasePaidFallbackBeforeProviderFetchV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation);
}
