#pragma once
// Port of src/authority/native-paid-policy.ts.
// Source-owned paid policy validators and hash projection; no storage or provider I/O.
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>

namespace uos::authority::native_paid_policy {
  // `value: unknown`: nullptr is undefined. A typed record is checked as json::value_from(record).
  bool hasStrictPaidFallbackPolicy(const json::value* value);
  bool hasStrictPaidFallbackPolicy(const json::value& value);

  bool hasStrictPaidFallbackKeyPolicy(const json::value* value);
  bool hasStrictPaidFallbackKeyPolicy(const json::value& value);

  // Pick<ApiKeyHashRecord, "paid_fallback_enabled" | "paid_fallback_limit_microcredits" |
  //   "paid_fallback_spent_microcredits" | "paid_fallback_reserved_microcredits" | "paid_fallback_reservation_request_id">
  struct PaidFallbackHashFields {
    bool paid_fallback_enabled;
    double paid_fallback_limit_microcredits;
    double paid_fallback_spent_microcredits;
    double paid_fallback_reserved_microcredits;
    std::optional<std::string> paid_fallback_reservation_request_id;
    friend bool operator==(const PaidFallbackHashFields&, const PaidFallbackHashFields&) = default;
  };

  PaidFallbackHashFields paidFallbackHashFields(const types::ApiKeyRecord& record);

  // Boost.JSON conversions: members in declaration order (the TS object literal's order);
  // value_to is strict and throws rt::TypeError.
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackHashFields& value);
  PaidFallbackHashFields tag_invoke(const json::value_to_tag<PaidFallbackHashFields>&, const json::value& value);
}
