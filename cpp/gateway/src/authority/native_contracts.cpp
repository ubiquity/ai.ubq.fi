#include "uos/authority/native_contracts.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"

#include <string>

namespace uos::authority::native_contracts {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyPolicy& value) {
    out = Writer()
      .required("token_hash", value.token_hash)
      .required("key_id", value.key_id)
      .required("expires_at_ms", value.expires_at_ms)
      .required("usage_limit_requests", value.usage_limit_requests)
      .required("window_ms", value.window_ms)
      .required("window_start_ms", value.window_start_ms)
      .required("usage_reset_at_ms", value.usage_reset_at_ms)
      .required("policy_version", value.policy_version)
      .required("paid_fallback_enabled", value.paid_fallback_enabled)
      .required("paid_fallback_limit_microcredits", value.paid_fallback_limit_microcredits)
      .required("paid_fallback_spent_microcredits", value.paid_fallback_spent_microcredits)
      .required("paid_fallback_reserved_microcredits", value.paid_fallback_reserved_microcredits)
      .orNull("paid_fallback_reservation_request_id", value.paid_fallback_reservation_request_id)
      .done();
  }

  ApiKeyPolicy tag_invoke(const json::value_to_tag<ApiKeyPolicy>&, const json::value& value) {
    const Reader in(value, "ApiKeyPolicy");
    return {
      .token_hash = in.required<std::string>("token_hash"),
      .key_id = in.required<std::string>("key_id"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .usage_limit_requests = in.required<double>("usage_limit_requests"),
      .window_ms = in.required<double>("window_ms"),
      .window_start_ms = in.required<double>("window_start_ms"),
      .usage_reset_at_ms = in.required<double>("usage_reset_at_ms"),
      .policy_version = in.required<std::string>("policy_version"),
      .paid_fallback_enabled = in.required<bool>("paid_fallback_enabled"),
      .paid_fallback_limit_microcredits = in.required<double>("paid_fallback_limit_microcredits"),
      .paid_fallback_spent_microcredits = in.required<double>("paid_fallback_spent_microcredits"),
      .paid_fallback_reserved_microcredits = in.required<double>("paid_fallback_reserved_microcredits"),
      .paid_fallback_reservation_request_id = in.orNull<std::string>("paid_fallback_reservation_request_id"),
    };
  }
}
