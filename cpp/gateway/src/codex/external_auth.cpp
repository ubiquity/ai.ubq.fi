#include "uos/codex/external_auth.hpp"

#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace uos::codex::external_auth {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using utils::getString;
    using utils::isRecord;

    // `value[key]` where the TS has narrowed `value` with utils.isRecord (an object or an
    // array); none of the keys read here is an array index or `length`.
    const json::value* property(const json::value* value, std::string_view key) { return value != nullptr ? rt::get(*value, key) : nullptr; }

    // `a === b` with `b` a string.
    bool isString(const json::value* value, std::string_view text) { return value != nullptr && value->is_string() && value->get_string() == text; }
  }

  std::optional<ExternalCodexIdentity> externalCodexIdentity(std::string_view token) {
    static const rt::Regex DASH("-", "g");
    static const rt::Regex UNDERSCORE("_", "g");
    const std::vector<std::string> parts = rt::split(token, ".");
    if (parts.size() != 3) return std::nullopt;
    try {
      const std::string payload = rt::replace(rt::replace(parts[1], DASH, "+"), UNDERSCORE, "/");
      const std::int64_t padding = (4 - (rt::length(payload) % 4)) % 4;
      const json::value raw = rt::jsonParse(utils::decodeBase64ToString(payload + rt::repeat("=", padding)));
      const json::value* claims = isRecord(raw) && isRecord(property(&raw, "https://api.openai.com/auth")) ? property(&raw, "https://api.openai.com/auth") : nullptr;
      const std::optional<std::string> userId = claims != nullptr ? getString(property(claims, "chatgpt_user_id")) : std::nullopt;
      const std::optional<std::string> accountId = claims != nullptr ? getString(property(claims, "chatgpt_account_id")) : std::nullopt;
      if (rt::truthy(userId) && rt::truthy(accountId)) return ExternalCodexIdentity{.userId = *userId, .accountId = *accountId};
      return std::nullopt;
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
  }

  std::optional<ExternalNativeCodexDocument> parseExternalNativeCodexDocument(const json::value* raw) {
    if (!isRecord(raw) || !isString(property(raw, "auth_mode"), "chatgptAuthTokens")) return std::nullopt;
    const json::value* apiKey = property(raw, "OPENAI_API_KEY");
    if (apiKey == nullptr || !apiKey->is_null()) return std::nullopt;
    const json::value* tokens = property(raw, "tokens");
    if (!isRecord(tokens)) return std::nullopt;
    const std::optional<std::string> accessToken = getString(property(tokens, "access_token"));
    const std::optional<std::string> accountId = getString(property(tokens, "account_id"));
    if (!rt::truthy(accessToken) || !rt::truthy(accountId) || !isString(property(tokens, "id_token"), *accessToken) ||
        !isString(property(tokens, "refresh_token"), "")) {
      return std::nullopt;
    }
    const std::optional<ExternalCodexIdentity> identity = externalCodexIdentity(*accessToken);
    if (!identity || identity->accountId != *accountId) return std::nullopt;
    const json::value* lastRefresh = property(raw, "last_refresh");
    std::optional<std::string> refreshedAt = getString(lastRefresh);
    if (!rt::isNullish(lastRefresh) && (!rt::truthy(refreshedAt) || !rt::Number::isFinite(rt::Date::parse(*refreshedAt)))) return std::nullopt;
    return ExternalNativeCodexDocument{
      .auth = {.access_token = *accessToken, .refresh_token = "", .account_id = *accountId},
      .userId = identity->userId,
      .refreshedAt = std::move(refreshedAt),
    };
  }

  std::optional<ExternalNativeCodexDocument> parseExternalNativeCodexDocument(const json::value& raw) { return parseExternalNativeCodexDocument(&raw); }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ExternalNativeCodexDocument::Auth& value) {
    out = Writer().required("access_token", value.access_token).required("refresh_token", value.refresh_token).required("account_id", value.account_id).done();
  }

  ExternalNativeCodexDocument::Auth tag_invoke(const json::value_to_tag<ExternalNativeCodexDocument::Auth>&, const json::value& value) {
    const Reader in(value, "ExternalNativeCodexDocument.auth");
    return {
      .access_token = in.required<std::string>("access_token"),
      .refresh_token = in.required<std::string>("refresh_token"),
      .account_id = in.required<std::string>("account_id"),
    };
  }
}
