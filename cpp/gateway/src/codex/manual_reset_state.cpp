#include "uos/codex/manual_reset_state.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/utils.hpp"

#include <array>
#include <string_view>

namespace uos::codex::manual_reset_state {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    const rt::Regex& HASH() {
      static const rt::Regex pattern("^[a-f0-9]{64}$", "");
      return pattern;
    }

    // `typeof value === "number" && Number.isSafeInteger(value) && value >= 0`
    bool nonNegativeSafeInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      return number.has_value() && rt::Number::isSafeInteger(*number) && *number >= 0;
    }
  }

  rt::KvKey intentKey(const std::string& accountIdHash) { return rt::KvKey{CODEX_MANUAL_RESET_PREFIX, accountIdHash}; }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ManualIntent& value) {
    out = Writer()
            .required("v", value.v)
            .required("account_id_hash", value.account_id_hash)
            .required("credential_version", value.credential_version)
            .required("quota_reset_at_ms", value.quota_reset_at_ms)
            .required("routing_generation", value.routing_generation)
            .required("created_at_ms", value.created_at_ms)
            .orNull("promoted_at_ms", value.promoted_at_ms)
            .orNull("quota_signal_at_ms", value.quota_signal_at_ms)
            .done();
  }

  ManualIntent tag_invoke(const json::value_to_tag<ManualIntent>&, const json::value& value) {
    const Reader in(value, "ManualIntent");
    return {
      .v = in.required<double>("v"),
      .account_id_hash = in.required<std::string>("account_id_hash"),
      .credential_version = in.required<std::string>("credential_version"),
      .quota_reset_at_ms = in.required<double>("quota_reset_at_ms"),
      .routing_generation = in.required<double>("routing_generation"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .promoted_at_ms = in.orNull<double>("promoted_at_ms"),
      .quota_signal_at_ms = in.orNull<double>("quota_signal_at_ms"),
    };
  }

  std::optional<ManualIntent> parseManualResetIntent(const json::value& value) {
    const json::object* record = value.if_object();
    const std::optional<std::string_view> accountIdHash = rt::getString(value, "account_id_hash");
    const std::optional<std::string_view> credentialVersion = rt::getString(value, "credential_version");
    // `isRecord` (an array is a record there) and `Object.keys(value).length !== 8`: no array passes
    // `value.v === 1`, so only an object can continue.
    if (!utils::isRecord(value) || record == nullptr || rt::getNumber(value, "v") != 1 || record->size() != 8 || !accountIdHash || !HASH().test(*accountIdHash) ||
        !credentialVersion || !HASH().test(*credentialVersion))
      return std::nullopt;
    for (const std::string_view key : std::array<std::string_view, 3>{"quota_reset_at_ms", "routing_generation", "created_at_ms"})
      if (!nonNegativeSafeInteger(rt::get(*record, key))) return std::nullopt;
    for (const std::string_view key : std::array<std::string_view, 2>{"promoted_at_ms", "quota_signal_at_ms"}) {
      const json::value* member = rt::get(*record, key);
      const bool isNull = member != nullptr && member->is_null();
      if (!isNull && !nonNegativeSafeInteger(member)) return std::nullopt;
    }
    const std::optional<double> promotedAtMs = rt::getNumber(*record, "promoted_at_ms");
    const std::optional<double> createdAtMs = rt::getNumber(*record, "created_at_ms");
    if (promotedAtMs && createdAtMs && *promotedAtMs < *createdAtMs) return std::nullopt;
    return ManualIntent{
      .v = 1,
      .account_id_hash = std::string(*accountIdHash),
      .credential_version = std::string(*credentialVersion),
      .quota_reset_at_ms = *rt::getNumber(*record, "quota_reset_at_ms"),
      .routing_generation = *rt::getNumber(*record, "routing_generation"),
      .created_at_ms = *createdAtMs,
      .promoted_at_ms = promotedAtMs,
      .quota_signal_at_ms = rt::getNumber(*record, "quota_signal_at_ms"),
    };
  }
}
