#pragma once
// Discriminated unions are std::variant over one struct per alternative, named
// <Union><Discriminant>; the discriminant is a static member. The snake_case rows
// have boost::json conversions: value_from writes the members in declaration order
// (undefined optional members omitted, null written, numbers canonical); value_to
// throws rt::TypeError naming the first member the C++ type cannot hold and drops
// members the type does not declare.
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace uos::authority::native_kernel_contracts {
  /** Source-owned kernel V2 data. These private snapshots are populated only by trusted server adapters. */
  using KernelQuotaScope = std::string;  // "repo" | "org"

  struct KernelQuotaIdentity {
    KernelQuotaScope scope;
    std::string owner;
    std::optional<std::string> repo;  // string | undefined
    friend bool operator==(const KernelQuotaIdentity&, const KernelQuotaIdentity&) = default;
  };

  struct KernelQuotaConstants {
    double noUsageLimit;
    double noExpirationMs;
    friend bool operator==(const KernelQuotaConstants&, const KernelQuotaConstants&) = default;
  };

  /** Opaque storage revisions and expiry metadata are never supplied by an API caller. */
  // `value: T | null`; with the default T (unknown) that is a json::value holding null.
  template <class T = json::value> struct KernelQuotaRead {
    std::conditional_t<std::is_same_v<T, json::value>, json::value, std::optional<T>> value;
    std::optional<std::string> revision;
    rt::Nullable<double> expiresAtMs{};
    friend bool operator==(const KernelQuotaRead&, const KernelQuotaRead&) = default;
  };

  struct KernelQuotaDefaults {
    double limit;
    double windowMs;
    friend bool operator==(const KernelQuotaDefaults&, const KernelQuotaDefaults&) = default;
  };

  struct KernelQuotaPolicyV2 {
    double v = 2;
    KernelQuotaScope scope;
    std::string owner;
    std::optional<std::string> repo{};
    double usage_limit_requests;
    double window_ms;
    double expires_at_ms;
    double created_at_ms;
    double updated_at_ms;
    friend bool operator==(const KernelQuotaPolicyV2&, const KernelQuotaPolicyV2&) = default;
  };

  struct KernelQuotaWindowV2 {
    double v = 2;
    KernelQuotaScope scope;
    std::string owner;
    std::optional<std::string> repo{};
    double usage_requests;
    double reserved_requests;
    double usage_reset_at_ms;
    double applied_window_ms;
    double created_at_ms;
    double updated_at_ms;
    friend bool operator==(const KernelQuotaWindowV2&, const KernelQuotaWindowV2&) = default;
  };

  struct KernelQuotaReservationRowV2 {
    double v = 2;
    KernelQuotaScope scope;
    std::string owner;
    std::optional<std::string> repo{};
    std::string request_id;
    std::string route;
    double window_created_at_ms;
    double window_reset_at_ms;
    std::string state;                           // "reserved" | "committed" | "released"
    std::optional<std::string> terminal_intent;  // "committed" | "released" | null
    double reserved_at_ms;
    double lease_expires_at_ms;
    std::optional<double> committed_at_ms;
    std::optional<double> released_at_ms;
    std::optional<std::string> release_reason;
    friend bool operator==(const KernelQuotaReservationRowV2&, const KernelQuotaReservationRowV2&) = default;
  };

  struct KernelDefaultWindowCutoverV2 {
    double v = 2;
    std::string id;
    double created_at_ms;
    double expires_at_ms;
    friend bool operator==(const KernelDefaultWindowCutoverV2&, const KernelDefaultWindowCutoverV2&) = default;
  };

  struct KernelQuotaEffectivePolicy {
    std::optional<KernelQuotaPolicyV2> policy;
    double limit;
    double windowMs;
    double expiresAtMs;
    std::string source;  // "default" | "kv"
    friend bool operator==(const KernelQuotaEffectivePolicy&, const KernelQuotaEffectivePolicy&) = default;
  };

  struct KernelQuotaEffectiveWindow {
    KernelQuotaWindowV2 window;
    bool needsWrite;
    friend bool operator==(const KernelQuotaEffectiveWindow&, const KernelQuotaEffectiveWindow&) = default;
  };

  struct KernelQuotaPolicySetOptions {
    std::optional<bool> resetUsage{};
    std::optional<double> windowMs{};
    std::optional<double> expiresAtMs{};
    friend bool operator==(const KernelQuotaPolicySetOptions&, const KernelQuotaPolicySetOptions&) = default;
  };

  using KernelReservationSettlement = std::string;  // "settled" | "terminal_mismatch" | "missing" | "invalid" | "conflict"

  // KernelQuotaIdentity & { requestId, route, windowCreatedAtMs }
  struct KernelQuotaReservationIdentity : KernelQuotaIdentity {
    std::string requestId;
    std::string route;
    double windowCreatedAtMs;
    friend bool operator==(const KernelQuotaReservationIdentity&, const KernelQuotaReservationIdentity&) = default;
  };

  struct KernelQuotaEligibilitySnapshot {
    KernelQuotaRead<> policy;
    std::optional<KernelQuotaRead<>> repoGuard;
    KernelQuotaRead<> defaultCutover;
    std::optional<std::string> expectedRepoPolicy{};  // "present" | "absent"
    friend bool operator==(const KernelQuotaEligibilitySnapshot&, const KernelQuotaEligibilitySnapshot&) = default;
  };

  struct KernelQuotaRefusal {
    static constexpr std::string_view kind = "refused";
    double status;  // 429 | 503
    std::string message;
    friend bool operator==(const KernelQuotaRefusal&, const KernelQuotaRefusal&) = default;
  };

  struct KernelQuotaEligibilityPlanEligible {
    static constexpr std::string_view kind = "eligible";
    KernelQuotaEffectivePolicy effective;
    friend bool operator==(const KernelQuotaEligibilityPlanEligible&, const KernelQuotaEligibilityPlanEligible&) = default;
  };
  using KernelQuotaEligibilityPlan = std::variant<KernelQuotaRefusal, KernelQuotaEligibilityPlanEligible>;

  /** Closed family write sets. Adapters derive fixed keys and retain complete row CAS checks. */
  struct KernelQuotaReservePlanFull {
    static constexpr std::string_view kind = "full";
    KernelQuotaWindowV2 window;
    double limit;
    friend bool operator==(const KernelQuotaReservePlanFull&, const KernelQuotaReservePlanFull&) = default;
  };
  struct KernelQuotaReservePlanReserve {
    static constexpr std::string_view kind = "reserve";
    KernelQuotaReservationRowV2 reservation;
    KernelQuotaWindowV2 window;
    double expireIn;
    friend bool operator==(const KernelQuotaReservePlanReserve&, const KernelQuotaReservePlanReserve&) = default;
  };
  using KernelQuotaReservePlan = std::variant<KernelQuotaRefusal, KernelQuotaReservePlanFull, KernelQuotaReservePlanReserve>;

  struct KernelQuotaTerminalPlanUnchanged {
    static constexpr std::string_view kind = "unchanged";
    KernelReservationSettlement outcome;
    friend bool operator==(const KernelQuotaTerminalPlanUnchanged&, const KernelQuotaTerminalPlanUnchanged&) = default;
  };
  struct KernelQuotaTerminalPlanIntent {
    static constexpr std::string_view kind = "intent";
    KernelQuotaReservationRowV2 reservation;
    double expireIn;
    double leaseExpiresAtMs;
    friend bool operator==(const KernelQuotaTerminalPlanIntent&, const KernelQuotaTerminalPlanIntent&) = default;
  };
  using KernelQuotaTerminalPlan = std::variant<KernelQuotaTerminalPlanUnchanged, KernelQuotaTerminalPlanIntent>;

  // The same object type as KernelQuotaTerminalPlan's `unchanged` alternative.
  using KernelQuotaSettlePlanUnchanged = KernelQuotaTerminalPlanUnchanged;
  struct KernelQuotaSettlePlanSettle {
    static constexpr std::string_view kind = "settle";
    KernelQuotaReservationRowV2 reservation;
    std::optional<KernelQuotaWindowV2> window;
    double expireIn;
    friend bool operator==(const KernelQuotaSettlePlanSettle&, const KernelQuotaSettlePlanSettle&) = default;
  };
  using KernelQuotaSettlePlan = std::variant<KernelQuotaSettlePlanUnchanged, KernelQuotaSettlePlanSettle>;

  struct KernelQuotaCutoverPlanRefused {
    static constexpr std::string_view kind = "refused";
    std::string reason;  // "concurrent_change" | "unavailable"
    friend bool operator==(const KernelQuotaCutoverPlanRefused&, const KernelQuotaCutoverPlanRefused&) = default;
  };
  struct KernelQuotaCutoverPlanAcquire {
    static constexpr std::string_view kind = "acquire";
    KernelDefaultWindowCutoverV2 marker;
    double expireIn;
    friend bool operator==(const KernelQuotaCutoverPlanAcquire&, const KernelQuotaCutoverPlanAcquire&) = default;
  };
  using KernelQuotaCutoverPlan = std::variant<KernelQuotaCutoverPlanRefused, KernelQuotaCutoverPlanAcquire>;

  struct KernelQuotaPolicyWritePlan {
    KernelQuotaPolicyV2 policy;
    KernelQuotaWindowV2 window;
    friend bool operator==(const KernelQuotaPolicyWritePlan&, const KernelQuotaPolicyWritePlan&) = default;
  };

  // ---- JSON conversions of the stored rows ---------------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaPolicyV2& value);
  KernelQuotaPolicyV2 tag_invoke(const json::value_to_tag<KernelQuotaPolicyV2>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaWindowV2& value);
  KernelQuotaWindowV2 tag_invoke(const json::value_to_tag<KernelQuotaWindowV2>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaReservationRowV2& value);
  KernelQuotaReservationRowV2 tag_invoke(const json::value_to_tag<KernelQuotaReservationRowV2>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelDefaultWindowCutoverV2& value);
  KernelDefaultWindowCutoverV2 tag_invoke(const json::value_to_tag<KernelDefaultWindowCutoverV2>&, const json::value& value);
}
