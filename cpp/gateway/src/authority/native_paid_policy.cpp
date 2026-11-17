#include "uos/authority/native_paid_policy.hpp"

#include "uos/api_keys.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace uos::authority::native_paid_policy {
  namespace {
    using api_keys::PAID_FALLBACK_NO_LIMIT;

    bool isNonNegativeSafeInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      return number && rt::Number::isSafeInteger(*number) && *number >= 0;
    }

    bool isPositiveSafeInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      return number && rt::Number::isSafeInteger(*number) && *number > 0;
    }

    // value === PAID_FALLBACK_NO_LIMIT
    bool isNoLimit(const json::value* value) { return rt::getNumber(value) == PAID_FALLBACK_NO_LIMIT; }

    bool isPaidFallbackLimit(const json::value* value) { return isNoLimit(value) || isNonNegativeSafeInteger(value); }

    bool isNullableString(const json::value* value) {
      if (value && value->is_null()) return true;
      const std::optional<std::string_view> text = rt::getString(value);
      return text && rt::length(rt::trim(*text)) > 0;
    }
  }

  bool hasStrictPaidFallbackPolicy(const json::value* value) {
    if (!utils::isRecord(value)) return false;
    const std::optional<bool> enabled = rt::getBool(*value, "paid_fallback_enabled");
    if (!enabled) return false;
    if (!isPaidFallbackLimit(rt::get(*value, "paid_fallback_limit_microcredits"))) return false;
    if (!isNonNegativeSafeInteger(rt::get(*value, "paid_fallback_spent_microcredits"))) return false;
    if (!isNonNegativeSafeInteger(rt::get(*value, "paid_fallback_reserved_microcredits"))) return false;
    if (!isNullableString(rt::get(*value, "paid_fallback_reservation_request_id"))) return false;

    const json::value* limit = rt::get(*value, "paid_fallback_limit_microcredits");
    if (*enabled && !isNoLimit(limit) && !isPositiveSafeInteger(limit)) return false;
    return true;
  }

  bool hasStrictPaidFallbackPolicy(const json::value& value) { return hasStrictPaidFallbackPolicy(&value); }

  bool hasStrictPaidFallbackKeyPolicy(const json::value* value) {
    if (!hasStrictPaidFallbackPolicy(value) || !utils::isRecord(value)) return false;
    const json::array* modelIds = rt::getArray(*value, "paid_fallback_model_ids");
    if (!modelIds) return false;
    for (const json::value& model : *modelIds) {
      if (!model.is_string() || rt::trim(std::string_view(model.get_string())).empty()) return false;
    }
    if (!isNonNegativeSafeInteger(rt::get(*value, "paid_fallback_quota_per_credit"))) return false;
    const json::value* pricingCheckedAtMs = rt::get(*value, "paid_fallback_pricing_checked_at_ms");
    if (!(pricingCheckedAtMs && pricingCheckedAtMs->is_null()) && !isPositiveSafeInteger(pricingCheckedAtMs)) {
      return false;
    }
    if (*rt::getBool(*value, "paid_fallback_enabled")) {
      if (modelIds->empty()) return false;
      if (!isPositiveSafeInteger(rt::get(*value, "paid_fallback_quota_per_credit"))) return false;
      if (!isPositiveSafeInteger(pricingCheckedAtMs)) return false;
    }
    return true;
  }

  bool hasStrictPaidFallbackKeyPolicy(const json::value& value) { return hasStrictPaidFallbackKeyPolicy(&value); }

  PaidFallbackHashFields paidFallbackHashFields(const types::ApiKeyRecord& record) {
    return {
      .paid_fallback_enabled = record.paid_fallback_enabled,
      .paid_fallback_limit_microcredits = record.paid_fallback_limit_microcredits,
      .paid_fallback_spent_microcredits = record.paid_fallback_spent_microcredits,
      .paid_fallback_reserved_microcredits = record.paid_fallback_reserved_microcredits,
      .paid_fallback_reservation_request_id = record.paid_fallback_reservation_request_id,
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackHashFields& value) {
    out = rt::record::Writer()
      .required("paid_fallback_enabled", value.paid_fallback_enabled)
      .required("paid_fallback_limit_microcredits", value.paid_fallback_limit_microcredits)
      .required("paid_fallback_spent_microcredits", value.paid_fallback_spent_microcredits)
      .required("paid_fallback_reserved_microcredits", value.paid_fallback_reserved_microcredits)
      .orNull("paid_fallback_reservation_request_id", value.paid_fallback_reservation_request_id)
      .done();
  }

  PaidFallbackHashFields tag_invoke(const json::value_to_tag<PaidFallbackHashFields>&, const json::value& value) {
    const rt::record::Reader in(value, "PaidFallbackHashFields");
    return {
      .paid_fallback_enabled = in.required<bool>("paid_fallback_enabled"),
      .paid_fallback_limit_microcredits = in.required<double>("paid_fallback_limit_microcredits"),
      .paid_fallback_spent_microcredits = in.required<double>("paid_fallback_spent_microcredits"),
      .paid_fallback_reserved_microcredits = in.required<double>("paid_fallback_reserved_microcredits"),
      .paid_fallback_reservation_request_id = in.orNull<std::string>("paid_fallback_reservation_request_id"),
    };
  }
}
